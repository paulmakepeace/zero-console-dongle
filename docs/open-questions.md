# Open questions

- What sets the cellular module's schedule. The hourly wake charges the
  12 V battery only when the module answers, roughly one wake in a dozen to
  fifty by the console captures and the app logs alike, and the rest time
  out. Whether the module wakes on its own clock, on signal, or on a server
  check-in is not known; the cover was on for both a charge and a timeout,
  so signal alone does not explain it.
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
- Is the CAN bus on pins 6 and 14 active while the MBB hibernates?
- Which of the bike's CAN networks is on OBD pins 6 and 14, and at what
  bitrate. The bench plan is in hardware.md.
- What the console prints with long-term storage mode on: the `LTSM
  state` name and the `bms` storage-mode word. The firmware takes any
  state but DIS, and the word Active, as on until they are seen.
- What DTC `B1A0F` means.
- Current available on pin 16 and the fuse size upstream of it.
- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: does the fault
  return, and is 80 ohms the right value if it does?
- Does the USB-C receptacle line up with the shell's notch once the board
  sits on its post?
