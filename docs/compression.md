# Compression

How the session files are compressed, what the alternatives were measured
at, and what it costs the flash. The design itself is item 9 of
[firmware.md](firmware.md); this is the evidence behind it. All figures are
from the bike unit's own session files, replayed in order through the
store's real commit boundaries (3 s of MBB quiet, 15 s, or a full 4 KB
output buffer), with fixed Huffman codes throughout, which is what the
dongle's compressor emits.

## The three designs

- **Naive: one deflate stream per file, a 4 KB window.** Each file is
  compressed on its own against its own history. A ride compresses well,
  because its heartbeat lines repeat within the file. An hourly wake does
  not: its text, the banner, the self-test and the hibernate sequence, is
  the same every hour but appears only once per file, so the compressor
  sees it fresh each time, and the two stamps on every line are unique
  bytes on top. This is what firmware 0.6.x did.
- **A dictionary trained once and baked into the firmware.** A block of
  the lines the MBB prints, compiled in, so the compressor's history is
  never empty. It works, and on a wake file measures 9.6x, but every bike
  prints a different banner, firmware revision and board name, so a baked
  dictionary fits one bike and its firmware version and nothing else, and
  every change to it is a firmware release.
- **A dictionary the dongle learns from its own sessions.** The same
  mechanism, but the dictionary is built on the board from what it has
  already stored, kept on the flash as a file named by its checksum, and
  named in every session file's header so the puller can fetch the right
  one. Each bike learns its own. This is firmware 0.7.0.

## The process

Every MBB line the store takes is stripped of its two stamps and looked up
in the dictionary. A line already there is marked as used; a line not
there is kept as a candidate, in a 4 KB buffer, first come. When a session
ends with the MBB asleep, and the candidates hold at least 1 KB of new
text, and the last rebuild is over an hour old, the dictionary is rebuilt:
the lines proven since the last rebuild first, up to two thirds of the
size, then the new lines, then the remaining proven lines, then the rest,
cut at 6 KB. A rebuild that fits no new line writes nothing. The proven
marks start over every 48 sessions, so a line that stops recurring loses
its standing and falls off. The result is written to a temporary name,
read back against its checksum, renamed to `dict-<adler32>.txt`, its id
saved, and used from the next session on. A dictionary file is deleted only when
no session file on the flash names it and it is not the one in use; the
space reclaim never touches them.

The stream's window is the dictionary followed by 8 KB of the file's own
history, so a line can match either. A match into the dictionary is
emitted at its true distance in the decoder's stream, which grows as the
history slides, and past 32 KB of output the dictionary is out of the
decoder's reach and the file's own history carries on alone.

## Before and after

The bike unit's first 26 sessions, in order. "Plain" is the naive design;
"learned" is the dictionary as it stood when each file was written, so
the first files have none and the wakes at the end have a mature one.

| Session | Kind | Raw bytes | Plain | Learned |
|---|---|---|---|---|
| b0005-01 | mixed | 355 | 212 (1.7x) | 212 (1.7x) |
| b0008-01 | mixed | 510 | 149 (3.4x) | 149 (3.4x) |
| b0011-01 | mixed | 246 | 200 (1.2x) | 200 (1.2x) |
| b0011-02 | ride | 205078 | 24029 (8.5x) | 23462 (8.7x) |
| b0096-01 | key-on | 210 | 146 (1.4x) | 85 (2.5x) |
| b0097-01 | key-on | 210 | 146 (1.4x) | 82 (2.6x) |
| b0098-01 | charging | 1969 | 713 (2.8x) | 599 (3.3x) |
| b0099-01 | charging | 15941 | 3772 (4.2x) | 3137 (5.1x) |
| b0100-001 | key-on | 811 | 383 (2.1x) | 266 (3.0x) |
| b0101-001 | key-on | 811 | 388 (2.1x) | 270 (3.0x) |
| b0102-001 | key-on | 1389 | 461 (3.0x) | 344 (4.0x) |
| b0103-001 | charging | 8067 | 2085 (3.9x) | 1215 (6.6x) |
| b0104-001 | charging | 11692 | 3211 (3.6x) | 2429 (4.8x) |
| b0105-001 | key-off | 1941 | 732 (2.7x) | 395 (4.9x) |
| b0105-002 | wake | 6231 | 1848 (3.4x) | 1387 (4.5x) |
| b0106-001 | wake | 6913 | 2052 (3.4x) | 970 (7.1x) |
| b0106-002 | wake | 6798 | 2041 (3.3x) | 977 (7.0x) |
| b0107-001 | wake | 6218 | 1831 (3.4x) | 773 (8.0x) |
| b0107-002 | wake | 7030 | 2054 (3.4x) | 989 (7.1x) |
| b0107-003 | wake | 7111 | 2073 (3.4x) | 1019 (7.0x) |
| b0107-004 | wake | 6812 | 2009 (3.4x) | 946 (7.2x) |
| b0107-005 | wake | 6870 | 2004 (3.4x) | 945 (7.3x) |
| b0107-006 | wake | 7111 | 2066 (3.4x) | 1011 (7.0x) |
| b0107-007 | wake | 7110 | 2073 (3.4x) | 1020 (7.0x) |
| b0107-008 | wake | 7111 | 2070 (3.4x) | 1019 (7.0x) |
| b0107-009 | wake | 7030 | 2033 (3.5x) | 975 (7.2x) |
| all 26 | | 331575 | 60781 (5.5x) | 44876 (7.4x) |

By kind:

| Kind | Files | Raw bytes | Plain | Learned |
|---|---|---|---|---|
| Rides and key-ons | 10 | 211561 | 7.9x | 8.3x |
| Charging | 4 | 37669 | 3.9x | 5.1x |
| Hourly wakes | 12 | 82345 | 3.4x | 6.8x |

The wakes are what a parked bike writes, so they are the figure that sets
how long the flash lasts between pulls: a wake costs one 4 KB block
either way, since the filesystem counts in blocks, but a parked day drops
from about 60 KB to about 30 KB of files, and the area holds about a
month. Rides were already fine.

## What the flash sees

A rebuild writes a new 6 KB dictionary file, two 4 KB blocks, and the old
one is deleted once nothing on the flash names it. The rule that a rebuild
needs over 1 KB of new lines and an hour since the last one is what keeps
small adaptive changes from turning into files: in the replay above there
were six rebuilds, all in the first day while the dictionary was
learning, 37 KB written for dictionaries against 45 KB of session files,
and none across the last nine wakes once the dictionary had settled. The
steady state is a rebuild after something new, a ride with a fault, a
firmware revision, and nothing between.

Against the flash's endurance this is noise either way. LittleFS levels
wear across the log area's 224 blocks, each good for about 100,000
erases; at a hundred kilobytes of writes a day, sessions and dictionaries
together, the area lasts thousands of years. The puller sees a rebuild as
one 6 KB fetch, cached by id.

## What remains

With the text matched, what is left in a compressed wake is the stamps:
the dongle's and the MBB's, two per line, about 45 characters of mostly
unique digits on 80 lines. They are about a quarter of the compressed
bytes. A delta-encoded stamp in a structured record would take that out;
it is the next step if the flash ever needs one, and it is a format
change, so it waits for a reason.
