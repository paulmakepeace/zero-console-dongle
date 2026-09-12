#pragma once
#include <Arduino.h>

// The push: session files uploaded to the archive on every WiFi join and
// every session end, oldest first, each deleted from the flash once the
// server has answered 200. Dictionaries go first and are never deleted.
// Off with no URL set. Everything runs on the loop task.
void pushBegin(const char* url);
bool pushSetUrl(const String& url);   // empty turns it off; false: not an http://host[:port][/path] URL
const char* pushUrl();
void pushRequest();    // a join or a session end: files may be waiting
void pushTick();
bool pushBusy();       // files are due: the dongle stays up for them
String pushStatusJson();
