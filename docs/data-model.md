# The data model: what the log is for, and where the data goes

The store captures the console as a transcript, one stamped line per line the
MBB prints. That was the right first job. This note says what the transcript is
for, which parts of it carry the value, and where the data should live once it
leaves the flash, so that storage, compression, rendering and the agent
interface are decided against one model rather than one at a time.

## What the transcript contains

Measured over the pulled corpus of 71 sessions and 37,752 lines:

| Part of the transcript | Share of lines | What it is |
|---|---|---|
| Poll batch output | over 95 % | the same 13 command tables every 60 s, already parsed on the board into the readings table |
| MBB narration | under 1 % | the bike's own stamped lines: state changes, LSS, contactors, faults, the hibernate directive |
| Dongle lines | under 1 % | session start and end, clock, sleep, poll markers, loss markers |
| Banners, prompts, echoes | the rest | framing around the above |

One 30-minute awake session is 12,313 lines and 852 KB raw, of which 29 poll
batches are about 12,000 lines. At the store's measured 3.4x that is about
250 KB of the 896 KB log partition, so an awake bike fills the flash in under
two hours. The compression ceiling work in
[compression-ceiling.md](compression-ceiling.md) exists to shrink those batches
after they have been serialised as text. The cheaper move is not to serialise
them.

## What the data is for

Three streams, with different lifetimes and different consumers:

- **Events.** The MBB's narration and the dongle's own lines, verbatim, with
  both stamps. This is the bike's event log, the same content owners read in
  the app's export as zerologs.bike renders it, complete rather than a ring
  buffer that hourly wakes overwrite. Keep it forever; it is tiny.
- **Readings.** The figures in the poll tables as numbers over time: the charge
  curve, pack and motor temperatures, the 12 V rails, state of health, cell
  signal. Every item in [owner-asks.md](owner-asks.md) is one of these. Wanted
  per minute for the hours of a charge or a ride, and once a day for years.
- **Snapshots.** The full text of a command's output, kept for what the
  readings whitelist does not carry and for learning what a field means.
  Wanted at a session's start, on a change to `faults` or `obd`, and at a slow
  cadence otherwise. Nobody needs the 29th identical table of a session.

Byte-accurate reproduction of the console is a fourth use, for reverse
engineering the console itself. That is `tools/capture.py` on a laptop with a
real terminal, not the store.

## The on-flash format, when it changes

Not built. The direction is JSON Lines through the unchanged compressor and
dictionary: one wide object per poll batch carrying the 50 readings, one
object per event with the dongle stamp, the MBB stamp when the line had one,
and the text, and a snapshot object for a command's text at the slower
cadence. Estimated per batch, against today:

| One poll batch | Raw | After the existing deflate |
|---|---|---|
| Command text, today | 17 KB | about 5 KB |
| CSV row, header once per session | 0.3 KB | about 0.15 KB |
| JSON object, keys repeated | 1.7 KB | about 0.25 KB |

The keys repeat exactly every record and the previous record is always within
the 8 KB history, so deflate removes them and JSON costs within 100 bytes of
CSV while staying readable with `jq` and a stdlib Python script. An awake hour
then costs about 15 KB on flash against about 300 KB today.

Two consequences. The learned dictionary and the compressor stay as they are;
the transform in [compression-ceiling.md](compression-ceiling.md) is parked
because its input disappears. And once content is this small the 4 KB
filesystem block is the dominant waste: a parked day is 24 wake files of about
1 KB in 24 blocks, 96 KB for 24 KB of content, so
[log-rotation.md](log-rotation.md) design b2 is what buys parked retention,
from about nine days to about a month. Whether that matters depends only on
the pull cadence.

## Where the data lives

Three tiers, not alternatives:

- **The dongle is a buffer.** It holds what arrived since the last pull,
  serves the live state and, later, the current session's chart from a RAM
  ring of readings rows, about 12 KB an hour. It is never the archive.
- **The homelab is the archive.** `tools/pull-logs.py` already runs there and
  inflates each session. `tools/logdb.py` loads the inflated sessions into one
  SQLite file, which the MCP server, a panel and a page all read. A year of
  this bike is a few megabytes. No new infrastructure, one owner's data,
  private.
- **A phone or browser is a view.** The dongle's pages on the LAN, the archive
  away from it. An owner without a homelab gets the dongle's pages and a
  JSON export, which is the fixed-firmware case on the phase 2 list. Data
  held on the owner's phone, backed up by the phone's own cloud, is the shape
  that keeps it private without anyone running a service; its data flow is
  open.

A hosted service enters only with other owners' data and a reason to carry
auth, uptime and the exposure noted in [sources.md](sources.md). The MQTT push
on the phase 2 list is the seam that keeps that move cheap if it ever comes.

## The archive schema

`tools/logdb.py` reads the pulled `.log` files and writes `logs/dongle.db`.
Ingest is idempotent per file, keyed on name and size, and a session whose
file has been deleted from the directory is dropped. Tables:

- `sessions`: one per file; board, session id and part, boot count and reason,
  firmware, first and last stamp, line and batch counts.
- `lines`: every line, with its dongle stamp as ISO text and as an epoch in the
  dongle's zone, a kind (`event`, `dongle`, `mbb`, `echo`, `output`, `prompt`),
  the batch and command it belongs to, and the MBB stamp for events. An FTS5
  index over the text.
- `batches`: one per `dongle: poll` line or per command typed on the console.
- `commands`: one per command answered inside a batch, with its full output
  text. This is the snapshot stream.
- `readings`: every parsed row of every command output, normalized as (time,
  command, name, value, valid), so a series is one query and the whitelist in
  `readings.cpp` is not a limit here. Units come from that whitelist where it
  has the name.

Times are the dongle's local stamps as written; range arguments are ISO
prefixes (`2026-09-10`) or relative (`-7d`, `-12h`).

## The MCP server

MCP is a protocol between an AI client and a server that advertises tools, each
a function with a schema and a result. The client here is Claude Code or Claude
Desktop; the server is `tools/mcp/server.py`, launched over stdio by the
client, reading the SQLite archive and the dongle's HTTP API. It replaces
grep over transcripts with named calls whose results are already shaped: a
question about the 12 V battery over a week is one `series` call, not a
240,000-token read.

Tools:

| Tool | Answers |
|---|---|
| `sessions` | what sessions there were in a range, with their state changes |
| `events` | the bike's event log in a range, optionally filtered |
| `series` | one reading over time, downsampled to a step |
| `reading_names` | which names exist, from which command, how many samples |
| `snapshot` | a command's full output nearest a time |
| `search` | full-text search over every line |
| `session_lines` | a stretch of one session's transcript |
| `live_status`, `live_readings`, `live_command` | the dongle now |
| `ingest` | load new pulled files |

### Questions it answers today

Tried against the archive as it stands (66 sessions, 6 to 11 September, the
poller's readings from the 10th on), each run through the server over the
protocol and checked against the raw files, and each mapped to an owner ask in
[owner-asks.md](owner-asks.md) or an open question:

1. **Is the 12 V battery healthy?** `series("12V_Battery", step_s=3600)`,
   with `DC-DC` and `Total_Current` beside it. 134 samples so far, resting
   between 12.87 and 13.22 V, and the wake-time sag against the DC-DC rail is
   the health figure the OEM app hides.
2. **Did the hourly wakes do anything last night?** `sessions(since="-24h")`
   reads the states: `STRT>PWSU, PWSU>HIB` with `Timed out in PW Startup` is a
   wake that found no cell module; a top-up shows as a charge state. Nine of
   nine wakes on the 10th timed out, and `series("connected_to_starcom")` is
   zero in all 132 samples, which is the open cellular question, answered
   nightly.
3. **How balanced is the pack?** `series("lowest_cell_voltage_mv")` against
   `series("pack_voltage_mv")`; the pack is 28 cells in series, so the gap
   between the pack average and the lowest cell is the imbalance. It reads
   4 mV at 54 % on the 10th. A derived tool once the arithmetic is settled.
4. **What did the last charge look like?** `sessions()` finds `STOP>CHRG`;
   `series("State_of_Charge")`, `pack_current_ma` and `max_pack_temp_c`
   inside that window are the charge curve. The two charges in the archive
   predate the poller, so today this names them and shows their events; the
   next charge fills in the curve.
5. **Has the bike set any faults?** `events(pattern="Fault set")` and
   `events(pattern="Fault cleared")` for the MBB's own record, `snapshot("faults")`
   for what is active now, `series("Active_DTCs")` for the stored count. The
   archive shows the controller-warning faults of the 7th setting and clearing
   within seconds, one stored DTC, MIL off.

State of health, the most-asked figure, is `series("pack_capacity_ah")`: flat
at 84 Ah so far, a question for months of data rather than days.

`.mcp.json` at the repo root registers it for Claude Code; `tools/mcp/run.sh`
creates the server's Python 3.10+ venv on first run and installs the `mcp`
SDK, so the checkout needs nothing else. The server never sends the dongle
anything but GETs and a poll request.
