#pragma once
#include <Arduino.h>
#include <FS.h>

bool storeBegin();
void storeSessionOpen();
void storeSessionClose();
void storeAppend(const String& line);
void storeTick();
String storeActiveName();
String storeListJson();
bool storeDelete(const String& name);
File storeOpenRead(const String& name);
void storeStats(size_t& total, size_t& used);
String storeLastLines();
uint32_t storeBootCount();
