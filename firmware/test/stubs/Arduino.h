// Enough Arduino for the host to compile and test the stateful modules.
// The clock is ours: tests step it rather than sleeping.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

extern uint32_t testMillis;   // the test owns time
inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }

struct String {
    std::string s;
    String() {}
    String(const char* p) : s(p ? p : "") {}
    String(const std::string& x) : s(x) {}
    explicit String(int v) { char b[24]; snprintf(b, sizeof b, "%d", v); s = b; }
    explicit String(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); s = b; }
    explicit String(unsigned v) { char b[24]; snprintf(b, sizeof b, "%u", v); s = b; }
    explicit String(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); s = b; }
    explicit String(float v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, (double)v); s = b; }
    const char* c_str() const { return s.c_str(); }
    size_t length() const { return s.size(); }
    void reserve(size_t n) { s.reserve(n); }
    void concat(const char* p, size_t n) { s.append(p, n); }
    bool endsWith(const char* p) const { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
    bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
    String substring(unsigned from, unsigned to) const {
        if (from > s.size()) from = s.size();
        if (to > s.size()) to = s.size();
        return String(from < to ? s.substr(from, to - from) : std::string());
    }
    String substring(unsigned from) const { return substring(from, s.size()); }
    int indexOf(char c) const { size_t p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
    String& operator+=(const char* p) { s += p; return *this; }
    String& operator+=(char c) { s += c; return *this; }
    String& operator+=(const String& o) { s += o.s; return *this; }
    bool operator==(const char* p) const { return s == p; }
    bool operator==(const String& o) const { return s == o.s; }
    bool operator!=(const String& o) const { return s != o.s; }
    operator const char*() const { return s.c_str(); }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }

// Arduino defines these as macros; the host does not.
template <class T> inline T min(T a, T b) { return a < b ? a : b; }
template <class T> inline T max(T a, T b) { return a > b ? a : b; }

#ifndef strlcpy
inline size_t strlcpy(char* d, const char* s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
#endif

// The USB console, swallowed: the tests assert on state, not on prints.
struct SerialStub {
    int printf(const char*, ...) { return 0; }
    void println(const char*) {}
    void println() {}
    void print(const char*) {}
    void flush() {}
};
inline SerialStub Serial;

// FreeRTOS's recursive mutex on one thread: a handle that is never null, so
// the store's Lock takes the same path it takes on the board.
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffUL
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { static int m; return &m; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, unsigned long) { return 1; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return 1; }
