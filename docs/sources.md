# Sources

- zerologs.bike, "Extract Zero Logs with a Serial Console": pinout, baud, port
  location.
- zerologs.bike, "Zero Gen3 Log Format: 0xB2 Record Specification (MBB & BMS)"
  (learn/gen3-log-format): the byte-level format of the app's and dealers'
  log dumps, the event log this console will not print. Record framing and
  the delimiter escape, the MBB and BMS record types and field maps, what a
  hibernation wake writes, and the retractions.
- zeromanual.com, Gen2/Gen3 MBB Console and "How to build a cable". The site
  blocks fetches; reachable via search caches.
- Espressif, ESP-IDF programming guide, "Sleep Modes" for the ESP32: the
  definitions of light and deep sleep, the wake sources, and what each keeps.
- electricmotorcycleforum.com, "Zero SR/F MY2020 OBD cable to serial console".
- Facebook zmcowners group, photo 10165269412658619 (2026-07-27): an owner's
  own phone app reading Zero's Starcom cloud with the OEM app login, showing
  what the OEM app hides: 12 V battery voltage and health, altitude, LTE-M
  signal, satellite count, a theft-attempt flag, location. The fields the
  console's `ccm` and `in` print locally, seen from the cloud end.
- Facebook zmcowners group: post 8283341791734956 (Kevin Campbell, 2024-09-23)
  and post 8326312574104544 (Ernst Glatzer, 2024-09-28). LED mode confirmation
  and the threshold measurements in [led-signals.md](led-signals.md).
- Zero SR/S Service Manual rel. 1.1 (manualslib 3142308): sections 70-11 and
  70-12 front signals, 60-14 outer fairings, 60-2 headlight upper fairing,
  70-16 rear signals.
- ECMCables "OBDLink MBB Spy", discontinued (2013 to 2018 bikes). No current
  Gen3 product exists.
