# Working rules for this repo

These come out of the 2026-09-09 retro, and each cites the defect that bought
it. They are rules about how work is verified here, not about the firmware.

## Read the whole file before editing it

The firmware source is 3902 lines in total, about 43,000 tokens, so reading any
file whole costs nothing worth counting. Windows are for locating a thing, not
for deciding what to do to it.

**Why:** on 2026-09-09 an edit was made from a 17-line window of `http.cpp`.
The window did not show that the enclosing call holds the store's lock and an
open directory handle while it calls back, so the inserted capture tick could
commit to flash inside the directory walk that was reading it. It reached a
commit, stalled the capture stage for ten seconds on the bench, and was
reverted. Every review agent that found real defects had read whole files.

## Read the diff before claiming the fix

After any batch of edits, read `git diff` before writing what changed into a
commit message, a tag body or a message to the operator. A successful build is
not evidence that a particular edit landed.

**Why:** twice in one session a fix was reported as applied when the edit had
silently not landed. Once the flag was cleared before the thing it marked could
happen, so the code compiled and did nothing; once a batch aborted partway and
only some of it was re-applied. Both claims reached a tag body.

## Use `tools/apply.py` for batched edits

It applies all-or-nothing across files, refuses an ambiguous match the way a
proper editor does, prints a unified diff, and exits non-zero on any miss. Do
not hand-roll another string-replacement script: 531 of those were written in
one window, 263 carrying the same helper, and 75 printed their failure into a
scrollback where nobody read it.

A single delicate edit is better done with the dedicated Edit tool, which
cannot silently no-op.

## Use `jq` for a field, `tools/status.py` for a summary

`curl -s http://HOST/api/status | jq '.observed'` rather than piping into a
python one-liner. Raw `curl` to a board ran 230 times in one window, 178 of
them into a heredoc, against 19 uses of the summary tool. The tool was not
wrong; the ad-hoc case simply belongs to `jq`.

## State a cost in units before choosing the slower path

Before waiting, deferring, or avoiding an action because it costs something,
say what it costs in the units the operator cares about. If it is below the
noise floor, spend it.

**Why:** this is the most-repeated correction in the project and it has now
failed against three separate memory entries. On 2026-09-09 an hour's wait was
chosen over a 10 mV cost on a 13,000 mV battery, measured and reported by the
agent minutes earlier, and described in the same sentence as "free". A research
query on why written corrections of this class do not stick is open; until it
returns, this is a forcing function rather than a principle: the sentence with
the number in it has to be written before the choice is made.

## Keep it short, and apply a correction everywhere at once

A code comment defaults to one line; a release-note bullet to two or three.
Mechanism goes in the commit or tag body, not inline. And a correction is a
standing rule, not a one-off edit: apply it to everything already written and
about to be shown, in one pass.

**Why:** in one session the same over-long comment was written three times
after a clear correction that carried an example. Redundancy is a cost to
every reader, human and machine.
