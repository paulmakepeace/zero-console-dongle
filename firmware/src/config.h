#pragma once

#define FW_VERSION      "0.7.1"
#define DONGLE_NAME     "zero-dongle"      // base of the hostname, mDNS name and setup AP name; the last four hex digits of the MAC are appended
// The setup network's password defaults to "zero-" plus the last six hex digits
// of the MAC, printed at boot, and can be replaced from the setup page.

// Wiring, phase 1: no series resistors, internal pulls only.
#define PIN_MBB_RX      33   // from OBD pin 8, MBB TX. RTC-capable, has internal pull-down.
#define PIN_BOOT_BUTTON 0    // the DevKit's BOOT button, held at power-up to raise the setup network
#define PIN_MBB_TX      17   // to OBD pin 9, MBB RX and hibernation wake pin. Driven only while the MBB is awake.
#define MBB_BAUD        115200
#define CONSOLE_BAUD    115200   // the DevKit's own USB port; platformio.ini monitor_speed matches
#define UART_RX_BUF     8192 
#define UART_TX_BUF     1024
#define UART_EVENT_QUEUE 64
#define EVENT_BUF       16384  // framed lines and edges waiting for the loop task

#define HTTP_PORT       80
#define CONSOLE_PORT    6638
#define CONSOLE_CLIENTS 2
#define AUTH_FAILS_FOR_PORTAL 3
#define AUTH_PORTAL_MS  600000    // how long a setup network raised by those failures stays up unattended before the retry resumes

// The poller: a fixed command set on a slow schedule while the MBB is awake.
#define POLL_CMDS       "status", "charging", "bms", "pdu", "in", "faults"
#define POLL_INTERVAL_S 60
#define POLL_SETTLE_MS  20000    // no commands into a MBB that is still booting
#define POLL_TIMEOUT_MS 8000     // a command with no prompt back by then is abandoned
#define POLL_MAX_BYTES  3072     // kept per command

// Light sleep between MBB sessions.
#define SLEEP_GRACE_MS   120000   // stay reachable this long after the MBB sleeps
#define SLEEP_MARGIN_PCT 10       // sleep this much less than the time until the MBB is due: the RC clock runs long
#define SLEEP_MIN_S      30       // shorter than this is not worth the WiFi round trip
#define SLEEP_FALLBACK_S 3600     // with no announcement seen, wake hourly anyway
#define SLEEP_AFTER_DAYS 3        // sleep only once the bike has gone this long without a 12 V top-up or a key-on; 0 for always   // consecutive authentication failures before the setup network is raised

#define LOG_DIR         "/logs"
#define FS_MIN_FREE     (96 * 1024)   // delete the oldest file below this much free space
#define LAST_LINES      40            // lines kept in RAM for the status page
#define LAST_LINE_CHARS 200           // each cut to this many characters there
#define SESSION_MAX_BYTES (256 * 1024) // a session that never sleeps rolls to a new file here

#define IDLE_FLUSH_MS   2000   // a partial line (the prompt) is written after this much silence
#define SLEEP_AFTER_MS  5000   // MBB counted asleep after this long with pin 8 low and no bytes
#define AWAKE_HIGH_MS   60     // pin 8 high this long with nothing arriving counts as awake
#define TX_HOLD_MS      2000   // TX stays attached this long after the last byte sent
#define RECLAIM_GAP_MS  2000   // a failed write retries reclamation at most this often
#define IDLE_COMMIT_MS  3000   // lines reach flash once the MBB has been quiet this long
#define MAX_PENDING_MS  15000  // or after this long regardless, or when the output buffer is full
// The session stream's fixed arrays: dictionary, history window, longest line, output buffer, hash table.
#define GZ_DICT         6144
#define GZ_HISTORY      8192
#define GZ_LINE_CAP     1100
#define GZ_OUT          4096
#define GZ_HASH_BITS    10
// The dictionary the dongle learns: candidate buffer per session, the most lines it indexes,
// how much new text a session must bring before the dictionary is rebuilt, and how often at most.
#define DICT_CAND       3072
#define DICT_MAX_LINES  700    // 6144 bytes of lines at least 9 bytes each
#define DICT_NOVELTY    1024
#define DICT_REFRESH_MIN_MS 3600000
#define DICT_MARK_SESSIONS  48   // sessions before the used marks start over, so a line that stopped recurring can fall off
#define GZ_HEADER_ROOM  200    // kept spare until the session header is in: the header is at most about 160 characters, 180 bytes at 9-bit literals
#define LOOP_WDT_S      120    // loop() or the capture task silent this long: panic and reboot

#define TZ_DEFAULT      "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER      "pool.ntp.org"
#define NTP_FRESH_S     (6 * 3600)   // an NTP fix younger than this outranks the MBB's stamps
