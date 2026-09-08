#pragma once
#include <Arduino.h>
#include "pure/json_escape.h"

inline String jsonEscape(const String& in) {
    std::string s = jsonEscape(in.c_str(), in.length());
    return String(s.c_str());
}
