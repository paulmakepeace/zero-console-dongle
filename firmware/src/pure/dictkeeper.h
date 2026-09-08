// The dongle's own dictionary: the lines that keep coming back, learned from
// its own sessions. Each incoming line is stripped of its stamps and looked
// up; a line already in the dictionary is marked as used, a new one is kept
// as a candidate. At a session's end, if the session brought enough new
// lines, the dictionary is rebuilt as new lines first, then the old lines
// that were used, then the rest, up to its size. Nothing here touches the
// heap.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Copy a line without its stamps: the dongle's up to the first space, and
// every MM/DD/YYYY hh:mm:ss.mmm the MBB put in. Returns the length written.
inline size_t stripStamps(const char* in, size_t len, char* out, size_t cap) {
    size_t i = 0;
    while (i < len && in[i] != ' ') i++;   // the dongle stamp
    while (i < len && in[i] == ' ') i++;
    size_t o = 0;
    static const char pat[] = "dd/dd/dddd dd:dd:dd.ddd";
    const size_t plen = sizeof(pat) - 1;
    while (i < len && o + 1 < cap) {
        if (i + plen <= len) {
            bool m = true;
            for (size_t j = 0; j < plen && m; j++) {
                char c = in[i + j];
                m = pat[j] == 'd' ? (c >= '0' && c <= '9') : (c == pat[j]);
            }
            if (m) {   // drop the stamp, and the space after it when one precedes it
                i += plen;
                while (i < len && in[i] == ' ' && o && out[o - 1] == ' ') i++;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\t' || out[o - 1] == '\r')) o--;   // trailing space
    size_t s = 0;
    while (s < o && (out[s] == ' ' || out[s] == '\t')) s++;                                // leading space
    if (s) { memmove(out, out + s, o - s); o -= s; }
    return o;
}

template <size_t D, size_t CAND, size_t MAXLINES, size_t LINE_CAP>
struct DictKeeper {
    uint8_t dict[D];
    size_t dlen = 0;
    uint16_t start[MAXLINES];   // offsets of the dictionary's lines
    uint16_t nlines = 0;
    uint8_t used[MAXLINES / 8 + 1];
    uint8_t cand[CAND];
    size_t clen = 0;
    uint16_t ncand = 0;
    uint16_t sessionsSinceLoad = 0;
    char tmp[LINE_CAP];

    static_assert(MAXLINES >= D / 9 + 1, "every line is at least 9 bytes with its newline; the index must hold them all");

    // Adopt a dictionary: lines separated by newlines. Used marks start over.
    void load(const uint8_t* bytes, size_t len) {
        if (len > D) len = D;
        memcpy(dict, bytes, len);
        dlen = len;
        index();
        memset(used, 0, sizeof used);
        sessionsSinceLoad = 0;
        clen = 0;
        ncand = 0;
    }

    // A session ended without a rebuild: its candidates go, but what it used
    // stays marked, so "used" means used since the last rebuild; the marks
    // start over every so often so a line that stopped recurring loses them.
    void endSession(unsigned markSessions = 48) {
        clen = 0;
        ncand = 0;
        if (++sessionsSinceLoad >= markSessions) { memset(used, 0, sizeof used); sessionsSinceLoad = 0; }
    }
    void clear() { memset(used, 0, sizeof used); clen = 0; ncand = 0; }

    // A line as the store took it, stamps and all.
    void note(const char* line, size_t len) {
        size_t n = stripStamps(line, len, tmp, sizeof tmp);
        if (n < 8) return;
        int at = find(dict, dlen, start, nlines, tmp, n);
        if (at >= 0) { used[at / 8] |= 1 << (at % 8); return; }
        if (findCand(tmp, n) >= 0) return;
        if (clen + n + 1 > CAND) return;   // the candidate buffer is full; later novelty waits for the next session
        memcpy(cand + clen, tmp, n);
        cand[clen + n] = '\n';
        clen += n + 1;
        ncand++;
    }

    size_t noveltyBytes() const { return clen; }

    // Rebuild into `out`: proven lines up to two thirds of the cap, then this
    // session's new lines, then the remaining proven lines, then the rest.
    // Returns the length; newBytes says how much of the new got in, so a
    // rebuild that learned nothing can be skipped.
    size_t rebuild(uint8_t* out, size_t cap, size_t* newBytes = nullptr) const {
        size_t o = 0;
        o = copyDictLines(out, o, cap * 2 / 3, true);
        size_t before = o;
        o = copyLines(cand, clen, out, o, cap);
        if (newBytes) *newBytes = o - before;
        o = copyDictLines(out, o, cap, true, before);   // the proven lines the two-thirds cut left out
        o = copyDictLines(out, o, cap, false);
        return o;
    }

private:
    void index() {
        nlines = 0;
        size_t s = 0;
        for (size_t i = 0; i < dlen && nlines < MAXLINES; i++) {
            if (dict[i] == '\n') { start[nlines++] = s; s = i + 1; }
        }
        if (s < dlen && nlines < MAXLINES) start[nlines++] = s;
    }

    static int find(const uint8_t* buf, size_t blen, const uint16_t* st, uint16_t n, const char* line, size_t len) {
        for (uint16_t i = 0; i < n; i++) {
            size_t s = st[i], e = i + 1 < n ? st[i + 1] : blen;
            size_t l = e - s;
            if (l && buf[e - 1] == '\n') l--;
            if (l == len && memcmp(buf + s, line, len) == 0) return i;
        }
        return -1;
    }

    int findCand(const char* line, size_t len) const {
        size_t s = 0;
        for (size_t i = 0; i < clen; i++) {
            if (cand[i] != '\n') continue;
            if (i - s == len && memcmp(cand + s, line, len) == 0) return 1;
            s = i + 1;
        }
        return -1;
    }

    // Copy lines with the wanted mark, in order, skipping those already copied
    // (the first `alreadyBytes` bytes of the proven run), until cap.
    size_t copyDictLines(uint8_t* out, size_t o, size_t cap, bool wantUsed, size_t alreadyBytes = 0) const {
        size_t seen = 0;
        for (uint16_t i = 0; i < nlines; i++) {
            bool u = used[i / 8] & (1 << (i % 8));
            if (u != wantUsed) continue;
            size_t s = start[i], e = i + 1 < nlines ? start[i + 1] : dlen;
            if (seen < alreadyBytes) { seen += e - s; continue; }
            if (o + (e - s) > cap) return o;
            memcpy(out + o, dict + s, e - s);
            o += e - s;
        }
        return o;
    }

    static size_t copyLines(const uint8_t* src, size_t slen, uint8_t* out, size_t o, size_t cap) {
        size_t s = 0;
        for (size_t i = 0; i < slen; i++) {
            if (src[i] != '\n') continue;
            size_t l = i + 1 - s;
            if (o + l > cap) return o;
            memcpy(out + o, src + s, l);
            o += l;
            s = i + 1;
        }
        return o;
    }
};
