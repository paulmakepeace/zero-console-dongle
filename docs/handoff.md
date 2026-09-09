# Handoff

What a session picking this up needs that is not already in the ordered list
in the [README](../README.md) or in the per-topic docs: which threads are half
finished, what state the two boards are in, and how to verify a change.

## The two boards

`zero-dongle-ebdc` is the bench board: a DevKit and a CP2102 adapter facing
each other, no bike. `zero-dongle-a12c` lives in the tank compartment on the
bike. The code is the source of truth for which is which: `bench.py` refuses
to run against `a12c` by name, and `flash.sh` defaults to the bench. Both run
the same firmware. `tools/status.py` prints one line per board
and only lists a board that answers, so a missing bike line means the bike is
hibernating, not that anything failed. Boot times, UART overflow counts and
heap floor are on that line; the firmware [README](../firmware/README.md) has
the endpoints behind it.

The bench board is the one to flash first. `tools/bench.py` is the regression
and it needs the bench rig; it restores the operator's poll interval and sleep
settings on the way out, but a run killed partway leaves them as it found
them, so a scenario that fails for no visible reason is worth re-running from
a clean start before it is believed.

## Threads left open

- **The space reclaim at a hundred files.** The first item in the README's
  list and the measured entry in [open-questions.md](open-questions.md). Now
  addressed but not re-measured: `FS_MAX_FILES` caps the log count as a
  backstop, `dictCollect` reads dictionary ids from the file name rather than
  opening every file, and the status page urges a pull as the count climbs.
  The evacuation itself is the normal path: `tools/pull-logs.py` already
  deletes each file once it is safely stored, so it wants a timer (a cron or
  launchd job on the workstation), not a hand run. Left to do: re-measure the
  133-file case on the bench to confirm the walk is now sub-second, and have
  the bench clear old sessions above a file threshold so a long bench day does
  not walk into the stall.
- **The log-name format changed.** Session files are now
  `bBBBB-SSS-<when>-<dict>.log.z`, the dictionary's Adler-32 appended so the
  store can collect unused dictionaries without opening files. Before flashing
  this, drain both boards with `tools/pull-logs.py` so no old-format names are
  left on flash: an old name carries no id, so `dictCollect` reads it as
  naming no dictionary and could collect a dictionary an unpulled old file
  still needs. A full drain is a clean cutover and needs no migration code.
- **Absolute deadlines.** Decided, not open: the raw `millis()` comparisons
  are the wrap-safe elapsed form (`now - start >= interval`), and the two
  sites that store an absolute deadline use the deliberate signed-difference
  idiom in `mbb_uart.cpp`. A review of the whole tree found no live wrap or
  signed/unsigned bug, only one latent off-by-one in `parseHibernateSeconds`,
  now fixed. The proposed deadline type would not earn its complexity here, so
  it is not being built; reopen this only if a third bug of the class appears.
- **The clock decoupling.** Untouched, and now with a warning bought by a
  reverted attempt. Dropping the NTP step measurement from `clockTick` looked
  safe (its only visible output is a phrase in the sync note), but `bench.py`'s
  lightsleep scenario parses that `stepped +X.X s` value to check the RC sleep
  timer stayed inside `SLEEP_MARGIN_PCT`. It is load-bearing for the
  regression, not cosmetic, so any future simplification has to preserve or
  replace it. The larger piece stays open: the sleep planner reads wall time
  (`time(nullptr)`), so it depends on the sync being right; a monotonic
  elapsed counter would cut that dependency, but only if `millis()` advances
  across light sleep on this core, which the current design does not assume (it
  uses wall time because the RTC advances it across sleep). Settle that on the
  rig before touching it.
- **The longer cellular hold.** Five-minute holds do not make the module
  attach; the evidence and what stays open are in
  [open-questions.md](open-questions.md). The experiment to close it is
  `tools/ccm-wake-experiment.py` with a 900-second hold. Each wake costs about
  10 mV off a 13,000 mV battery and suppresses that hour's natural wake.
- **Why written corrections do not stick.** A research query is open on the
  question. Nothing in the repo waits on it.

## Verifying a change

Build and host tests are in the firmware [README](../firmware/README.md). The
host tests run on the Mac with the Arduino and LittleFS stubs under
`firmware/test/stubs/`, so `millis()` is a variable and nothing sleeps; the
whole suite is seconds. `tools/flash.sh` builds and flashes over the air.
`tools/bench.py` is the regression through the adapter and is the gate before
a version bump.

Three standing rules bound the work. Pushes are the operator's: commit, verify,
and hand over the command. Every version bump gets an annotated `vX.Y.Z` tag
whose body rolls up what changed, dated to the commit. Nothing that identifies
the bike reaches a tracked file; `tools/check-private.sh` is the pre-commit
gate and the README says how to install it in a fresh clone.
