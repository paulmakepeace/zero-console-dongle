#pragma once

#define FW_VERSION      "0.3.2"
#define DONGLE_NAME     "zero-dongle"      // base of the hostname, mDNS name and setup AP name; the last four hex digits of the MAC are appended
// The setup network's password defaults to "zero-" plus the last six hex digits
// of the MAC, printed at boot, and can be replaced from the setup page.

// Wiring, phase 1: no series resistors, internal pulls only.
#define PIN_MBB_RX      33   // from OBD pin 8, MBB TX. RTC-capable, has internal pull-down.
#define PIN_BOOT_BUTTON 0    // the DevKit's BOOT button, held at power-up to raise the setup network
#define PIN_MBB_TX      17   // to OBD pin 9, MBB RX and hibernation wake pin. Driven only while the MBB is awake.
#define MBB_BAUD        115200
#define CONSOLE_BAUD    115200   // the DevKit's own USB port; platformio.ini monitor_speed matches
#define UART_RX_BUF     16384
#define UART_TX_BUF     1024
#define UART_EVENT_QUEUE 16

#define HTTP_PORT       80
#define CONSOLE_PORT    6638
#define CONSOLE_CLIENTS 2

#define LOG_DIR         "/logs"
#define FS_MIN_FREE     (96 * 1024)   // delete the oldest file below this much free space
#define LAST_LINES      40            // lines kept in RAM for the status page
#define LAST_LINE_CHARS 200           // each cut to this many characters there
#define SESSION_MAX_BYTES (256 * 1024) // a session that never sleeps rolls to a new file here

#define IDLE_FLUSH_MS   2000   // a partial line (the prompt) is written after this much silence
#define SLEEP_AFTER_MS  5000   // MBB counted asleep after this long with pin 8 low and no bytes
#define AWAKE_SAMPLES   3      // consecutive 20 ms samples of pin 8 high before it counts as awake
#define TX_HOLD_MS      2000   // TX stays attached this long after the last byte sent
#define RECLAIM_GAP_MS  10000  // a failed write retries reclamation at most this often
#define IDLE_COMMIT_MS  3000   // lines reach flash once the MBB has been quiet this long
#define MAX_PENDING_MS  15000  // or after this long regardless
#define PENDING_MAX     12288  // or when this much is waiting in RAM
#define LOOP_WDT_S      120    // loop() or the capture task silent this long: panic and reboot

#define TZ_DEFAULT      "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER      "pool.ntp.org"
#define NTP_FRESH_S     (6 * 3600)   // an NTP fix younger than this outranks the MBB's stamps
