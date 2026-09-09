#pragma once
#include <Arduino.h>

// The figures owners asked for, kept by name as the rows carrying them go
// by, from a poll batch, a typed command or a line the MBB prints on its
// own alike. Each keeps its last value and when it was seen.
void readingsNoteLine(const char* line, size_t len);                 // every framed line, live
void readingsFeed(const char* text, size_t len, long epoch);         // a saved output, line by line, stamped with its wall time
String readingsJson();                                               // [{n, g, v, u, age_s}, ...] in display order
