# Handoff

What a session picking this up needs that is not already in the ordered list
in the [README](../README.md) or in the per-topic docs: which threads are half
finished, what state the two boards are in, and how to verify a change.

## The two boards

`zero-dongle-a12c` is the bench board: a DevKit and a CP2102 adapter facing
each other, no bike. `zero-dongle-ebdc` lives in the tank compartment on the
bike. Both run the same firmware. `tools/status.py` prints one line per board
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

- **Working rules.** [`CLAUDE-pending.md`](../CLAUDE-pending.md) at the root
  holds five rules about how work is verified here, each citing the defect
  that bought it. They are waiting on a `CLAUDE.md`, which is the operator's
  to write. Merging them is deliberate, not automatic.
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
- **Absolute deadlines.** Twenty-nine time comparisons in the firmware are
  raw `millis()` arithmetic. Two separate bugs this project has hit were the
  same shape: a deadline kept after it expired, which comes back true when
  `millis()` wraps at 24.9 days, and a signed-versus-unsigned comparison. The
  live sites are correct now and `mbb_uart.cpp` carries the comment saying
  why. A small deadline type that cannot be stored expired, with the wrap
  comparison in one place, would retire the class. Proposed, not built.
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
