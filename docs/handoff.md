# Handoff

The live to-do list, and the context a session needs that is not in the code,
the [README](../README.md) ordered list, or the per-topic docs. Not a journal:
a finished item comes out, and anything durable in it moves into `docs/` first.
Which board is which, and how a change is verified, are in
[dev-process.md](dev-process.md).

## To do

- **The network wedge (the "went dark" bug).** Under some marginal pin-8
  condition the board floods `mbb: awake` on the console at ~120 KB/s and the
  loop task starves until HTTP and the network go dark; it does not self-recover
  (no watchdog reboot). This is the open-questions "went dark, needed a key
  cycle". Seen on the bench when a closed adapter port left pin 8 floating, but
  it is **intermittent**: the same close, and a deliberate sustained break, both
  often leave the board fine, so it is not reproducible on demand, which is the
  blocker on fixing it. The flood is `onState()`, but `nowAwake` is gated on the
  5 s `SLEEP_AFTER_MS` threshold and cannot legitimately flip at that rate, so
  it is not a simple awake-flap and the real mechanism is not yet identified (an
  event-stream desync, or a path not found by reading). Next: add a counter at
  the `EV_AWAKE` post site and in `onState`, ship it, and catch the flood in the
  wild since the bench trigger is unreliable; a rate-limit on the edge
  logging/posting would bound the wedge regardless of the mechanism. On the bike
  pin 8 is the MBB's driven output, so the real-world trigger differs from the
  bench's floating pin.

- **Reclaim: two loose ends.** The stall is fixed and measured: the reclaim
  walk reads the dictionary id from the file name instead of opening each file,
  timed 2026-09-09 at ~1 ms/file against ~33 ms/file for the old header open (35
  files: 33 ms vs 1158 ms), so it no longer stalls the capture stage, and the
  file-count cap that was a backstop for it has been removed (the byte reserve
  is the limiter again). Left to do: have `bench.py` clear old sessions above a
  file threshold so a long bench day does not accrete a huge directory, and,
  when this first reaches the bike, drain it with `tools/pull-logs.py` before
  flashing since old-format names carry no dictionary id.

- **The clock decoupling.** A decision, not a blocker any more. The sleep
  planner reads wall time (`time(nullptr)`), so it depends on the MBB/NTP sync
  being right; a monotonic elapsed counter would cut that dependency. The rig
  question that gated it is answered: `last_slept_s` (millis-measured) reads
  about the sleep duration, so `millis()` advances across light sleep on this
  core (the IDF adjusts it on wake), and the move is viable. What is left is the
  call to make it: it changes sleep timing, which is safety-relevant on the bike
  (an over-long sleep misses the MBB's wake), so weigh it deliberately rather
  than fold it into a routine change. The lightsleep bench check that used to
  flag this is fixed: it leans on the millis-based margin now, not the flaky NTP
  step.

- **The longer cellular hold, likely moot.** Whether a 900-second hold makes
  the module attach where five minutes did not. But the bike's `ccm` output
  reads a T-Mobile SIM (mcc 310, mnc 260) with zero registration and zero
  signal while GPS is valid, and T-Mobile shut its 3G network in mid-2022, so
  this looks like an original 3G module that no network will take, and no hold
  will change that. Confirm the modem generation first (the Telit model is not
  in `ccm`; a Telit AT query or the physical label would give it) before
  spending battery on the experiment. `tools/ccm-wake-experiment.py`, each wake
  about 10 mV off a 13,000 mV battery, and it suppresses that hour's natural
  wake.
