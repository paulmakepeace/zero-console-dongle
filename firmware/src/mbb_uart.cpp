// UART2 to the MBB console through the IDF driver. The receive side never
// stops. The transmit pin is attached to the UART only while something is
// being sent, and only while the MBB is awake, then returns to an input with
// the internal pull-down. Pin 9 is the MBB's hibernation wake pin: a high
// level reboots a sleeping bike, and a UART idles high, so leaving TX
// attached would hold the MBB out of deep sleep for as long as the dongle
// is powered.
#include "mbb_uart.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static LineHandler lineHandler;
static RawHandler rawHandler;
static StateHandler stateHandler;
static volatile bool awake = false;
static volatile bool txAttached = false;
static volatile uint32_t lastActivityMs = 0;
static volatile uint32_t lastByteMs = 0;
static volatile uint32_t txHoldUntilMs = 0;
static volatile uint32_t overflows = 0;
static SemaphoreHandle_t txMtx;
static QueueHandle_t uartQueue;

static void inputPulldown(int pin) {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_ENABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&c);   // also disconnects any matrix output from the pin
}

void mbbPinsSafe() {
    inputPulldown(PIN_MBB_TX);
    inputPulldown(PIN_MBB_RX);
}

// Callers hold txMtx.
static void txAttach() {
    if (txAttached) return;
    uart_set_pin(UART_NUM_2, PIN_MBB_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    txAttached = true;
}

static void txDetach() {
    if (!txAttached) return;
    // Let the software ring buffer drain into the FIFO, then the FIFO onto the wire.
    size_t freeBytes = 0;
    for (int i = 0; i < 200; i++) {
        if (uart_get_tx_buffer_free_size(UART_NUM_2, &freeBytes) != ESP_OK || freeBytes >= UART_TX_BUF) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(200));
    inputPulldown(PIN_MBB_TX);
    txAttached = false;
}

static void emit(char* line, size_t len) {
    line[len] = 0;
    if (lineHandler) lineHandler(line, len);
}

static void drainEvents() {
    uart_event_t ev;
    while (uartQueue && xQueueReceive(uartQueue, &ev, 0) == pdTRUE) {
        if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            overflows = overflows + 1;
            static char msg[64];
            snprintf(msg, sizeof msg, "dongle: UART overflow %lu, bytes lost", (unsigned long)overflows);
            emit(msg, strlen(msg));
        }
    }
}

static void captureTask(void*) {
    static uint8_t buf[512];
    static char line[1024];
    size_t llen = 0;
    int highRun = 0;
    for (;;) {
        int n = uart_read_bytes(UART_NUM_2, buf, sizeof buf, pdMS_TO_TICKS(20));
        uint32_t now = millis();
        drainEvents();
        bool realBytes = false;
        for (int i = 0; i < n; i++) if (buf[i] != 0) { realBytes = true; break; }
        if (n > 0) lastByteMs = now;
        if (realBytes) {
            lastActivityMs = now;   // a lone NUL is a break or noise, not the MBB talking
            highRun = 0;
        } else if (gpio_get_level((gpio_num_t)PIN_MBB_RX)) {
            if (++highRun >= AWAKE_SAMPLES) lastActivityMs = now;   // sustained idle mark
        } else {
            highRun = 0;
        }
        bool nowAwake = (now - lastActivityMs) < SLEEP_AFTER_MS;
        if (nowAwake != awake) {
            xSemaphoreTake(txMtx, portMAX_DELAY);
            awake = nowAwake;
            if (!awake) txDetach();
            xSemaphoreGive(txMtx);
            if (stateHandler) stateHandler(awake);
        }
        if (txAttached && (int32_t)(now - txHoldUntilMs) > 0) {
            xSemaphoreTake(txMtx, portMAX_DELAY);
            if (txAttached && (int32_t)(millis() - txHoldUntilMs) > 0) txDetach();
            xSemaphoreGive(txMtx);
        }
        if (n > 0) {
            if (rawHandler) rawHandler(buf, n);
            for (int i = 0; i < n; i++) {
                uint8_t b = buf[i];
                if (b == 0 || b == '\r') continue;
                if (b == '\n') { emit(line, llen); llen = 0; continue; }
                if (llen >= sizeof line - 1) { emit(line, llen); llen = 0; }
                line[llen++] = b;
            }
        } else if (llen && (now - lastByteMs) > IDLE_FLUSH_MS) {
            emit(line, llen);   // the prompt, or anything else without a newline
            llen = 0;
        }
    }
}

void mbbBegin(LineHandler onLine, RawHandler onRaw, StateHandler onState) {
    lineHandler = onLine;
    rawHandler = onRaw;
    stateHandler = onState;
    txMtx = xSemaphoreCreateMutex();

    uart_config_t cfg = {};
    cfg.baud_rate = MBB_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;
    uart_param_config(UART_NUM_2, &cfg);
    uart_set_pin(UART_NUM_2, UART_PIN_NO_CHANGE, PIN_MBB_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_pull_mode((gpio_num_t)PIN_MBB_RX, GPIO_PULLDOWN_ONLY);   // uart_set_pin leaves a pull-up
    uart_driver_install(UART_NUM_2, UART_RX_BUF, UART_TX_BUF, UART_EVENT_QUEUE, &uartQueue, 0);
    lastActivityMs = millis() - SLEEP_AFTER_MS - 1;   // start asleep until pin 8 is seen high

    xTaskCreatePinnedToCore(captureTask, "mbb", 8192, nullptr, 3, nullptr, 1);
}

bool mbbAwake() { return awake; }
bool mbbTxAttached() { return txAttached; }
uint32_t mbbOverflows() { return overflows; }

size_t mbbWrite(const uint8_t* data, size_t len) {
    if (!awake || len == 0) return 0;
    xSemaphoreTake(txMtx, portMAX_DELAY);
    if (!awake) {   // the asleep edge may have landed between the check above and the lock
        xSemaphoreGive(txMtx);
        return 0;
    }
    txHoldUntilMs = millis() + TX_HOLD_MS;
    txAttach();
    int n = uart_write_bytes(UART_NUM_2, (const char*)data, len);
    xSemaphoreGive(txMtx);
    return n < 0 ? 0 : (size_t)n;
}
