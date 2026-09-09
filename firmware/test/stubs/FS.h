// store.h names File in its interface; the poller does not use it, so the
// host build needs the type to exist and nothing more.
#pragma once
#include <LittleFS.h>
typedef FsFile File;
