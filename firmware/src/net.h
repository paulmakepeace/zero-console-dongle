#pragma once
#include <Arduino.h>

void netBegin();
void netTick();
void netPushRaw(const uint8_t* data, size_t len);   // MBB bytes for the TCP console clients
