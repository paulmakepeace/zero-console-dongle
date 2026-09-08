# Firmware

## What it does

The MBB narrates on its own whenever it is awake: faults with the measured
current, state changes, contactor steps, the config write at key-off, the
hourly wake and 12 V top-up. That stream is a superset of the app's MBB log
(see [mbb-reference.md](mbb-reference.md)), so the first job is to capture it
whole. Everything else is layered on top.

Phase 1, in [../firmware/](../firmware/), with the build and the endpoints in
its [README](../firmware/README.md):

1. **Capture, always.** Every line from UART2 is stamped and appended to a
   file in the module's flash. No commands, no parsing on the ESP32.
2. **One file per MBB session.** A file opens when pin 8 goes high and closes
   five seconds after it goes low. Oldest files are deleted when free space
   runs low. Several days of parking and a few hours of riding fit in the
   flash.
3. **Serve the files over WiFi.** A status page, a JSON list, download and
   delete. `tools/pull-logs.py` on the homelab fetches new files on a
   schedule and deletes each one after the size checks.
4. **A raw TCP console** on port 6638, tee'd into the capture. Transmit is
   allowed only while pin 8 is high, because pin 9 is the MBB's wake pin and
   a high level reboots a sleeping bike, and the transmit pin is attached to
   the UART only while bytes are going out, because a UART idles high and a
   held-high pin 9 keeps the MBB out of deep sleep. Input arriving while the
   MBB sleeps is dropped, not queued.
5. **Time without a battery.** NTP when WiFi is up; otherwise the MBB's own
   stamps, which it prints on most lines, set the clock. Lines carry both.
6. **WiFi by provisioning.** No credentials in the build. With none stored,
   the dongle raises a setup access point and stores what is entered there.

Phase 2, in likely order:

- **Light sleep between sessions.** The MBB announces `Hibernating for
  3600 sec` and wakes 3600 s later to the second, so the dongle parses that
  line and sets a timer for ten seconds less, and is listening before the
  MBB boots. Pin 8 rising is the backstop for wakes it did not schedule.
  The ESP32 cannot wake from UART2, the port the MBB is on, so the UART is
  not a wake source. Light rather than deep sleep because this DevKit's
  regulator draws 5 mA regardless, and light sleep keeps WiFi associated.
- **A tabbed page of command outputs**, `pdu`, `in`, `bms`, `faults -v`,
  each refreshed by the poller and served raw at `/api/cmd/NAME`.
- **The poller needs a transaction**: send, hold the transmit pin attached
  across the whole batch, read until the prompt with a timeout, and stand
  aside while a console client is connected. The detach's own NUL makes the
  MBB print a prompt, so a framer that counts prompts must not see a detach
  in the middle of a batch. The capture module's line queue is the seam.
- **Light sleep needs the core's power management**, which the precompiled
  core leaves off, the same wall as the IRAM interrupt; and the UART runs
  from the APB clock, which stops in light sleep, so an unscheduled wake on
  pin 8 loses the first bytes of the banner. The timer wake, which has the
  dongle up before the MBB boots, avoids both.
- **CAN as a second stream.** TWAI in listen-only mode, frames stamped and
  written raw in a candump-style line format for SavvyCAN or a script. Which
  bus is on pins 6 and 14, and at what rate, is the first thing it tells us.
- **Poll a small command set on a slow schedule** while the bike is on: `in`,
  `pdu`, `bms`, `charging`, `faults`, about once a minute. Each response ends
  at the `ZERO MBB>` prompt, which is the frame delimiter. The poller pauses
  while a console client is connected, and never runs while the MBB sleeps.
- **Push instead of pull**, MQTT to the homelab, once pull has proven the
  files.
- **A fixed firmware other owners can flash** and configure from a browser:
  Improv WiFi with esp-web-tools.

## Implementation choice

Arduino framework under PlatformIO, on the pioarduino platform with the
Arduino core 3.x over ESP-IDF 5. Everything in phase 1 is a mature library
there. The hardware is driven through IDF calls where it matters, the UART
driver and the pin control in particular, so the code moves to ESP-IDF or to
Arduino-as-a-component without rewriting if power tuning or the ULP
coprocessor is ever needed. ESPHome was considered and left: the poller, the
console priority and the transmit gating are a custom component anyway, and
its logger and CAN component get in the way.

Development serial access: `pio device monitor`, `idf.py monitor`, or tio on
the DevKit's own USB port. See [console-port.md](console-port.md) for the tio
flags.

## Design rules

- The transmit pin is attached to the UART only while bytes are being sent
  and for 2 s after, only while the MBB is awake, and is an input with a
  pull-down otherwise, from the firmware's first instruction and again on
  every restart; the ROM boot window before that is what the phase 2 10k
  pull-down covers. The hold is checked from both the capture task and the
  loop task so no single stall can hold the pin high. The drop back to the
  pull-down reaches the MBB as one NUL byte, which it answers with a fresh
  prompt; verified on the bike with pin 9 connected.
- The capture task only reads bytes, frames lines and watches the pins. It
  never takes a mutex and never writes flash: lines, loss markers and the
  awake and asleep edges go through a queue, in order, to the loop task,
  which does the clock parse and the store append. That is what keeps a
  slow filesystem from stalling the reader into a false sleep. This is the one rule that keeps the
  dongle from waking the bike or holding it awake. Every future feature that
  sends anything goes through the same gate.
- The loop task never blocks on a network client. Console output to a client
  that cannot take it is dropped.
- Flash writes happen only after the MBB has been quiet for 3 s, plus at
  session end, with a 15 s bound. The proper fix is the UART interrupt in
  IRAM, which the precompiled core disables; pioarduino's `custom_sdkconfig`
  rebuild with `CONFIG_UART_ISR_IN_IRAM=y` gets through the IDF compile
  after stubbing four embedded certificate files it expects, then fails to
  link on an undefined `__wrap_log_printf` from the core's log wrapper.
  Left for phase 2, and this is what it buys. Measured on the bench with a
  continuous full-rate stream, the worst case the MBB never produces: on a
  freshly formatted filesystem, where every block is already erased and a
  commit only programs pages, 2179 lines in 20 s lost 2; on a filesystem in
  use, where a 12 KB commit erases three sectors at about 45 ms each with
  the interrupt suspended, 1926 lines in 15 s lost 261, every loss marked.
  With the interrupt in IRAM the 16 KB ring absorbs any erase and neither
  number is above zero, and the quiet-time commit rule becomes an
  optimisation rather than a necessity. Real MBB traffic is a few lines a
  minute with bursts of a dozen, which the quiet rule already keeps clear of
  the erases. A flash erase holds the UART interrupt off for longer
  than the receive FIFO covers, and the precompiled core keeps that
  interrupt out of IRAM. Frunk USB dies at key-off without warning, and the
  phase 2 supply is cut by a switch, so the bound is also the most a power
  cut can lose.
- Awake means the MBB's console block is powered: real bytes arriving with
  the line idling high behind them, or the line high for 60 ms with nothing
  arriving. A lone byte on a dead line is noise and does not count.
- Loss is marked in the file where it happened: a FIFO overrun (bytes lost)
  and a frame error (a nearby line may be corrupt) each write a marker line
  and count in the status; the driver's buffer-full event is back-pressure,
  counted but not a loss; the break the MBB makes as it sleeps is expected
  and silent.
- The sniffer never ACKs or transmits on the bike's bus. TWAI listen-only in
  the driver, and the transceiver's driver input tied recessive in hardware.
- CAN bitrate is unknown. Try 500k, then 250k, then 125k.
