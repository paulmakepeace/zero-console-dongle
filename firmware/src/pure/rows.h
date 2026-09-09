// A console row to a name and a number. Two shapes carry nearly every
// figure the MBB prints:
//
//   comma table:  "            Motor_Temp,         35,         C,      Yes,         0"
//   dash list:    " - lowest_cell_voltage_mv 3976"   or   " - max_charge_voltage 117.6 V"
//
// The value is kept as an integer with a count of decimals, so 117.6 is
// 1176 with one decimal and nothing is lost. Units are not read from the
// row: the third column is a unit in some tables and a label in others.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

struct RowValue {
    const char* name;   // into the line; not terminated
    size_t nameLen;
    long value;
    uint8_t decimals;
    bool valid;         // false when a table's Valid column says No
};

// A signed integer with up to three decimals; false if the text is not one
// or has something after it other than a comma, a space or the end.
inline bool parseNumber(const char* s, size_t len, long& value, uint8_t& decimals) {
    size_t i = 0;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    if (i >= len || s[i] < '0' || s[i] > '9') return false;
    long v = 0;
    uint8_t d = 0;
    bool frac = false;
    for (; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (frac) { if (d == 3) continue; d++; }
            if (v > 99999999L) return false;   // nothing the console prints
            v = v * 10 + (c - '0');
        } else if (c == '.' && !frac) frac = true;
        else if (c == ',' || c == ' ' || c == '\t') break;
        else return false;
    }
    value = neg ? -v : v;
    decimals = d;
    return true;
}

inline bool parseRow(const char* line, size_t len, RowValue& out) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return false;
    if (line[i] == '-' && i + 1 < len && line[i + 1] == ' ') {
        // dash list: name is one word, the value the next
        i += 2;
        while (i < len && line[i] == ' ') i++;
        size_t n = i;
        while (i < len && line[i] != ' ') i++;
        if (i == n || i >= len) return false;
        out.name = line + n; out.nameLen = i - n;
        while (i < len && line[i] == ' ') i++;
        out.valid = true;
        return parseNumber(line + i, len - i, out.value, out.decimals);
    }
    // comma table: name up to the first comma, value the next field
    size_t n = i;
    while (i < len && line[i] != ',') i++;
    if (i >= len || i == n) return false;
    size_t e = i;
    while (e > n && line[e - 1] == ' ') e--;
    out.name = line + n; out.nameLen = e - n;
    i++;
    while (i < len && line[i] == ' ') i++;
    if (i >= len) return false;
    if (!parseNumber(line + i, len - i, out.value, out.decimals)) return false;
    // The fourth field of a five-column table is Valid: "No" is a figure
    // the MBB itself does not stand behind.
    out.valid = true;
    int commas = 0;
    for (; i < len && commas < 2; i++) if (line[i] == ',') commas++;
    if (commas == 2) {
        while (i < len && line[i] == ' ') i++;
        if (i + 2 <= len && line[i] == 'N' && line[i + 1] == 'o' && (i + 2 == len || line[i + 2] == ',' || line[i + 2] == ' ')) out.valid = false;
    }
    return true;
}

inline bool rowNameIs(const RowValue& r, const char* name) {
    return strlen(name) == r.nameLen && memcmp(r.name, name, r.nameLen) == 0;
}
