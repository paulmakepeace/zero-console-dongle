# Dev process

The loop for changing this firmware, and how to tell a real failure from bench
noise before reacting to it. The verification rules behind the steps are in
[../CLAUDE.md](../CLAUDE.md).

## The cycle

1. **Dev, then show the diff.** Make the change, get the host suites green
   (`tools/test.sh`, seconds, no hardware), and show the diff (the Diff pane,
   `Cmd+Shift+D`). Nothing is committed yet.
2. **On comment or approval, commit.** The shown diff is the operator's window
   onto any thrashing as much as a review gate, so it goes up even for a change
   the author is sure of, and the pause to skim it is the point: it is where a
   loop gets caught and broken early. Commit only what has been shown, so a fix
   is never claimed before it is seen.
3. **Adversarial review, to convergence.** Run a full `/code-review high` over
   the change (whole files, every angle). Fix each significant finding, then run
   the review *again*: a significant defect is not cleared by its fix alone,
   because the fix is itself unreviewed code and often hides the next one (a
   dead, unwired feature sat behind an over-deletion bug here on 2026-09-10).
   Repeat until a full pass surfaces nothing significant. This gates the bench:
   no regression is run until the review has converged, so the bench is spent
   only on code a review already believes in. `/code-review ultra` is the
   heavier, operator-triggered cloud pass for a whole branch or PR.
4. **Run the regressions on the bench.** `tools/bench.py auto --host
   zero-dongle-ebdc.local` (`auto` picks the scenarios for the changed files;
   `all` is everything). This is the gate before a version.
5. **Green? Bump, tag, then hand off the push.** Bump `FW_VERSION` and commit
   it, make the annotated `vX.Y.Z` tag with a roll-up body dated to that
   commit, then hand over the push. The bump and tag come before the push, and
   the push is the operator's.
6. **Then either** go back to step 1 for the next change,
7. **or deploy to the bike** when the change is what an experiment or more data
   needs. Flash it by name, after draining it; a routine change does not go to
   the bike just because it passed.

## The two boards, and the one not to touch by accident

`zero-dongle-ebdc` is the bench: a DevKit and a CP2102 adapter facing each
other on the desk, no bike. `zero-dongle-a12c` is the bike, in the tank. The
code is the source of truth for which is which, not prose: `bench.py` refuses
any host containing `a12c`, and `flash.sh` defaults to the bench. When unsure,
`tools/status.py all` and read the signal (the bench sits by the router at
rssi around -30, the bike is far) and which board drives the adapter.

Never run `bench.py` against the bike: it holds the adapter line low, which on
the bike is the MBB's own output. Flash the bench first, every time; flash the
bike only by naming it (step 7).

## Reading a bench failure without oscillating

A red check in the bench step (step 4) is one of three things. Work them in this
order, and do not revert or patch until you know which:

1. **Flaky.** Re-run the one scenario from a clean start. Timing-tight checks
   (the transmit-pin release, the poll markers) fail under load and pass on a
   quiet re-run.
2. **The rig, not the firmware.** If it reproduces, flash the base
   (`git checkout main`, `tools/flash.sh`, run the scenario, then check the
   branch back out and reflash). A failure identical on base is the rig, not
   your change. Known rig issue as of this writing: the `break` scenario with
   the current CP2102N adapter, whose `TIOCSBRK` is not seen as a GPIO low on
   pin 8, so the pin releases on the 5 s awake timeout rather than the line
   drop, and the scenario can leave the board unreachable.
3. **A real regression.** Only when base is green and the branch is red. Then
   read the whole enclosing file, not a window, before changing anything.

## When a board wedges

A board that stops answering HTTP is recovered over USB without a power cycle,
through the DevKit's own bridge (`usbserial-0001`; the adapter is
`usbserial-3`):

    pio run -d firmware -e devkit -t upload --upload-port /dev/cu.usbserial-0001

It hard-resets over RTS and flashes whatever the working tree builds. After
the reboot mDNS can lag by a minute; the board answers on its IP, which the
last flash prints, before `ebdc.local` resolves again.

## Standing rules

- The adversarial review converges before the bench runs (step 3 before step 4);
  the bench is the gate before a version bump (step 4 before step 5).
- Every version bump gets an annotated `vX.Y.Z` tag, roll-up body, dated to
  the commit.
- Pushes are the operator's: commit, verify, hand the command over.
- Nothing that identifies the bike reaches a tracked file; `check-private.sh`
  is the pre-commit gate.
