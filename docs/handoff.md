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

- **Reclaim: verify, and cap the bench.** The reclaim is implemented (a
  file-count cap, and the dictionary id read from the file name instead of
  opening each file). Left to do: re-measure the ~100-file case on the bench to
  confirm the walk is now sub-second, and have `bench.py` clear old sessions
  above a file threshold so a long bench day does not walk into the stall. When
  this first reaches the bike, drain it with `tools/pull-logs.py` before
  flashing, since old-format names carry no dictionary id.

- **The clock decoupling.** Bench-gated. The sleep planner reads wall time
  (`time(nullptr)`), so it depends on the MBB/NTP sync being right; moving it to
  a monotonic elapsed counter would cut that dependency, but only if `millis()`
  advances across light sleep on this core, which the current design does not
  assume (it uses wall time because the RTC advances it across sleep). Settle
  that on the rig before touching it.

- **The longer cellular hold.** Whether a 900-second hold makes the module
  attach where five minutes did not; the experiment is
  `tools/ccm-wake-experiment.py` and the evidence is in
  [open-questions.md](open-questions.md). Each wake costs about 10 mV off a
  13,000 mV battery and suppresses that hour's natural wake.
