// UART2 to the MBB console through the IDF driver. The receive side never
// stops. The transmit pin is attached only while the MBB is awake, because
// pin 9 is the MBB's hibernation wake pin and a high level reboots a sleeping
// bike; when detached it is an input with the internal pull-down.
#include "mbb_uart.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static LineHandler lineHandler;
static RawHandler rawHandler;
static StateHandler stateHandler;
static volatile bool awake = false;
static volatile bool txEnabled = false;
static volatile uint32_t lastActivityMs = 0;
static volatile uint32_t lastByteMs = 0;

static void txAttach() {
    uart_set_pin(UART_NUM_2, PIN_MBB_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    txEnabled = true;
}

static void txDetach() {
    txEnabled = false;
    gpio_reset_pin((gpio_num_t)PIN_MBB_TX);
    gpio_set_direction((gpio_num_t)PIN_MBB_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_MBB_TX, GPIO_PULLDOWN_ONLY);
}

static void emit(char* line, size_t len) {
    line[len] = 0;
    if (lineHandler) lineHandler(line, len);
}

static void captureTask(void*) {
    static uint8_t buf[512];
    static char line[1024];
    size_t llen = 0;
    for (;;) {
        int n = uart_read_bytes(UART_NUM_2, buf, sizeof buf, pdMS_TO_TICKS(20));
        uint32_t now = millis();
        if (n > 0) {
            lastByteMs = now;
            lastActivityMs = now;
        } else if (gpio_get_level((gpio_num_t)PIN_MBB_RX)) {
            lastActivityMs = now;   // idle mark: the MBB's UART is powered
        }
        bool nowAwake = (now - lastActivityMs) < SLEEP_AFTER_MS;
        if (nowAwake != awake) {
            awake = nowAwake;
            if (awake) txAttach(); else txDetach();
            if (stateHandler) stateHandler(awake);
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
    uart_driver_install(UART_NUM_2, UART_RX_BUF, 1024, 0, nullptr, 0);
    txDetach();

    xTaskCreatePinnedToCore(captureTask, "mbb", 8192, nullptr, 3, nullptr, 1);
}

bool mbbAwake() { return awake; }
bool mbbTxEnabled() { return txEnabled; }
uint32_t mbbLastByteMs() { return lastByteMs; }

size_t mbbWrite(const uint8_t* data, size_t len) {
    if (!txEnabled) return 0;
    int n = uart_write_bytes(UART_NUM_2, (const char*)data, len);
    return n < 0 ? 0 : (size_t)n;
}
