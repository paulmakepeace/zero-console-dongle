# Open questions

- Whether holding the MBB awake makes the cellular module check in. Answered
  no, for five minutes at a time: overnight on 2026-09-09, fourteen wakes
  from hibernation each held the MBB up for about five minutes, and
  `cell_network_registration`, `connected_to_starcom` and
  `cell_signal_percent` read zero in all 840 samples, with the `ccm` rows
  refreshing throughout (median age 43 s). The module is alive during those
  wakes, since `gps_is_valid` reads 1 and its heartbeat state of charge
  moved over the night, so it has a fix and is being read; it simply does
  not attach to the network. Still open: whether a longer hold does, since
  five minutes may be short for an LTE-M attach, and what the module is
  waiting for. The data is in a git-ignored CSV under logs/.
- What sets the cellular module's schedule. The hourly wake charges the
  12 V battery only when the module answers, roughly one wake in a dozen to
  fifty by the console captures and the app logs alike, and the rest time
  out. Whether the module wakes on its own clock, on signal, or on a server
  check-in is not known; the cover was on for both a charge and a timeout,
  so signal alone does not explain it. Owners see the result from the far
  end: location uploads every two to five hours with the ignition off, and
  none while the 12 V system is down, which is the hibernating bike (see
  [sources.md](sources.md), the 2023 theft post). An impact registered by
  a parked bike reached its owner as a notification, so the module's
  accelerometer, the `ccm_accel` rows of `ccm`, is read during those wakes.
- Does the half-hour heartbeat happen from deep sleep? None appeared in
  eleven 96-second timeout wakes; the one charge wake printed it at its
  30-minute mark.
- Does the hourly cycle count from every HIB entry, including one that
  follows a wake-pin reset? The entry after the observed reset was not
  captured.
- One line in the first captured wake arrived corrupted at the PWSU to HIB
  transition; the second wake's same line was clean. Watch whether it recurs.
- The bike unit once went dark on the network mid console session with its
  power LED on and no reboot, and needed a key cycle. Not reproduced on the
  bench with a large response or a stalled client, nor on the bike after.
  A recurrence leaves evidence: the watchdog reboots a hung task and the
  status carries the reset reason and the WiFi disconnect count.
- How the space reclaim behaves once the log directory holds a hundred files
  or more. Measured on the bench board 2026-09-09 with 133 files and the
  filesystem at its reserve: the reclaim walks the whole directory up to four
  times from inside the capture stage, and the longest capture pass went to
  10 s, long enough for every HTTP request to time out. With the two dozen
  files a pulled bike carries it is 0.7 s. The bike reaches a hundred files
  in about four days without a pull, so this is on the path, not a bench
  artifact. Addressed but not yet re-measured: `dictCollect` no longer opens
  every file to read its dictionary header (the id is in the file name now),
  which was the bulk of the per-file cost, and `FS_MAX_FILES` caps the log
  count as a backstop so the directory cannot grow to where the walk stalls.
  The walk still holds the store's lock, so re-run the 133-file case on the
  bench to confirm the longest capture pass is now sub-second, and that the
  cap holds, before calling this closed.
- Is the CAN bus on pins 6 and 14 active while the MBB hibernates?
- Which of the bike's CAN networks is on OBD pins 6 and 14, and at what
  bitrate. The bench plan is in hardware.md.
- What the console prints at a wake with long-term storage mode on,
  presumably `LTSM state: INIT to EN` since EN is the state `bms` shows.
  The firmware takes any state but DIS and INIT as on.
- What DTC `B1A0F` means.
- Current available on pin 16 and the fuse size upstream of it.
- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: does the fault
  return, and is 80 ohms the right value if it does?
- Does the USB-C receptacle line up with the shell's notch once the board
  sits on its post?
