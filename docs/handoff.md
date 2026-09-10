# Handoff

The live to-do list, and the context a session needs that is not in the code,
the [README](../README.md) ordered list, or the per-topic docs. Not a journal:
a finished item comes out, and anything durable in it moves into `docs/` first.
Which board is which, and how a change is verified, are in
[dev-process.md](dev-process.md).

## To do

- **Test the setup SSID naming on a second unit.** The setup network is now
  named `Zongle`, or the lowest free `Zongle N` when another `Zongle*` is in
  range (`wlan.cpp` `zongleSsid`), and the pages show the brand while the host
  keeps its MAC. Untested with two units and a phone: iOS remembers a captive
  SSID it has joined and may mark it succeeded, so a second unit needs checking,
  whether the captive sheet still appears, whether iOS treats `Zongle 2` as new,
  and whether re-provisioning the first unit is confused by the remembered join.
  Also confirm the ~2 s scan at setup does not bother the boot path.

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
  event-stream desync, or a path not found by reading). On the bike pin 8 is the
  MBB's driven output, so the real-world trigger differs from the bench's
  floating pin.

  Groundwork shipped in 0.11.11 so the next flood is diagnosable without a
  console: `/api/status` now carries `uart.awake_edges`, the count the capture
  task posts, beside the existing `awake_count` the loop records. In a flood the
  two discriminate the mechanism: **they track together** means the awake edge
  really is toggling at that rate (the sample logic, against reading); **the
  delivery `awake_count` races far past `awake_edges`** means the loop is reading
  a desynced event stream, manufacturing edges the capture task never posted. The
  `onState` console print is now coalesced to at most one line a second (a
  suppressed-count summary), which both bounds the loop starvation on the 115200
  console drain and keeps HTTP reachable so those counters can be read live during
  the storm. Still to do once a repro is in hand: fix the identified mechanism,
  and bound the flash side (`storeSessionClose()` runs per asleep edge) if the
  storm turns out to toggle rather than desync.

- **Reclaim: the per-file open is gone, but the walk count is not, and it can
  still stall ~1 s near capacity.** The name-parse walk replaced the header
  open per file (the old ~33 ms/file, 1158 ms over 35 files), which was the big
  win. But a real measurement at a hundred files on the bench (2026-09-10, a
  temporary `/api/debug/reclaimbench` since reverted) contradicts the earlier
  single-point extrapolation of "~120 ms at 100 files":

  | files | one directory walk | worst-case reclaim (~7 walks) |
  |------:|--------------------:|------------------------------:|
  |    57 |               51 ms |                        353 ms |
  |   100 |              138 ms |                      ~970 ms |
  |   130 |              188 ms |                       ~1.3 s |

  One walk is ~1.4 ms/file (slightly super-linear as `readdir` scans a fuller
  directory), but `ensureSpace` walks the directory up to seven times per
  reclaim (`dictCollect`'s up-to-three walks, plus one per delete round, up to
  four), all under the store lock the capture stage takes. Worst case crosses a
  second around 130 files, and the bike reaches ~100 in about four days without
  a pull. So the reclaim is faster than before but not sub-second worst-case at
  the file counts this will actually see. The fix at the right depth is to cut
  the walk count (one pass that both collects dictionaries and finds the oldest,
  rather than three-plus-one-per-round), or to cap the directory so the walk is
  never long; the removed file-count cap was a blunt version of the latter. The
  code comment at `store.cpp` "measured ~1 ms/file, so a large directory no
  longer stalls the capture stage" now overstates the result and should be
  corrected with these numbers.

  Also still open: have `bench.py` clear old sessions above a file threshold so
  a long bench day does not accrete a huge directory, and, when this first
  reaches the bike, drain it with `tools/pull-logs.py` before flashing since
  old-format names carry no dictionary id.

- **The clock decoupling: deferred, pending an actual problem.** The sleep
  planner reads wall time (`time(nullptr)`), so it depends on the MBB/NTP sync
  being right; a monotonic elapsed counter (like the boot-file naming) would cut
  that dependency, and the rig question that gated it is answered (`millis()`
  advances across light sleep, so the move is viable). But it changes
  safety-relevant sleep timing for no observed failure, so it stays parked until
  a real sync problem shows up rather than being done pre-emptively. The
  lightsleep bench check that used to flag this already leans on the millis-based
  margin, not the flaky NTP step, so nothing is waiting on it.

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
