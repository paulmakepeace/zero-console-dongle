// NVS on the host: one map for the whole run, so a value saved by one "boot"
// is there for the next, as on the board. Preferences::wipe() clears it.
#pragma once
#include <Arduino.h>
#include <map>
#include <string>

class Preferences {
    static std::map<std::string, std::string>& nvs() { static std::map<std::string, std::string> m; return m; }
    bool ro = false;
    template <class T> size_t put(const char* k, const T& v) {
        if (ro) return 0;
        nvs()[k] = std::string((const char*)&v, sizeof v);
        return sizeof v;
    }
    template <class T> T get(const char* k, T def) const {
        auto it = nvs().find(k);
        if (it == nvs().end() || it->second.size() != sizeof(T)) return def;
        T v; memcpy(&v, it->second.data(), sizeof v); return v;
    }
public:
    static void wipe() { nvs().clear(); }
    bool begin(const char*, bool readOnly = false) { ro = readOnly; return true; }
    void end() {}
    bool isKey(const char* k) const { return nvs().count(k) > 0; }
    uint32_t getUInt(const char* k, uint32_t def = 0) const { return get<uint32_t>(k, def); }
    size_t putUInt(const char* k, uint32_t v) { return put(k, v); }
    bool getBool(const char* k, bool def = false) const { return get<bool>(k, def); }
    size_t putBool(const char* k, bool v) { return put(k, v); }
    long getLong(const char* k, long def = 0) const { return get<long>(k, def); }
    size_t putLong(const char* k, long v) { return put(k, v); }
    String getString(const char* k, const String& def = String()) const { auto it = nvs().find(k); return it == nvs().end() ? def : String(it->second); }
    size_t putString(const char* k, const String& v) { if (ro) return 0; nvs()[k] = v.s; return v.length(); }
};
