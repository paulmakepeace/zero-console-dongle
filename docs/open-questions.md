# Open questions

- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: does the fault
  return, and is 80 ohms the right value if it does?
- How long the console stays up after key-off. At least 20 s by observation;
  the upper bound needs a timestamped capture. This sizes the dongle's
  shutdown window.
- Which of the bike's CAN networks is on OBD pins 6 and 14.
- What DTC `B1A0F` means.
- Zero CAN bitrate. Try 500k, then 250k, then 125k, listen-only.
- Current available on pin 16 and the fuse size upstream of it.
- Does the DevKit corner hole line up with a post and the notch with the USB-C
  receptacle?
- Does ESPHome `esp32_can` support listen-only mode?
