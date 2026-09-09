#pragma once
#include <Arduino.h>

// Runs a fixed command set on the MBB on a slow schedule while it is awake
// and no console client is connected, and keeps the last output of each;
// one of the set typed by a console client is kept the same way.
void pollerBegin(uint32_t intervalS);
void pollerSetInterval(uint32_t s);   // 0 turns the schedule off; a request still runs
uint32_t pollerInterval();
bool pollerConsumeLine(const char* line, size_t len);   // true: a line kept for the API; the log takes it regardless
void pollerTick(bool mbbAwake, bool consoleBusy);
bool pollerRequest();   // false when the MBB has announced its hibernation: the request is not kept
bool pollerActive();
String pollerListJson();
uint32_t pollerObservedAnswerMaxMs();   // the longest answer seen; MBB_ANSWER_MS is the assumption behind POLL_TIMEOUT_MS
bool pollerHasCommand(const char* name);   // exactly one of the polled set
const String* pollerOutput(const char* name);
long pollerOutputAgeS(const char* name);   // seconds; -1 none, -2 from before this boot with the clock not yet set
long pollerSoc();              // -1 until known
bool pollerPack(long& soc, long& packMv, long& currentMa, long& capacityAh, long& tempHiC, long& tempLoC);   // from the last status
const char* pollerBikeState(); // "" until known
