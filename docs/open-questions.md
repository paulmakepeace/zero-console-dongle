# Open questions

- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: does the fault
  return, and is 80 ohms the right value if it does?
- How long pin 8 stays live after key-off, and whether it drops cleanly.
  The console has answered commands about 20 s after key-off; the 0 V
  reading in the pinout was taken at an unrecorded delay. An hour-plus run
  of `tools/capture.py` with the bike asleep answers it, sizes the shutdown
  window, and decides whether the pin 16 supply is worth building.
- How long the hourly wake keeps the console up, and what it prints.
- Which of the bike's CAN networks is on OBD pins 6 and 14.
- What DTC `B1A0F` means.
- Zero CAN bitrate. Try 500k, then 250k, then 125k, listen-only.
- Current available on pin 16 and the fuse size upstream of it.
- Does the DevKit corner hole line up with a post and the notch with the USB-C
  receptacle, with a USB-C cable from the frunk socket also fitting through
  the notch?
