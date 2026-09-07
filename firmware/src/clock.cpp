// Wall time without a battery: NTP when WiFi is up, otherwise the MBB's own
// stamps, which it prints on most lines as MM/DD/YYYY hh:mm:ss.mmm.
#include "clock.h"
#include "config.h"
#include <sys/time.h>
#include <time.h>
#include "esp_sntp.h"

static volatile TimeSource source = TIME_NONE;

static void onSntpSync(struct timeval*) { source = TIME_NTP; }

void clockBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_sntp_set_time_sync_notification_cb(onSntpSync);
#else
    sntp_set_time_sync_notification_cb(onSntpSync);
#endif
    configTzTime(TZ_DEFAULT, NTP_SERVER);
}

bool clockValid() { return source != TIME_NONE; }
TimeSource clockSource() { return source; }

const char* clockSourceName() {
    switch (source) {
        case TIME_NTP: return "ntp";
        case TIME_MBB: return "mbb";
        default: return "none";
    }
}

String clockStamp() {
    char buf[40];
    if (source == TIME_NONE) {
        uint32_t ms = millis();
        snprintf(buf, sizeof buf, "u%06lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
        return String(buf);
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    size_t n = strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + n, sizeof buf - n, ".%03ld", (long)(tv.tv_usec / 1000));
    return String(buf);
}

static int num(const char* p, int n) {
    int v = 0;
    while (n--) v = v * 10 + (*p++ - '0');
    return v;
}

static bool parseMbbTime(const char* s, size_t len, struct timeval* out) {
    static const char pat[] = "dd/dd/dddd dd:dd:dd.ddd";
    const size_t plen = sizeof(pat) - 1;
    for (size_t i = 0; i + plen <= len; i++) {
        bool ok = true;
        for (size_t j = 0; j < plen && ok; j++) {
            char c = s[i + j];
            ok = (pat[j] == 'd') ? (c >= '0' && c <= '9') : (c == pat[j]);
        }
        if (!ok) continue;
        const char* p = s + i;
        struct tm tm = {};
        tm.tm_mon = num(p, 2) - 1;
        tm.tm_mday = num(p + 3, 2);
        tm.tm_year = num(p + 6, 4) - 1900;
        tm.tm_hour = num(p + 11, 2);
        tm.tm_min = num(p + 14, 2);
        tm.tm_sec = num(p + 17, 2);
        tm.tm_isdst = -1;
        if (tm.tm_year < 2024 - 1900 || tm.tm_mon < 0 || tm.tm_mon > 11) continue;
        time_t t = mktime(&tm);
        if (t < 1700000000) continue;
        out->tv_sec = t;
        out->tv_usec = num(p + 20, 3) * 1000;
        return true;
    }
    return false;
}

void clockMaybeSetFromMbb(const char* line, size_t len) {
    if (source == TIME_NTP) return;
    struct timeval tv;
    if (!parseMbbTime(line, len, &tv)) return;
    if (source == TIME_MBB) {
        struct timeval now;
        gettimeofday(&now, nullptr);
        long diff = (long)(now.tv_sec - tv.tv_sec);
        if (diff > -5 && diff < 5) return;
    }
    settimeofday(&tv, nullptr);
    source = TIME_MBB;
}
