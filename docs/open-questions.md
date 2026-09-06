# Open questions

- Rear LED draw on its own. Indicate with the rear signal unplugged and
  subtract from the mixed-load figure in led-signals.md.
- Current per side with all four LEDs fitted, in LED mode: is 80 ohms right?
- Where exactly the LED-mode ceiling sits, between 422 mA and 872 mA.
- How long the console stays up after key-off. It answered `help` after the
  key went off in the first session, but the capture has no timestamp for it.
  This sizes the dongle's shutdown hold.
- Which of the bike's CAN networks is on OBD pins 6 and 14.
- What DTC `B1A0F` means.
- Zero CAN bitrate. Try 500k, then 250k, then 125k, listen-only.
- Current available on pin 16 and the fuse size upstream of it.
- Does the DevKit corner hole line up with a post and the notch with the USB-C
  receptacle?
- Does ESPHome `esp32_can` support listen-only mode?
