# Open questions

- Does pin 8 rise on the MBB's own hourly wake with nothing driving pin 9?
  Everything so far was observed with pin 9 held high by the adapter, which
  keeps the console up. If the console block also powers up for an RTC wake,
  a dongle sleeping on pin 8 captures every wake and top-up; if not, it
  captures rides and key-on only. One hourly wake with the DevKit or the
  adapter connected on pins 5 and 8 only answers it.
- The same capture shows whether the half-hour heartbeat, the limits and
  torque lines at 30-minute marks, happens at all without pin 9 held.
- Does the hourly cycle count from every HIB entry, including one that
  follows a wake-pin reset? The entry after the observed reset was not
  captured.
- Is the CAN bus on pins 6 and 14 active while the MBB hibernates?
- Which of the bike's CAN networks is on OBD pins 6 and 14, and at what
  bitrate. Try 500k, then 250k, then 125k, listen-only.
- What DTC `B1A0F` means.
- Current available on pin 16 and the fuse size upstream of it.
- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: does the fault
  return, and is 80 ohms the right value if it does?
- Does the USB-C receptacle line up with the shell's notch once the board
  sits on its post?
