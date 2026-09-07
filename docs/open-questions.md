# Open questions

- Does a wake from deep sleep ever run the 12 V top-up? The one captured so
  far timed out in PWSU after 60 s and went back to sleep; the two top-ups
  seen were both from the shallow hibernation the adapter holds the MBB in.
  More deep-sleep wakes with the dongle on pins 5 and 8 only answer it, and
  whether the choice depends on the 12 V battery's voltage.
- Does the half-hour heartbeat happen from deep sleep, or only in the
  shallow state? The first deep-sleep cycle showed none in its 96 s awake.
- Does the hourly cycle count from every HIB entry, including one that
  follows a wake-pin reset? The entry after the observed reset was not
  captured.
- One line in the first captured wake arrived corrupted at the PWSU to HIB
  transition. One in seventy; watch whether it recurs at the same point.
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
