# Fake MBB answers

What the bench's fake MBB replies to each polled command, one file per
command, as the bike's own MBB printed it during an hourly wake, with the
battery serial replaced. `tools/bench.py` reads these; the responder
echoes the command, sends the file, and ends with the prompt, as the MBB
does. The `pdu` and `in` files are from a key-on, `bms interface`,
`controller`, `msc`, `dash info` and `ccm` from a key-on sweep with the
GPS fix and the cellular unit id masked, the rest from an hourly wake.
