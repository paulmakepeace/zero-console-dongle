# Releases

Reverse chronological. Each heading is the firmware tag; the date beneath it is
the tagged commit's date. This tracks feature work and significant bug fixes;
minor goofs and one-off tidy-ups are left to the tag bodies.

## 0.12.0
September 11, 2026

- The dongle pushes its session files to an archive service on every WiFi
  join and every session end. `push_url` in the settings names it; each file
  is PUT as the flash holds it and deleted on the server's 200, and a
  dictionary the server lacks is sent on demand. Off with no URL set.
- `server/` is that service for the NAS: one container holding the push
  endpoint, the ingest into SQLite and the MCP server over HTTP.
  `tools/zlog.py` is the session-file library the puller, the ingest and the
  service share.

## 0.11.13
September 10, 2026

- IPv6 enabled on the station so mDNS answers AAAA. Without it a dual-stack
  `.local` lookup stalled about five seconds on the missing answer every
  request; now about ten milliseconds.

## 0.11.12
September 10, 2026

- The log reclaim runs in a single directory walk, without a full-filesystem
  traversal per delete. A deep reclaim at 150 files drops from 4.3 s to 1.4 s;
  deletion behaviour and the safety bails are unchanged.

## 0.11.11
September 10, 2026

- Network-wedge groundwork: `/api/status` reports `uart.awake_edges` beside the
  loop's `awake_count`, so a flood's mechanism (real toggling versus an
  event-stream desync) is readable without a console.
- The `onState` console print is coalesced to at most one line a second, keeping
  the loop and HTTP responsive during a flood so those counters can be read live.

## 0.11.10
September 10, 2026

- The 12 V amps become first-class readings. `Total_Current` (total 12 V load)
  and `DC-DC_to_Batt` (charge into the 12 V battery, valid only while charging)
  are read from the `pdu` table into the `12v` group, so the 12 V system shows V
  and A together the way the traction pack already does.

## 0.11.9
September 9, 2026

- Friendly "Zongle" name. The pages and the setup-network SSID show "Zongle";
  the SSID scans for existing `Zongle*` APs and uses the lowest free `Zongle N`
  when one is already in range, so a lone dongle stays clean and two are told
  apart in the WiFi list. The host, mDNS and log names keep the MAC and do not
  move, so bookmarks hold.
- Trimmed the NTP clock-step machinery down to plain source promotion, dropping
  the step measurement and the redundant race re-query (about 19 lines out).

## 0.11.8
September 9, 2026

- On a successful setup join the page shows a full-width "Open `<name>`.local"
  button as the clear next step, raising iOS's "Open in Safari?" handoff to the
  real browser.
- Removed the log-count cap. It backstopped the reclaim stall that 0.11.7 fixed;
  measured on the bench the in-use scan is about 1 ms/file (name parse) against
  33 ms/file (header open), so the byte reserve is the limiter again and
  retention is restored.

## 0.11.7
September 9, 2026

- Reclaim stops opening every file. The session filename carries its
  dictionary's Adler-32, so the store collects unused dictionaries by reading
  names instead of opening every file, removing the bulk of the ten-second stall
  past a hundred files.
- Removed the inert observed/assumed timing block from the status JSON and the
  measurements behind it: read only by eyes, never actioned.

## 0.11.6
September 9, 2026

- The constants that describe the MBB rather than the dongle are grouped under a
  heading saying so, each carrying its measurement, and the status reports what
  the board observed beside what was assumed. Two of the four measurements were
  wrong when written and are corrected (the boot now times itself; the asleep
  delay measures the bike, not the dongle's own threshold).
- Host tests for the poller: fourteen cases with the filesystem and Arduino
  stubbed, no hardware.

## 0.11.5
September 9, 2026

- A POST to `/update` whose body is not multipart took the raw path and faulted
  the board into a panic; it now checks the content type. A multipart POST with
  no file part falsely reported "ok, rebooting"; only an upload that actually
  began can report a flash, and a failure answers 400 or 500.
- A sleep could be planned for months from a mangled hibernate digit or a clock
  stepped backward under a plan held in wall time, the one outcome the sleep
  feature exists to prevent; it is now clamped to the fallback hour.
- The provoked-wake flag (a session the dongle itself caused must not count as
  the bike being attended) was inert and now lasts as long as pin 9 is driven.
- The UTF-8 check rejects what UTF-8 forbids (surrogate halves, overlong
  encodings, lead bytes past the last code point) that were passing unescaped
  into the JSON.
- An OTA upload froze the poller's timeout, holding the transmit pin attached
  and pin 9 high for the whole upload; the upload now lets a stalled batch time
  out as a download does.

## 0.11.4
September 9, 2026

- A wake's deadline, kept past expiry, came back true 24.9 days later when
  `millis()` wrapped past it and idled pin 9 high for 24.9 days, holding the MBB
  out of hibernation with its console block powered and draining the bike's 12 V
  battery; the deadline is now forgotten the moment it expires.
- A wake is refused while the MBB is awake, so it cannot reset one that is
  charging the 12 V battery and abandon the top-up.
- The reclaim's "keep the eight oldest" insert took its full shortcut on the
  eighth name too, an out-of-bounds stack read that produced a phantom name and
  poisoned the following rounds' candidate set.
- A correct password typed at the edge of range is no longer erased on a single
  handshake timeout; only an authentication failure clears it.

## 0.11.3
September 9, 2026

- A batch started within seconds of a console client leaving met the MBB's last
  prompt still in the framer and closed its first command on it, shifting every
  slot by one; a batch now waits for the line to be quiet past the prompt's
  flush time.
- A poll requested while the MBB slept fired at the wake into the boot chatter,
  skipping the settle; only a request made while awake skips it.
- The prompt now leaves the framer the instant it arrives rather than after the
  idle flush, so a command closes the moment it is answered and a thirteen
  command batch takes seconds rather than half a minute.
- `/api/wake?hold=S` drives pin 9 high whatever pin 8 says, waking a hibernating
  MBB and keeping it up: the lever for the cellular check-in experiment.

## 0.11.2
September 9, 2026

- The saved batch's header put the name first, and a name with a space (`bms
  interface`, `dash info`) stopped the whole load; the name is now last, so after
  a reboot all thirteen commands and their readings come back.
- A row whose Valid column says No leaves the last good value in place.
- `obd` and `performance` join the batch, so the faults group and total Wh fill.

## 0.11.1
September 9, 2026

- The first large batch overflowed the UART FIFO on both boards: 17 KB of
  answers fills the output buffer mid-batch, and the forced flash erase holds the
  UART interrupt off longer than the FIFO covers. The poller now writes the lines
  so far at each command's close, while the MBB waits for the next command and
  the line is quiet.

## 0.11.0
September 9, 2026

- A readings table keyed by console row name, fed from every framed line whatever
  carried it (a batch, a typed command, or the MBB on its own), with a parser for
  the console's two shapes (comma tables and dash lists) and units pinned per
  name. Surfaced at `/api/readings` and as a grouped Bike table at the top of the
  main page.
- The poller runs eleven commands, about 17 KB raw a batch, with the interval as
  the volume control.

## 0.10.1
September 8, 2026

- A poll command typed on the console is kept from its answer as it goes by: a
  poller watch slot opens on the console client's echo and closes on the prompt,
  so the command page is as fresh as the session with no added MBB traffic.

## 0.10.0
September 8, 2026

- WiFiManager replaced by the driver's own join and a setup page of ours. The
  join is started and never waited for, so a boot with the network away no longer
  holds the loop task for 20 s. The setup network is an open soft AP up beside the
  station (BOOT held at power-up, on a refused key, or after ten minutes unjoined)
  and answers a phone's connectivity probe with the setup page, so the sheet opens
  by itself.
- A key that has joined before is retried every five minutes; a key that never
  joined is erased on its first refusal. Flash and static RAM both drop with the
  library gone.

## 0.9.5
September 8, 2026

- The store commits at every batch end; the bench picks its scenarios by what
  changed.

## 0.9.4
September 8, 2026

- "Use", the ten-minute window that holds the sleep off, is now a page opened or
  an action taken over HTTP; a page's own background refreshes no longer count, so
  a tab left open cannot hold the dongle awake forever.
- A read-only audit of every statement about sleep, use, the poller, storage mode
  and the status fields against the code fixed 23 discrepancies.

## 0.9.3
September 8, 2026

- A request other than a status check now counts as use for ten minutes rather
  than 30 s.

## 0.9.2
September 8, 2026

- The board's identity (node name and setup-network password) is derived once
  from the eFuse MAC instead of from three separate reads. No behaviour change.

## 0.9.1
September 8, 2026

- Split-review rework: the console never blocks the loop on a client (greeting,
  slots-full and input notes all go through the non-blocking send); `/api/wifi/reset`
  verifies the erase and retries it; `/update` restarts only when an image was
  actually written; settings over their length are refused with a 400 and nothing
  applied.

## 0.9.0
September 8, 2026

- Every poll batch goes into the log behind a "dongle: poll" line, so the pulled
  files carry the bike's state every minute it was awake: a charge curve, a ride's
  pack temperatures, the storage-mode state. The poll interval is the volume
  control.

## 0.8.1
September 8, 2026

- `net.cpp` split into its owners (`wlan`, `http`, `console`, `settings`), a pure
  move with no behaviour change.

## 0.8.0
September 8, 2026

- Storage mode arms the sleep at once whatever the days count, taken from the
  MBB's own statement that the bike is parked (an LTSM state other than DIS, or
  storage mode Active in the bms snapshot).
- Sleep rework: one timed sleep per announcement, since the dongle advances its
  clock by the planned time and a second plan on the remainder had landed on or
  after the MBB; a sleep is never entered with pin 8 high or without its timer.

## 0.7.1
September 8, 2026

- Sleep for nine tenths of the wait instead of timing the wake.

## 0.7.0
September 8, 2026

- Session files carry a dictionary the dongle learns from its own sessions.
- Hardware: the RTC crystal identified as the phase-2 fix for the sleep timer.

## 0.6.2
September 8, 2026

- Sleep only once the bike has gone days unattended.

## 0.6.1
September 8, 2026

- Log usage and the pack row added to the status; sleep off by default.

## 0.6.0
September 8, 2026

- Session files become gzip streams, compressed line by line on fixed arrays.

## 0.5.0
September 7, 2026

- The poller and the command page; light sleep timed from the MBB.

## 0.4.5
September 7, 2026

- The pure layer extracted, with host tests for it and for the pull script.

## 0.4.4
September 7, 2026

- Tooling for the retro's first four rows; the session header names the board.

## 0.4.3
September 7, 2026

- The transmit gate follows pin 8 itself, edges are never dropped, and cross-task
  counters are atomic.

## 0.4.2
September 7, 2026

- One portal-and-services state machine, live settings, and console fixes.

## 0.4.1
September 7, 2026

- Review round three: the store keeps honest counts and sortable names.

## 0.4.0
September 7, 2026

- Review round two: the capture task frames lines and hands off, nothing more.

## 0.3.2
September 7, 2026

- A rolled session names the file it continued from.

## 0.3.1
September 7, 2026

- No timed setup network; BOOT held at power-up raises it instead.

## 0.3.0
September 7, 2026

- Review round one: clock policy, watchdog feeding, naming, settings, and the
  network doors.

## 0.2.7
September 7, 2026

- Task watchdog on the loop and capture tasks; reset reason and WiFi disconnects
  in the status.

## 0.2.6
September 7, 2026

- Each board names itself from its MAC.

## 0.2.5
September 7, 2026

- Partition table set to 1.5 MB app slots and 896 KB for logs.

## 0.2.4
September 7, 2026

- Commit after 3 s of quiet.

## 0.2.3
September 7, 2026

- The session header carries the first line's time.

## 0.2.2
September 6, 2026

- Commit lines to flash only while the MBB is quiet.

## 0.2.1
September 6, 2026

- Second review pass: the TX race and the NUL-wake closed, console back-pressure,
  portal shutdown, overflow and format accounting.

## 0.2.0
September 6, 2026

- Review fixes: the transmit pin is attached only while sending, the pins are made
  safe first, and no loss is silent; the deep-sleep wake.

## 0.1.1
September 6, 2026

- A quiet session is renamed once the clock is known; the file is closed before an
  OTA restart.

## 0.1.0
September 6, 2026

- Phase-1 firmware: pin 9 is the wake pin, and power goes always-on from pin 16.
