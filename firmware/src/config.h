#pragma once

#define FW_VERSION      "0.2.3"
#define DONGLE_NAME     "zero-dongle"      // hostname, mDNS name, setup AP name
#define SETUP_AP_PASS   "zerodongle"       // password of the setup AP

// Wiring, phase 1: no series resistors, internal pulls only.
#define PIN_MBB_RX      33   // from OBD pin 8, MBB TX. RTC-capable, has internal pull-down.
#define PIN_MBB_TX      17   // to OBD pin 9, MBB RX and hibernation wake pin. Driven only while the MBB is awake.
#define MBB_BAUD        115200
#define UART_RX_BUF     16384
#define UART_TX_BUF     1024
#define UART_EVENT_QUEUE 16

#define HTTP_PORT       80
#define CONSOLE_PORT    6638
#define CONSOLE_CLIENTS 2

#define LOG_DIR         "/logs"
#define FS_MIN_FREE     (96 * 1024)   // delete the oldest file below this much free space
#define LAST_LINES      40            // lines kept in RAM for the status page

#define IDLE_FLUSH_MS   2000   // a partial line (the prompt) is written after this much silence
#define SLEEP_AFTER_MS  5000   // MBB counted asleep after this long with pin 8 low and no bytes
#define AWAKE_SAMPLES   3      // consecutive 20 ms samples of pin 8 high before it counts as awake
#define TX_HOLD_MS      2000   // TX stays attached this long after the last byte sent
#define RECLAIM_GAP_MS  10000  // a failed write retries reclamation at most this often
#define IDLE_COMMIT_MS  300    // lines reach flash once the MBB has been quiet this long
#define MAX_PENDING_MS  5000   // or after this long regardless
#define PENDING_MAX     12288  // or when this much is waiting in RAM

#define TZ_DEFAULT      "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER      "pool.ntp.org"
