// A LittleFS stand-in backed by a temporary directory on the host, so the
// store's directory walk runs the real opendir, readdir and stat it runs on
// the board. Space is counted the way LittleFS counts it: a file up to the
// inline size costs no data block, a larger one whole 4 KB blocks, every
// directory a metadata pair, and totalBytes is whatever the test says the
// partition is. A write fails when the test says the flash is full.
#pragma once
#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_READ  "r"
#define FILE_WRITE "w"

struct FsFile {
    std::shared_ptr<FILE> fp;
    std::string path;
    bool ok = false;
    int err = 0;
    bool full = false;
    explicit operator bool() const { return ok; }
    size_t write(const uint8_t* b, size_t n) { if (!fp || full) { err = 1; return 0; } return fwrite(b, 1, n, fp.get()); }
    size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
    size_t printf(const char* fmt, ...) {
        char b[256]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
        return n > 0 ? write((const uint8_t*)b, (size_t)n) : 0;
    }
    size_t read(uint8_t* b, size_t n) { return fp ? fread(b, 1, n, fp.get()) : 0; }
    size_t readBytes(char* b, size_t n) { return read((uint8_t*)b, n); }
    bool available() { if (!fp) return false; int c = fgetc(fp.get()); if (c == EOF) return false; ungetc(c, fp.get()); return true; }
    String readStringUntil(char t) {
        std::string out; int c;
        while (fp && (c = fgetc(fp.get())) != EOF) { if (c == t) break; out += (char)c; }
        return String(out);
    }
    void flush() { if (fp) fflush(fp.get()); }
    size_t size() { flush(); struct stat st; return stat(path.c_str(), &st) == 0 ? (size_t)st.st_size : 0; }
    int getWriteError() const { return err; }
    void close() { fp.reset(); ok = false; }
};

struct LittleFSClass {
    std::string root;
    size_t total = 917504;   // the board's log partition
    bool full = false;       // a test can make writes fail
    static const size_t BLOCK = 4096, INLINE_MAX = 512;

    LittleFSClass() { char t[] = "/tmp/dongle-fs-XXXXXX"; root = mkdtemp(t); }
    ~LittleFSClass() { clear(); rmdir(root.c_str()); }
    std::string real(const String& p) const { return root + p.c_str(); }
    const char* mountpoint() const { return root.c_str(); }
    bool begin(bool = false) { return true; }
    bool exists(const String& p) { struct stat st; return stat(real(p).c_str(), &st) == 0; }
    bool mkdir(const String& p) { return ::mkdir(real(p).c_str(), 0700) == 0; }
    bool remove(const String& p) { return unlink(real(p).c_str()) == 0; }
    bool rename(const String& a, const String& b) { return ::rename(real(a).c_str(), real(b).c_str()) == 0; }
    FsFile open(const String& p, const char* mode = "r") {
        FsFile f;
        f.path = real(p);
        bool writing = mode && mode[0] == 'w';
        FILE* fp = fopen(f.path.c_str(), writing ? "wb" : "rb");
        if (!fp) return f;
        f.fp = std::shared_ptr<FILE>(fp, [](FILE* x) { fclose(x); });
        f.ok = true;
        f.full = full && writing;
        return f;
    }
    size_t totalBytes() { return total; }
    size_t usedBytes() { return used(root); }
    void clear() { wipe(root); }   // everything under the root gone, the root kept: a fresh format
    std::string readAll(const String& p) {
        std::string s; FsFile f = open(p); uint8_t b[256]; size_t n;
        while (f && (n = f.read(b, sizeof b)) > 0) s.append((char*)b, n);
        return s;
    }
    void put(const String& p, size_t n, char fill = 'x') {   // a file of n bytes, as the reclaim finds one
        FsFile f = open(p, "w"); std::string s(n, fill); f.write((const uint8_t*)s.data(), s.size()); f.close();
    }

private:
    static size_t blocks(size_t sz) { return sz <= INLINE_MAX ? 0 : (sz + BLOCK - 1) / BLOCK * BLOCK; }
    static size_t used(const std::string& dir) {
        size_t sum = 2 * BLOCK;
        DIR* d = opendir(dir.c_str());
        if (!d) return sum;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = dir + "/" + n;
            struct stat st;
            if (stat(p.c_str(), &st) != 0) continue;
            sum += S_ISDIR(st.st_mode) ? used(p) : blocks((size_t)st.st_size);
        }
        closedir(d);
        return sum;
    }
    static void wipe(const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = dir + "/" + n;
            struct stat st;
            if (stat(p.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { wipe(p); rmdir(p.c_str()); } else unlink(p.c_str());
        }
        closedir(d);
    }
};
extern LittleFSClass LittleFS;
