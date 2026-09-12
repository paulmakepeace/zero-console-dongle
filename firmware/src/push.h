#pragma once
#include <Arduino.h>

// The push: session files uploaded to the archive on every WiFi join and
// session end, deleted from the flash on the server's 200. Off with no URL.
void pushBegin(const char* url);
bool pushSetUrl(const String& url);   // empty turns it off; false: not an http://host[:port][/path] URL
void pushRequest();    // a join or a session end: files may be waiting
void pushTick();
bool pushBusy();       // files are due and the network is up: the dongle stays up for them
String pushStatusJson();
