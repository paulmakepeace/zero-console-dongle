#pragma once
#include <Arduino.h>
#include <FS.h>

enum StoreDeleteResult { STORE_DELETED, STORE_NOT_FOUND, STORE_REFUSED };

bool storeBegin(const char* resetReason);
void storeSessionClose();
void storeShutdown();   // before a deliberate restart: end the session, drop waiting notes counted
void storeAppend(const String& line, bool fromMbb = true);   // notes alone never open a session
void storeTick(bool mbbQuiet);   // the caller knows whether the MBB is talking
String storeActiveName();
String storeListJson();
StoreDeleteResult storeDelete(const String& name);
File storeOpenRead(const String& name, bool* busy = nullptr);   // pair with storeReadDone; the file is safe from reclaim meanwhile; busy: no free reader
void storeReadDone(const String& name);
void storeStats(size_t& total, size_t& used);
String storeLastLines();
uint32_t storeBootCount();
uint32_t storeDroppedLines();
bool storeOk();
uint32_t storeFormats();
void storeNoteEdge(bool awake);
String storeEdges();   // JSON fragment: last awake and asleep stamps, count
