// An in-memory filesystem, so the saved batch can be round-tripped on the host.
#pragma once
#include <Arduino.h>
#include <map>
#include <string>

#define FILE_WRITE "w"

struct FsFile {
    std::string* data = nullptr;
    size_t pos = 0;
    bool ok = false;
    int err = 0;
    explicit operator bool() const { return ok; }
    size_t print(const String& s) { if (data) data->append(s.s); return s.length(); }
    size_t printf(const char* fmt, ...) {
        char b[256]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
        if (data && n > 0) data->append(b, n);
        return n > 0 ? n : 0;
    }
    bool available() const { return data && pos < data->size(); }
    String readStringUntil(char t) {
        std::string out;
        while (data && pos < data->size() && (*data)[pos] != t) out += (*data)[pos++];
        if (data && pos < data->size()) pos++;
        return String(out);
    }
    size_t readBytes(char* dst, size_t n) {
        size_t got = 0;
        while (data && pos < data->size() && got < n) dst[got++] = (*data)[pos++];
        return got;
    }
    int getWriteError() const { return err; }
    void close() {}
};

struct LittleFSClass {
    std::map<std::string, std::string> files;
    bool full = false;   // a test can make writes fail
    FsFile open(const String& path, const char* mode = "r") {
        FsFile f;
        std::string p = path.s;
        if (mode && mode[0] == 'w') {
            files[p] = "";
            f.data = &files[p]; f.ok = true; f.err = full ? 1 : 0;
        } else {
            auto it = files.find(p);
            if (it == files.end()) return f;
            f.data = &it->second; f.ok = true;
        }
        return f;
    }
    bool remove(const String& path) { return files.erase(path.s) > 0; }
    bool rename(const String& a, const String& b) {
        auto it = files.find(a.s);
        if (it == files.end()) return false;
        files[b.s] = it->second; files.erase(it); return true;
    }
    bool exists(const String& path) { return files.count(path.s) > 0; }
};
extern LittleFSClass LittleFS;
