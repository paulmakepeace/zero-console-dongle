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
2. **One file per MBB session.** A file opens when the first MBB lines are
   committed and closes five seconds after pin 8 goes low. Oldest files are
   deleted when free space runs low; the sizes are in the firmware
   [README](../firmware/README.md).
3. **Serve the files over WiFi.** A status page, a JSON list, download and
   delete. [`tools/pull-logs.py`](../tools/pull-logs.py) on the homelab fetches new files on a
   schedule and deletes each one after the size checks.
4. **A raw TCP console** on port 6638, tee'd into the capture. Transmit is
   gated by the transmit-pin rule below. Input arriving while the MBB sleeps
   is dropped, not queued.
5. **Time without a battery.** NTP when WiFi is up; otherwise the MBB's own
   stamps, which it prints on most lines, set the clock. Lines carry both.
6. **WiFi by provisioning.** No credentials in the build. With none stored,
   the dongle raises a setup access point and stores what is entered there.
7. **The poller.** A fixed command set, `status`, `charging`, `bms`, `pdu`,
   `in`, `faults`, on a slow schedule while the MBB is awake, 60 s by
   default, and on request. Each response ends at the `ZERO MBB>` prompt,
   which is the frame delimiter. A batch is a transaction: the transmit pin
   stays attached across every command, because the detach's own NUL makes
   the MBB print a prompt that a prompt-counting framer would take for a
   command's end; the poller waits 20 s after the MBB wakes, never runs
   while it sleeps, and stands aside for a console client, finishing the
   command in flight. Responses come back through the capture module's
   line queue: the prompt closes a command, lines the MBB prints on its own
   pass through to the log, and everything else is the command's output,
   kept in RAM and out of the log. The last output of each is served raw at
   `/api/cmd/NAME` and on the tabbed page at `/cmd`; the state of charge
   from `bms` and the bike state from `status` go into the status JSON.
8. **Light sleep between sessions.** Two things sleep in this design and
   the words mean different things for each. The MBB's two depths, shallow
   hibernation and deep sleep, are its own and are defined in the sleep
   section of [mbb-reference.md](mbb-reference.md). The ESP32's are
   Espressif's, in the
   [sleep modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
   reference:

   > **Light sleep**: the CPUs and most clocks stop, RAM and the
   > peripherals keep their state, and execution resumes on the
   > instruction after the sleep call. Wake sources include a timer and a
   > GPIO level. A few hundred microamps for the chip itself.
   >
   > **Deep sleep**: only the RTC domain stays powered; RAM and the
   > peripherals lose their state and a wake is a reboot through the ROM
   > loader. Tens of microamps for the chip, but a boot on every wake.

   The MBB announces `Hibernating for
   3600 sec` and wakes 3600 s later to the second, so the dongle keeps that
   line's time and, once the MBB has been asleep for a grace period with
   nobody using the dongle, light-sleeps until ten seconds before the MBB
   is due. The sleep timer runs on the ESP32's internal RC clock, which is
   a few percent off, so the wait is taken in chunks of at most ten
   minutes: each wake rejoins WiFi, NTP puts the clock right, and the next
   chunk is planned against wall time with a margin for the drift, so the
   last one lands before the MBB whatever the RC clock did. A chunk
   boundary is a short awake window; a pull that starts in it keeps the
   dongle up. Pin 8 rising wakes it regardless, for
   wakes it did not schedule, at the cost of the first bytes of the banner,
   because the UART runs from the APB clock, which stops in light sleep.
   With no announcement seen it wakes hourly anyway. WiFi goes down for
   the sleep and the join brings the services back; the sleep is noted in
   the log with its length and wake source. The ESP32 cannot wake from
   UART2, the port the MBB is on, so the UART is not a wake source. Light
   rather than deep sleep because the DevKit's regulator and USB bridge
   draw their few milliamps whatever the ESP32 does (see the power section
   of [hardware.md](hardware.md)), and light sleep keeps RAM and the
   peripherals as they were.

Phase 2, in likely order:

- **CAN as a second stream.** TWAI in listen-only mode, frames stamped and
  written raw in a candump-style line format for SavvyCAN or a script. Which
  bus is on pins 6 and 14, and at what rate, is the first thing it tells us.
- **Push instead of pull**, MQTT to the homelab, once pull has proven the
  files.
- **A fixed firmware other owners can flash** and configure from a browser:
  Improv WiFi with esp-web-tools.
- **Store shape for CAN and compression.** A second file per session needs
  a store handle per stream with one commit and reclaim policy, reclaim by
  session rather than by name so a session's files go together, and the
  listing grouped by session. Compression belongs at session close, not in
  the commit path, with the on-disk name and size staying authoritative and
  a flag in the listing the puller understands; the loss markers become
  framed records rather than spliced text. The buffers become two fixed
  arrays: an input ring the capture side writes into at one pointer and the
  compressor reads from at another, and an output array the compressor
  fills and the store commits by its own policy. A partial write is then
  charged to exactly the lines lost. Measured on 46 pulled sessions
  (676 KB): deflate with a 4 KB window gets 8.8x on whole files and 8.3x on
  independent 12 KB commit blocks, so blocks can be compressed as they are
  committed and the file never re-read. A dictionary trained on earlier
  captures adds little at that block size (8.9x with 8 KB, 10.1x with 32 KB
  and a 32 KB window) and earns its keep only on small blocks: 1 KB blocks
  go from 4.5x to 6.4x. If one is baked in, it is versioned by id in the
  file header, the puller holds each version, and the training set is
  scrubbed of the VIN and serials first, since a trained dictionary carries
  literal fragments of its input.
- **Boot-loop control.** Count boots that die inside 60 s in RTC memory, which
  survives a soft reset with no flash wear, and after three of them start
  in a safe mode with the filesystem and WiFiManager left out so the board
  stays reachable and flashable over USB. The boot counter itself moves to
  RTC memory with a periodic NVS write, so a loop cannot wear the NVS
  either.
- **A non-blocking boot connect.** WiFiManager's autoConnect at boot and
  its connect on a portal save each hold the loop task for about 20 s, the
  longest stalls the capture queue has to ride out. Starting the join and
  letting the network tick handle the result removes them. WiFiManager
  also registers its own update, restart and erase routes on the setup
  network whatever the menu shows; a replacement (Improv) closes that.
- **Network on its own task.** The HTTP server, the console and
  WiFiManager all run on the loop task, so a body sent one byte at a time
  holds the loop until the watchdog fires; the LAN-only posture makes that
  acceptable in phase 1. An async server, or the network on its own task with
  a queue to the store, removes the dependence. The console and the poller
  then need an explicit arbiter for the transmit pin.

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

- **The transmit pin.** Pin 9 is the MBB's wake pin: a high level reboots a
  sleeping MBB, and a UART idles high, so a held-high pin 9 keeps an awake
  MBB out of deep sleep. The pin is therefore attached to the UART only
  while bytes are being sent and for 2 s after, only while the MBB is awake
  and pin 8 is high at that instant, and is an input with a pull-down
  otherwise, from the firmware's first instruction and again on every
  restart; the ROM boot window before that is what the phase 2 10k pull-down
  covers. The hold is checked from both the capture task and the loop task
  so no single stall can hold the pin high, and it ends early the moment pin
  8 is seen low, within about 100 ms: the awake flag lags pin 8 by 5 s and
  must not be the transmit gate, or a keystroke as the hibernation line
  scrolls by would drive pin 9 into a MBB that is powering down. The drop
  back to the pull-down reaches the MBB as one NUL byte, which it answers
  with a fresh prompt. In light sleep every pad takes a sleep
  configuration unless told to keep its running one, so the transmit pin
  and pin 8 are told to, and stay pulled-down inputs. This is the one rule that keeps the dongle from
  waking the bike or holding it awake; every feature that sends anything
  goes through the same gate.
- The capture task only reads bytes, frames lines and watches the pins. It
  never takes a mutex and never writes flash: lines, loss markers and the
  awake and asleep edges go through a queue, in order, to the loop task,
  which does the clock parse and the store append. That is what keeps a
  slow filesystem from stalling the reader into a false sleep.
- The loop task never blocks on a network client. Console output to a client
  that cannot take it is dropped.
- Flash writes happen only after the MBB has been quiet for 3 s, plus at
  session end, with a 15 s or 24 KB bound, because a flash erase holds the
  UART interrupt off for longer than the receive FIFO covers and the
  precompiled core keeps that interrupt out of IRAM. Frunk USB dies at
  key-off without warning and the phase 2 supply is cut by a switch, so the
  bound is also the most a power cut can lose. The proper fix, a phase 2
  item, is the UART interrupt in IRAM: pioarduino's `custom_sdkconfig`
  rebuild with `CONFIG_UART_ISR_IN_IRAM=y` does not link against the
  precompiled core (`__wrap_log_printf`), so it needs a full core rebuild.
  What it buys, with a continuous full-rate stream, a worst case the MBB
  never produces: on a freshly formatted filesystem, where a commit only
  programs pages, 2179 lines in 20 s lose 2; on a filesystem in use, where a
  12 KB commit erases three sectors at about 45 ms each with the interrupt
  suspended, 1926 lines in 15 s lose 261, every loss marked. With the
  interrupt in IRAM the 16 KB ring absorbs any erase and neither number is
  above zero, and the quiet-time commit rule becomes an optimisation. Real
  MBB traffic is a few lines a minute with bursts of a dozen, which the
  quiet rule keeps clear of the erases.
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
