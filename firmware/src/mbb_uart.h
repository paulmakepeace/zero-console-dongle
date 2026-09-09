#pragma once
#include <Arduino.h>

typedef void (*LineHandler)(const char* line, size_t len);
typedef void (*RawHandler)(const uint8_t* data, size_t len);
typedef void (*StateHandler)(bool awake);

void mbbPinsSafe();   // both console pins as inputs with pull-downs; call first thing in setup()
bool mbbBegin(RawHandler onRaw);   // raw bytes are pushed straight from the capture task; false if the driver failed
void mbbTick(LineHandler onLine, StateHandler onState);   // from loop(): delivers lines, markers and edges in order
bool mbbOk();
bool mbbAwake();
bool mbbTxAttached();
size_t mbbWrite(const uint8_t* data, size_t len);   // bytes queued; 0 while the MBB is asleep
void mbbTxHold(bool on);   // keep the transmit pin attached across a batch; pin 8 low still releases it
void mbbWake(uint32_t holdMs);   // drive pin 9 high for the hold, pin 8 low or not: wakes a hibernating MBB and keeps it up
uint32_t mbbWakeHoldS();         // seconds left of a wake's hold; 0 when none
uint32_t mbbLastByteMs();
uint32_t mbbOverflows();       // FIFO overruns: bytes were lost
uint32_t mbbBackpressure();    // ring buffer full: bytes were held, none lost
uint32_t mbbFrameErrors();
uint32_t mbbQueueDrops();         // lines the loop task was too slow to take
bool mbbLineHigh();               // pin 8 as of the last sample, the transmit gate
uint32_t mbbCaptureStackFree();
// What this bike does, against the constants in config.h that assume it.
uint32_t mbbObservedBootMs();           // the line coming up to the MBB's first byte; MBB_BOOT_MS is the assumption
uint32_t mbbObservedLineLowAfterMs();   // the last byte to the line going down; SLEEP_AFTER_MS assumes it is inside that
uint32_t mbbObservedPrompts();         // answers that ended with MBB_PROMPT: zero on a bike whose prompt differs   // bytes never used, from the high-water mark
