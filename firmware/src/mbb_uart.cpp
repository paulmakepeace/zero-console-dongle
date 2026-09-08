// UART2 to the MBB console through the IDF driver. The capture task does
// nothing but read bytes, frame lines and watch the pins: it never takes a
// mutex and never touches the flash, so it cannot be stalled into a false
// sleep and cannot hold the UART interrupt off. Lines, loss markers and the
// awake and asleep edges go through a queue, in order, to the loop task.
//
// The transmit pin is attached to the UART only while something is being
// sent and for a short hold after, and only while the MBB is awake; the rest
// of the time it is an input with the internal pull-down. Pin 9 is the MBB's
// hibernation wake pin: a high level reboots a sleeping bike, a UART idles
// high, and a held-high pin 9 keeps an awake MBB out of deep sleep.
#include "mbb_uart.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "freertos/stream_buffer.h"
#include <atomic>
#include "pure/framer.h"

static RawHandler rawHandler;
static volatile bool awake = false;
static volatile bool lineHigh = false;   // pin 8 right now, for the transmit gate; awake lags it by 5 s
static volatile bool txAttached = false;
static volatile bool driverOk = false;
static volatile uint32_t lastActivityMs = 0;
static volatile uint32_t lastByteMs = 0;
static volatile uint32_t txHoldUntilMs = 0;
static volatile bool txHeld = false;   // a batch in progress: the timed hold does not end it
static volatile uint32_t overflows = 0, backpressure = 0, frameErrors = 0;
static std::atomic<uint32_t> queueDrops{0};   // added on the capture task, taken on the loop task
static TaskHandle_t captureHandle;
static SemaphoreHandle_t txMtx;
static QueueHandle_t uartQueue;
static StreamBufferHandle_t events;   // records: type, length (2 bytes), payload
static size_t txFreeWhenEmpty = 0;

enum { EV_LINE = 1, EV_MARK = 2, EV_AWAKE = 3, EV_ASLEEP = 4 };

static bool inputPulldown(int pin) {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_ENABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&c) == ESP_OK;   // selects the GPIO function, so the UART no longer drives the pad
}

void mbbPinsSafe() {
    inputPulldown(PIN_MBB_TX);
    inputPulldown(PIN_MBB_RX);
}

// Callers hold txMtx.
static void txAttach() {
    if (txAttached) return;
    uart_set_pin(UART_NUM_2, PIN_MBB_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_pull_mode((gpio_num_t)PIN_MBB_TX, GPIO_FLOATING);   // no pull-down fighting the driver
    txAttached = true;
}

static void txDetach() {
    if (!txAttached) return;
    // Let the software ring buffer drain into the FIFO, then the FIFO onto the wire.
    size_t freeBytes = 0;
    for (int i = 0; i < 200; i++) {
        if (uart_get_tx_buffer_free_size(UART_NUM_2, &freeBytes) != ESP_OK || freeBytes >= txFreeWhenEmpty) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(200)) != ESP_OK)
        uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(200));   // once more rather than cut a byte in half
    if (inputPulldown(PIN_MBB_TX)) txAttached = false;   // on failure stay attached and try again next pass
}

// The hold ends on time, or the moment pin 8 is seen low: a MBB that is
// powering down must not find pin 9 driven.
static bool holdOver() { return (!txHeld && (int32_t)(millis() - txHoldUntilMs) > 0) || !lineHigh; }

static void checkHold() {
    if (txAttached && holdOver()) {
        xSemaphoreTake(txMtx, portMAX_DELAY);
        if (txAttached && holdOver()) txDetach();
        xSemaphoreGive(txMtx);
    }
}

static void post(uint8_t type, const char* payload, size_t len) {
    uint8_t hdr[3] = {type, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
    // Lines leave headroom so the awake and asleep edges, which carry no
    // payload, are never the records that get dropped.
    size_t need = 3 + len + (len ? 16 : 0);
    if (xStreamBufferSpacesAvailable(events) < need) { if (len) queueDrops.fetch_add(1); return; }
    xStreamBufferSend(events, hdr, 3, 0);
    if (len) xStreamBufferSend(events, payload, len, 0);
}

static void postMark(const char* fmt, uint32_t n) {
    char msg[64];
    int len = snprintf(msg, sizeof msg, fmt, (unsigned long)n);
    post(EV_MARK, msg, len);
    if (rawHandler) { rawHandler((const uint8_t*)"\n", 1); rawHandler((const uint8_t*)msg, len); rawHandler((const uint8_t*)"\n", 1); }
}

static void drainEvents() {
    uart_event_t ev;
    while (uartQueue && xQueueReceive(uartQueue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
            case UART_FIFO_OVF: overflows = overflows + 1; postMark("dongle: UART overflow %lu, bytes lost", overflows); break;
            case UART_BUFFER_FULL: backpressure = backpressure + 1; break;   // the driver stashes and re-delivers; nothing lost
            case UART_FRAME_ERR: frameErrors = frameErrors + 1; postMark("dongle: UART frame error %lu, a line above may be corrupt", frameErrors); break;
            default: break;   // data, break at the MBB's sleep, parity
        }
    }
}

static void captureTask(void*) {
    static uint8_t buf[512];
    static LineFramer<1024> framer;
    auto postLine = [](const char* l, size_t n) { post(EV_LINE, l, n); };
    uint32_t highSinceMs = 0;
    int lowSamples = 3;
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        // Read what is there, or wait 20 ms for one byte; never sit inside the
        // driver collecting a slow trickle while the pins go unwatched.
        size_t avail = 0;
        uart_get_buffered_data_len(UART_NUM_2, &avail);
        int n = uart_read_bytes(UART_NUM_2, buf, avail ? (avail < sizeof buf ? avail : sizeof buf) : 1, pdMS_TO_TICKS(20));
        uint32_t now = millis();
        bool level = gpio_get_level((gpio_num_t)PIN_MBB_RX);
        bool realBytes = false;
        for (int i = 0; i < n; i++) if (buf[i] != 0) { realBytes = true; break; }
        if (realBytes) lastByteMs = now;   // NUL-only reads are a break or noise, not talk
        // Awake means the MBB's console block is powered: real bytes with the
        // line idling high behind them, or a sustained idle mark with nothing
        // arriving. A lone byte on a dead line is noise.
        if (realBytes && level) {
            lastActivityMs = now;
            highSinceMs = 0;
        } else if (level && !realBytes) {
            if (highSinceMs == 0) highSinceMs = now ? now : 1;
            else if (now - highSinceMs >= AWAKE_HIGH_MS) lastActivityMs = now;
        } else {
            highSinceMs = 0;
        }
        // Bytes mean the line is powered even when a sample lands in a low bit;
        // three quiet low samples (about 60 ms) mean it is not.
        if (realBytes || level) { lowSamples = 0; lineHigh = true; }
        else if (lowSamples < 3 && ++lowSamples == 3) lineHigh = false;
        bool nowAwake = (now - lastActivityMs) < SLEEP_AFTER_MS;
        if (nowAwake != awake) {
            if (!nowAwake) framer.flush(postLine);   // a session keeps its own tail
            xSemaphoreTake(txMtx, portMAX_DELAY);
            awake = nowAwake;
            if (!awake) txDetach();
            xSemaphoreGive(txMtx);
            post(nowAwake ? EV_AWAKE : EV_ASLEEP, nullptr, 0);
        }
        checkHold();
        if (n > 0) {
            if (rawHandler) rawHandler(buf, n);
            framer.feed(buf, n, postLine);
        }
        if (framer.len && (now - lastByteMs) > IDLE_FLUSH_MS) framer.flush(postLine);   // the prompt, or anything else without a newline
        drainEvents();   // markers land after the bytes they interrupted
    }
}

static void shutdownPinsSafe() { mbbPinsSafe(); }

bool mbbBegin(RawHandler onRaw) {
    rawHandler = onRaw;
    txMtx = xSemaphoreCreateMutex();
    events = xStreamBufferCreate(EVENT_BUF, 1);
    if (!txMtx || !events) { Serial.println("mbb: no memory for the capture queue"); return false; }
    esp_register_shutdown_handler(shutdownPinsSafe);   // a restart must not leave pin 9 driven

    uart_config_t cfg = {};
    cfg.baud_rate = MBB_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;
    esp_err_t e = uart_param_config(UART_NUM_2, &cfg);
    if (e == ESP_OK) e = uart_driver_install(UART_NUM_2, UART_RX_BUF, UART_TX_BUF, UART_EVENT_QUEUE, &uartQueue, 0);
    if (e == ESP_OK) e = uart_set_pin(UART_NUM_2, UART_PIN_NO_CHANGE, PIN_MBB_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) { Serial.printf("mbb: UART driver failed: %s\n", esp_err_to_name(e)); return false; }
    gpio_set_pull_mode((gpio_num_t)PIN_MBB_RX, GPIO_PULLDOWN_ONLY);   // a dead MBB output reads low
    uart_flush_input(UART_NUM_2);   // whatever arrived while the pad was routed but unread
    uart_get_tx_buffer_free_size(UART_NUM_2, &txFreeWhenEmpty);
    lastActivityMs = millis() - SLEEP_AFTER_MS - 1;   // start asleep until pin 8 is seen high
    if (xTaskCreatePinnedToCore(captureTask, "mbb", 6144, nullptr, 3, &captureHandle, 1) != pdPASS) {
        Serial.println("mbb: capture task not created");
        return false;
    }
    driverOk = true;
    return true;
}

void mbbTick(LineHandler onLine, StateHandler onState) {
    checkHold();   // a second place that drops the pin, so a busy capture task is not the only one
    if (!events) return;
    static char payload[1100];
    uint8_t hdr[3];
    while (xStreamBufferBytesAvailable(events) >= 3) {
        xStreamBufferReceive(events, hdr, 3, 0);
        size_t len = hdr[1] | (hdr[2] << 8);
        size_t got = 0;
        while (got < len) got += xStreamBufferReceive(events, payload + got, len - got, pdMS_TO_TICKS(5));
        payload[len] = 0;
        switch (hdr[0]) {
            case EV_LINE: case EV_MARK: if (onLine) onLine(payload, len); break;
            case EV_AWAKE: if (onState) onState(true); break;
            case EV_ASLEEP: if (onState) onState(false); break;
        }
    }
    uint32_t d = queueDrops.exchange(0);
    if (d) {   // said once the queue has room again
        char msg[64];
        int n = snprintf(msg, sizeof msg, "dongle: %lu line(s) lost, capture queue full", (unsigned long)d);
        if (onLine) onLine(msg, n);
    }
}

bool mbbOk() { return driverOk; }
bool mbbAwake() { return awake; }
bool mbbTxAttached() { return txAttached; }
uint32_t mbbLastByteMs() { return lastByteMs; }
uint32_t mbbOverflows() { return overflows; }
uint32_t mbbBackpressure() { return backpressure; }
uint32_t mbbFrameErrors() { return frameErrors; }
uint32_t mbbQueueDrops() { return queueDrops.load(); }
bool mbbLineHigh() { return lineHigh; }

void mbbTxHold(bool on) {
    txHeld = on;
    if (!on) txHoldUntilMs = millis() + TX_HOLD_MS;   // the normal hold runs out from here
}
uint32_t mbbCaptureStackFree() { return captureHandle ? uxTaskGetStackHighWaterMark(captureHandle) : 0; }

size_t mbbWrite(const uint8_t* data, size_t len) {
    if (!driverOk || !awake || !lineHigh || len == 0) return 0;
    if (len > UART_TX_BUF) len = UART_TX_BUF;   // never block on the wire with the mutex held
    xSemaphoreTake(txMtx, portMAX_DELAY);
    if (!awake || !lineHigh) {   // the edge may have landed between the check above and the lock
        xSemaphoreGive(txMtx);
        return 0;
    }
    txHoldUntilMs = millis() + TX_HOLD_MS;   // provisional, so the hold cannot expire mid-write
    txAttach();
    int n = uart_write_bytes(UART_NUM_2, (const char*)data, len);
    txHoldUntilMs = millis() + TX_HOLD_MS;   // the hold counts from the last byte queued
    xSemaphoreGive(txMtx);
    return n < 0 ? 0 : (size_t)n;
}
