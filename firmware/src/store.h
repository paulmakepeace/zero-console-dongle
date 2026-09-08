#pragma once
#include <Arduino.h>
#include <FS.h>

bool storeBegin(const char* resetReason);
void storeSessionClose();
void storeAppend(const String& line);
void storeTick();
String storeActiveName();
String storeListJson();
bool storeDelete(const String& name);
File storeOpenRead(const String& name);   // pair with storeReadDone; the file is safe from reclaim meanwhile
void storeReadDone(const String& name);
void storeStats(size_t& total, size_t& used);
String storeLastLines();
uint32_t storeBootCount();
uint32_t storeDroppedLines();
bool storeOk();
uint32_t storeFormats();
void storeNoteEdge(bool awake);
String storeEdges();   // JSON fragment: last awake and asleep stamps, count
