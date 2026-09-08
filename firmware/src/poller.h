#pragma once
#include <Arduino.h>

// Runs a fixed command set on the MBB on a slow schedule while it is awake
// and no console client is connected, and keeps the last output of each.
void pollerBegin(uint32_t intervalS);
void pollerSetInterval(uint32_t s);   // 0 turns the schedule off; a request still runs
uint32_t pollerInterval();
bool pollerConsumeLine(const char* line, size_t len);   // true: poll output, not for the log
void pollerTick(bool mbbAwake, bool consoleBusy);
bool pollerRequest();   // false when the MBB has announced its hibernation: the request is not kept
bool pollerActive();
String pollerListJson();
const String* pollerOutput(const char* name);
uint32_t pollerOutputAgeS(const char* name);
long pollerSoc();              // -1 until known
bool pollerPack(long& soc, long& packMv, long& currentMa, long& capacityAh, long& tempHiC, long& tempLoC);   // from the last status
const char* pollerBikeState(); // "" until known
