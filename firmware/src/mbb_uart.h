#pragma once
#include <Arduino.h>

typedef void (*LineHandler)(const char* line, size_t len);
typedef void (*RawHandler)(const uint8_t* data, size_t len);
typedef void (*StateHandler)(bool awake);

void mbbPinsSafe();   // both console pins as inputs with pull-downs; call first thing in setup()
void mbbBegin(LineHandler onLine, RawHandler onRaw, StateHandler onState);
bool mbbAwake();
bool mbbTxAttached();
size_t mbbWrite(const uint8_t* data, size_t len);   // 0 while the MBB is asleep
