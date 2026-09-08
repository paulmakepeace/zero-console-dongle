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
   while it sleeps or once it has announced its hibernation, and stands
   aside for a console client, finishing the command in flight. The
   announcement expires after a minute if the MBB stays up, a key-on
   inside the countdown, and a poll requested during it is refused rather
   than kept. A failed attempt never replaces the last good output; the
   listing says when each last succeeded and when it last failed. Responses come back through the capture module's
   line queue: the prompt closes a command, lines the MBB prints on its own
   pass through to the log, and everything else is the command's output,
   kept in RAM for the API and written to the log as well, behind a
   `dongle: poll` line per batch, so a session's polls read as a console
   transcript and the pulled files carry the bike's state every minute it
   was awake: a charge curve, a ride's pack temperatures. A batch is some
   10 KB raw, most of it text the dictionary already holds, and the poll
   interval setting is the volume control. The last output of each is served raw at
   `/api/cmd/NAME` and on the tabbed page at `/cmd`; the bike state and
   the BMS row of `status`, state of charge, pack voltage and current,
   negative while the pack is being charged,
   capacity and the pack's high and low temperatures, go into the status
   JSON. The ESP32's own die temperature is there too, some 15 to 20 C
   above the air around it, so it says more about the board than the
   frunk. The MBB also prints its answers to the cellular module's own
   commands on the console, `ltsm en mod 2` from the app for one, and an
   answer that lands inside a poll's is kept with that output rather than
   the log.
8. **Light sleep when the bike is unattended.** The dongle sleeps only once
   the bike has gone a configurable number of days, three by default,
   without any of the three lines that say it is looked after: a 12 V
   top-up from the pack, the cellular module answering, or the key
   turning on. That is the case the always-on supply has to survive, a
   bike parked for weeks under a cover with the module not answering,
   where the dongle's 20 mA would otherwise drain a 12 V battery that
   nothing is refilling; the rest of the time it stays awake and
   reachable. The time of the last such line is kept in flash, written
   while the MBB sleeps. The other trigger is the bike's own statement
   that it is parked: the MBB prints its long-term storage mode's state at
   every wake, `LTSM state: INIT to DIS` while it is off, and `bms`, which
   the poller runs, reports it as `storage mode Inactive`. Any state but
   DIS, EN as the bike spells it and DIS_PEND on the way out, or
   `storage mode Active`, arms the sleep at once whatever the
   days count, and a key-on forgets it until the MBB restates it at its
   next wake, so the dongle stays reachable for the hour after a ride.
   Nothing about storage mode is kept in flash: the MBB says it again
   within the hour, and the status JSON's `sleep.storage` shows what it
   last said. Two things sleep in this design and
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
   nobody using the dongle, light-sleeps for nine tenths of the time until
   the MBB is due, once per announcement. The sleep timer runs on the
   ESP32's internal RC clock, which runs a few percent long, and the tenth
   covers that with room to spare: the dongle is up a few minutes before
   the MBB, which costs a few milliamp-hours a day and nothing in code. It
   then stays up for the MBB rather than planning again on the remainder,
   because the sleep advances its own clock by the planned time, not the
   real one, so the remainder it reads is too long by the timer's error
   and a second sleep would land on or after the MBB. For the same reason
   a sleep makes the NTP fix stale, so the MBB's stamps may put the clock
   right before NTP does, and the NTP note in the log carries the size of
   its step, which is that error measured. Pin 8 rising wakes it
   regardless, for
   wakes it did not schedule, at the cost of the first bytes of the banner,
   because the UART runs from the APB clock, which stops in light sleep;
   a pin 8 wake leaves the plan open, so a glitch with no MBB session
   behind it does not cost the rest of the hour. A sleep is never entered
   with pin 8 high or without its timer, and a transfer counts as use to
   its end, so a pull that runs past the grace is not cut off.
   With no announcement seen it wakes hourly anyway. The WiFi driver is
   stopped for the sleep and started after, never torn down, so its
   buffers are not re-allocated into a fragmented heap; a start that
   fails is counted in the status and retried; the sleep is noted in
   the log with its length and wake source. The ESP32 cannot wake from
   UART2, the port the MBB is on, so the UART is not a wake source. Light
   rather than deep sleep because the DevKit's regulator and USB bridge
   draw their few milliamps whatever the ESP32 does (see the power section
   of [hardware.md](hardware.md)), and light sleep keeps RAM and the
   peripherals as they were.

9. **Compressed session files, with a dictionary the dongle learns.** A
   session file is one zlib stream: deflate blocks with fixed Huffman
   codes as lines arrive, a sync flush (an empty stored block) at every
   commit so the file decodes up to the last commit after a power cut,
   and the Adler-32 trailer at session close. Names end `.log.z`. The
   stream's window is a dictionary followed by 8 KB of history, so a line
   can match text from this file or from the dictionary alike, and the
   header names the dictionary by its Adler-32, as zlib's FDICT does.

   The dictionary is the dongle's own: the lines that keep coming back on
   this bike, learned from its sessions. Every MBB line is stripped of its
   stamps and looked up; one already in the dictionary is marked as used,
   a new one is kept as a candidate. At a session's end with the MBB
   asleep, if the session brought more than 1 KB of new lines and the
   last rebuild is over an hour old, the dictionary is rebuilt as the
   proven lines first, then the new, then the rest, to 6 KB, verified on
   the flash as `dict-<id>.txt`, and used from the next session on; the
   session header carries the id too. The proven marks age, so a line the
   bike stops printing falls off. Dictionary files are never
   reclaimed for space; one is deleted only when no session file on the
   flash names it and it is not the one in use. The puller fetches a
   dictionary by id the first time a file needs it and keeps every version
   beside the files, so a lost session file costs only itself and a lost
   dictionary costs only the files that named it. A fresh board starts
   without one and learns it from its first sessions.

   The measurements behind this, per file and against the naive design and
   a baked-in dictionary, are in [compression.md](compression.md), with
   what a rebuild costs the flash. The compressor is uzlib's, patched for
   a fixed output buffer and the flushes (see `firmware/lib/uzlib/NOTES`),
   on fixed arrays: the dictionary, history and line window, a hash table,
   a 4 KB output buffer, the candidate buffer, about 34 KB in all and none
   of it on the heap. Lines are
   compressed as they arrive, so the buffer that waits for a commit holds
   compressed bytes and the commit rule is 3 s of MBB quiet, 15 s, or a
   full 4 KB buffer. A short write breaks the stream, so that part closes
   and the session continues in the next one, with the lines that were
   waiting counted as lost.

Phase 2, in likely order:

- **CAN as a second stream.** TWAI in listen-only mode, frames stamped and
  written raw in a candump-style line format for SavvyCAN or a script. Which
  bus is on pins 6 and 14, and at what rate, is the first thing it tells us.
- **Push instead of pull**, MQTT to the homelab, once pull has proven the
  files.
- **A fixed firmware other owners can flash** and configure from a browser:
  Improv WiFi with esp-web-tools.
- **Store shape for CAN.** A second file per session needs a store handle
  per stream with one commit and reclaim policy, reclaim by session rather
  than by name so a session's files go together, and the listing grouped
  by session; the loss markers become framed records rather than spliced
  text.
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

The source is one file per owner, each holding the state only it touches:
`mbb_uart.cpp` the capture task, the pins and the transmit gate;
`store.cpp` the files, the compressor and the dictionary; `clock.cpp` the
time and where it came from; `poller.cpp` the command outputs;
`sleep.cpp` the sleep decision; `settings.cpp` what is kept in flash and
applied live; `wlan.cpp` the join, the setup network, mDNS and the
services' up and down; `http.cpp` the web server; `console.cpp` the TCP
console; `main.cpp` the loop, the watchdog and the order the owners tick
in. The logic under `src/pure/` has no owner state and runs on the host.

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
  session end, with a 15 s bound from the first waiting MBB line and the
  4 KB output buffer as the hard one; the space reclaim also runs regardless
  once free space is under half the reserve. The other flash writes are
  the sleep policy's attended time, which waits for the MBB to sleep, and
  a settings save, which happens at once: a hand action, rare, and what it
  can lose is marked. That is because a flash erase holds the
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
  interrupt in IRAM the 8 KB ring absorbs any erase and neither number is
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
