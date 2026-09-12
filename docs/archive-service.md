# The archive service: the dongle pushes, the NAS keeps, MCP answers

Built 2026-09-11 as firmware 0.12.0: the push in `firmware/src/push.cpp`,
the service in `server/`, validated on the bench and flashed to the bike with
the push off until the service runs on the NAS. It follows from [data-model.md](data-model.md), which
settles what the data is and that the homelab is the archive; this note
settles how the data gets there and how it is asked for. Decided 2026-09-11
from a demo of the five questions in that doc against the archive.

## Requirements

Primary:

- **Live queries on the bike.** Presented however; the no-work option is a
  `<pre>` of console output, which the dongle's pages already are.
- **Time-series data queried over HTTP and MCP**, from any client, including a
  fresh chat window on a phone.

Secondary, the exhaust of the above:

- **A log of every console pull**, so a session is never only on the flash.

## The pieces

One container on the NAS, two volumes. The service holds three routes in one
Python process: an upload endpoint, the MCP endpoint, and a health line. The
volumes are the dated archive of raw session files, `YYYYMM/DD/*.log.z`, and
the SQLite database.

- **The raw files are the truth.** A session lands in the archive first, as
  the dongle wrote it, compressed. Ingest reads it from there, so a schema or
  parser change is `ingest --force` over the archive and nothing is lost.
- **SQLite stays.** There is no sidecar: the database is a file on the volume,
  the ingest is its one writer, WAL mode lets the MCP reads proceed during
  a write. At the measured rate a year is 1 to 2 GB and a few million reading
  rows, one indexed range scan per series. Postgres enters with a second
  writer or a Grafana panel, TimescaleDB after that. Crate, Elasticsearch and
  RethinkDB answer problems this data does not have.
- **JSONB does not buy anything yet.** The readings stream is column-shaped
  and the archive's readings table is already its long form, which is the
  time-series shape in any store. The document-shaped parts are the snapshots,
  stored as text and only ever fetched whole. JSONB earns a place when a
  per-command parser exists, which it does not: see below.
- **The MCP server moves to HTTP.** The same [server.py](../tools/mcp/server.py)
  on the streamable HTTP transport instead of stdio, registered by URL at user
  scope in Claude Code and as a connector in Claude Desktop, so its tools and
  prompts appear in every chat. A stdio server lives for one chat on one
  machine; the NAS is where always-on belongs. The live tools can send the
  dongle commands, so the endpoint carries authentication before it leaves
  the LAN.
- **Recipes live in the server.** A `/bike-soc` that fetches the day's state
  of charge is an MCP prompt, defined beside the tools, because it goes
  wherever the server is registered. A project command in `.claude/commands`
  is for work on this repo, not for asking about the bike. Resources for
  @-mention: the latest snapshot of a command, a session transcript, the
  day's events, the reading catalogue. A bench session is 12,000 lines, so
  transcripts are the one to attach with care.

## Push on join

The dongle uploads instead of being pulled. The hook exists: on every join
[wlan.cpp](../firmware/src/wlan.cpp) counts the got-IP event on the driver's
task and the loop task calls `startServices`. The push is a flag set there and
an upload done by the loop task once the services are up. The driver stops for
every sleep and rejoins on resume, so an upload after each join is an upload
after each MBB wake, with no clock or cron on either side. The puller stays as
the manual fallback.

Two constraints, both bought by earlier defects:

- **The push is activity.** A timed-out wake is about ninety seconds. The
  upload counts against the sleep decision the way the setup network does in
  `wifiBusy`, or the board sleeps mid-file.
- **The store lock.** Reading a session file for upload while the capture
  stage commits needs the same care as the 2026-09-09 capture-tick revert.

Each file is PUT as the flash holds it and deleted on the server's 200, oldest
first, which is what makes it retry-safe. A session's dictionary, named in its
file name and kept on the flash while any session names it, goes first once
per round; a session the server cannot inflate is kept raw and refused, and
the dongle moves on. The ingest runs on arrival. One archive and database
serve one board: session names carry no board, so the bench pushes to a
test server on the laptop, never to the bike's. GPS is not the trigger, since WiFi
is a prerequisite anyway, but `ccm` already prints it as radians times ten to
the eighth and the archive already parses those five rows, so a ride becomes
a track with a derived series and no firmware change.

## Where parsing stands

A row grammar, not a per-command parser. [rows.h](../firmware/src/pure/rows.h)
recognises a comma-table row and a dash-list row, without knowing which
command or section the row came from, and never reads a unit. The firmware
applies it to a whitelist of 50 names; the archive applies it to every
output line and has 318 names. Output lines accepted, per command:

| command | output lines | rows parsed | names |
|---|---|---|---|
| status | 8866 | 2642 | 142 |
| ccm | 6996 | 4752 | 36 |
| charging | 6951 | 3466 | 25 |
| bms | 5586 | 665 | 5 |
| faults | 945 | 0 | 0 |

About three fifths overall. The shortfalls are structural: the bms cell table
has several numeric columns per row and the grammar keeps the first; status
is several sections told apart only by row name; faults is prose. The
semantic layer, one parser per command producing a structured object, is the
piece that does not exist, and its home is the NAS ingest, where the raw
text is archived and any parser can be rerun.

## Found in the demo

- **The events tool leaks torque lines.** The MBB's `Torque:` debug logger
  carries an MBB stamp, so its lines are events, and the nosync file holds
  1749 of them. Exclude that logger from the event kind, or drop the file.
- **A ride is missing** from the 10th: state of charge fell from 77 % to
  54 % with no session between 11:35 and 14:51, because the dongle was on
  USB power and unplugged before the ride. The phase 2 supply removes the
  case.
- **A worktree has no archive.** The database is git-ignored, so a server
  started from a worktree opens an empty one and answers zero samples. Set
  `DONGLE_DB` to the main checkout's file, or run the server from there.
