# Handoff

The live to-do list, and the context a session needs that is not in the code,
the [README](../README.md) ordered list, or the per-topic docs. Not a journal:
a finished item comes out, and anything durable in it moves into `docs/` first.
Which board is which, and how a change is verified, are in
[dev-process.md](dev-process.md).

## To do

- **The awake-flap network wedge (the "went dark" bug).** A pin-8 condition
  that flaps the line rapidly makes `onState()` log and post an edge on every
  transition with no rate limit; the flood (seen at ~120 KB/s of `mbb: awake`
  on the console) starves the loop task until HTTP and the network go dark, and
  the watchdog does not catch it because nothing is strictly hung. Reproduced
  on the bench by leaving the adapter line marginal. This is the open-questions
  "went dark on the network, needed a key cycle". Fix: put hysteresis on the
  awake decision and rate-limit the edge logging the way the UART markers
  already are, so a flapping line cannot flood the loop.

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
