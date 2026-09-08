// The console owner: the MBB's bytes to whoever is listening on port 6638,
// and their keystrokes to the MBB through the transmit gate. Everything
// here runs on the loop task except consolePushRaw, from the capture task.
#include "console.h"
#include "config.h"
#include "mbb_uart.h"
#include "wlan.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include <atomic>

static WiFiServer console(CONSOLE_PORT);
static WiFiClient clients[CONSOLE_CLIENTS];
static StreamBufferHandle_t rawBuf;
static std::atomic<uint32_t> rawDropped{0};   // added on the capture task, taken here
static bool up = false;
static size_t inputCursor = 0;

// Per console client: CR-LF state, output that did not fit its socket yet,
// and the last time it was told the MBB is asleep.
struct ConsoleState {
    uint8_t prev = 0;
    uint8_t pend[512];
    size_t pendLen = 0;
    uint32_t lost = 0;
    uint32_t asleepNoteMs = 0;
};
static ConsoleState cstate[CONSOLE_CLIENTS];

int consoleClients() { int n = 0; for (auto& c : clients) if (c && c.connected()) n++; return n; }
String consoleStatusJson() { return "{\"clients\":" + String(consoleClients()) + ",\"dropped_bytes\":" + String(rawDropped.load()) + "}"; }

void consoleStart() {
    if (up) return;
    up = true;
    console.begin();
    console.setNoDelay(true);
}

void consoleStop() {
    if (!up) return;
    up = false;
    for (auto& c : clients) if (c) c.stop();
    console.end();
}

void consolePrepare() {
    rawBuf = xStreamBufferCreate(4096, 1);   // a few hundred milliseconds of the console at full rate; the loop drains it every 2 ms
    if (!rawBuf) Serial.println("console: no memory for the console buffer; console output off");
}

void consolePushRaw(const uint8_t* data, size_t len) {
    if (!rawBuf) return;
    uint8_t tmp[256];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0) continue;
        tmp[k++] = data[i];
        if (k == sizeof tmp) { rawDropped.fetch_add(k - xStreamBufferSend(rawBuf, tmp, k, 0)); k = 0; }
    }
    if (k) rawDropped.fetch_add(k - xStreamBufferSend(rawBuf, tmp, k, 0));
}

static void consoleSend(size_t ci, const uint8_t* data, size_t len) {
    WiFiClient& slot = clients[ci];
    ConsoleState& st = cstate[ci];
    // First whatever is still pending from last time.
    if (st.pendLen) {
        int sent = ::send(slot.fd(), st.pend, st.pendLen, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) slot.stop();
            st.lost += len;
            return;
        }
        if ((size_t)sent < st.pendLen) {
            memmove(st.pend, st.pend + sent, st.pendLen - sent);
            st.pendLen -= sent;
            st.lost += len;
            return;
        }
        st.pendLen = 0;
        if (st.lost) {   // the client is taking data again: say what it missed
            st.pendLen = snprintf((char*)st.pend, sizeof st.pend,
                                  "\n[dongle: %lu console bytes dropped]\n", (unsigned long)st.lost);
            st.lost = 0;
            consoleSend(ci, data, len);
            return;
        }
    }
    if (len == 0) return;
    int sent = ::send(slot.fd(), data, len, MSG_DONTWAIT);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) { slot.stop(); return; }
        sent = 0;
    }
    if ((size_t)sent < len) {   // keep the rest for the next pass
        size_t rest = len - sent;
        if (rest > sizeof st.pend) { st.lost += rest - sizeof st.pend; rest = sizeof st.pend; }
        memcpy(st.pend, data + sent, rest);
        st.pendLen = rest;
    }
}

// connected() misreads a clean close as still connected while nothing is
// being sent; a peek tells the truth.
static bool peerGone(WiFiClient& c) {
    char dummy;
    int r = recv(c.fd(), &dummy, 1, MSG_DONTWAIT | MSG_PEEK);
    return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

// With nobody listening the stream is thrown away as it arrives, so the
// first client sees live output and not hours of backlog with a drop marker.
static void discardRaw() {
    uint8_t junk[256];
    while (xStreamBufferReceive(rawBuf, junk, sizeof junk, 0) > 0) {}
    rawDropped.exchange(0);
}

void consoleTick() {
    if (!rawBuf) return;
    if (!up) { discardRaw(); return; }
    for (auto& c : clients) if (c && c.connected() && peerGone(c)) c.stop();
    if (console.hasClient()) {
        WiFiClient c = console.accept();
        bool placed = false;
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
            WiFiClient& slot = clients[ci];
            if (slot && slot.connected()) continue;
            slot = c;
            slot.setNoDelay(true);
            // A peer that vanishes without a FIN still looks connected; keepalive finds out.
            int one = 1, idle = 45, interval = 15, count = 3;
            slot.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
            slot.setOption(TCP_KEEPIDLE, &idle);
            slot.setOption(TCP_KEEPINTVL, &interval);
            slot.setOption(TCP_KEEPCNT, &count);
            cstate[ci] = ConsoleState();
            slot.printf("%s console. MBB %s. Enter twice for the prompt.\n", wifiName(),
                        mbbAwake() ? "awake" : "asleep, input dropped until it wakes");
            placed = true;
            break;
        }
        if (!placed) { c.print("all console slots are in use\n"); c.stop(); }
    }
    if (consoleClients() == 0) { discardRaw(); return; }
    // Output: an upstream drop marker if any, backlogs, then whatever the MBB said.
    uint32_t d = rawDropped.exchange(0);
    if (d) {
        char msg[64];
        int n = snprintf(msg, sizeof msg, "\n[dongle: %lu console bytes dropped upstream]\n", (unsigned long)d);
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++)
            if (clients[ci] && clients[ci].connected()) consoleSend(ci, (const uint8_t*)msg, n);
    }
    for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
        if (clients[ci] && clients[ci].connected() && cstate[ci].pendLen) consoleSend(ci, nullptr, 0);
    }
    uint8_t buf[512];
    size_t n;
    while ((n = xStreamBufferReceive(rawBuf, buf, sizeof buf, 0)) > 0) {
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
            if (clients[ci] && clients[ci].connected()) consoleSend(ci, buf, n);
        }
    }
    // Input: one client per pass, starting after the one served last time.
    for (size_t k = 0; k < CONSOLE_CLIENTS; k++) {
        size_t ci = (inputCursor + k) % CONSOLE_CLIENTS;
        WiFiClient& slot = clients[ci];
        if (!slot || !slot.connected() || !slot.available()) continue;
        uint8_t in[128], out[256];   // every byte can become two; sizes must keep that ratio
        int got = slot.read(in, sizeof in);
        size_t w = 0;
        uint8_t prev = cstate[ci].prev;
        for (int i = 0; i < got && w + 2 <= sizeof out; i++) {
            uint8_t b = in[i];
            if (b == 0x7f) b = 0x08;                            // delete to backspace
            if (b == '\n' && prev != '\r') out[w++] = '\r';     // the MBB wants CR LF
            out[w++] = b;
            prev = b;
        }
        cstate[ci].prev = prev;
        if (mbbAwake()) {
            size_t sent = mbbWrite(out, w);
            if (sent < w) slot.printf("(dongle: %u byte(s) of input not sent)\n", (unsigned)(w - sent));
        } else if (millis() - cstate[ci].asleepNoteMs > 1000) {
            cstate[ci].asleepNoteMs = millis();
            slot.print("(MBB asleep, input dropped)\n");
        }
        inputCursor = ci + 1;
        break;
    }
}
