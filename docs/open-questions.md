# Open questions

- Does `config` exist on this bike's MY2020 firmware? `help` will tell.
- Measured LED current per side after `config 5`: is a resistor still needed?
- Zero CAN bitrate. Try 500k, then 250k, then 125k, listen-only.
- Current available on pin 16 and the fuse size upstream of it.
- Does the DevKit corner hole line up with a post and the notch with the USB-C
  receptacle?
- Does ESPHome `esp32_can` support listen-only mode?
