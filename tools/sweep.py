#!/usr/bin/env python3
"""Run a list of MBB console commands once each and file their outputs.

Usage: tools/sweep.py [--host H] [--out DIR] [--only NAME,...] [--timeout S]
                      [--settle S] [--list]

Connects to the board's console (port 6638), sends each command, and keeps
what comes back until the MBB's prompt. One file per command under --out,
plus sweep.txt holding the lot in order. The console has priority over the
poller, so an open session stops a batch from starting and no answers
interleave.

The MBB must be awake. A hibernating bike answers nothing, so the script
refuses to start rather than filling the files with silence.

Outputs are for reading, not for committing: some carry serial numbers,
and ccm carries the bike's position. --out defaults under logs/, which is
git-ignored.

Large answers back to back have overflowed the dongle's UART buffer and lost
bytes; a "dongle: UART overflow" line inside an output marks the damage.
"""
import argparse
import json
import os
import socket
import sys
import time
import urllib.request

PORT = 6638
PROMPT = b"ZERO MBB>"

# Everything the MBB's help names under operational information that the
# poller does not already run every minute, most interesting first so a
# short wake window still yields the unknowns. Bare "config" and "ltsm" only
# report. "eldh" answers that log printing is not supported on this
# revision, kept so the refusal is on record.
COMMANDS = [
    "bms interface", "bms commands", "bms elig", "bms module", "bms status",
    "bms info", "faults -v", "state", "notif", "stats", "controller",
    "heater", "ccm", "dash", "msc", "performance", "obd", "compat",
    "notif_pool", "version", "dash version", "time", "config", "help",
    "eldh", "ltsm", "update", "dash info", "dash flags", "dash tt",
    "dash switch", "dash charge", "dash notif",
]
# Left out on purpose: "config N" and "dash clear" write; "elddh" is
# interactive and sits waiting for a date, which hangs the sweep; "set" needs
# a login. "dash version" is not a dash sub-command, it prints the same usage
# list as bare "dash".


def status(host, timeout=8):
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=timeout) as r:
        return json.loads(r.read())


def drain(sock, seconds):
    """Read whatever is already coming, so a command starts from quiet."""
    out = b""
    end = time.time() + seconds
    sock.settimeout(0.3)
    while time.time() < end:
        try:
            b = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not b:
            break
        out += b
    return out


def run_one(sock, cmd, timeout, quiet_after):
    """Send one command; keep bytes until the prompt or the timeout."""
    sock.sendall(cmd.encode() + b"\n")
    out = b""
    deadline = time.time() + timeout
    last = time.time()
    sock.settimeout(0.5)
    while time.time() < deadline:
        try:
            b = sock.recv(4096)
        except socket.timeout:
            # The prompt may have arrived already; a quiet gap after it ends the read.
            if PROMPT in out and time.time() - last > quiet_after:
                break
            continue
        except OSError:
            break
        if not b:
            break
        out += b
        last = time.time()
    return out, (PROMPT in out)


def clean(raw, cmd):
    """Drop the echo and the trailing prompt, keep the body and its line ends."""
    text = raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    while lines and not lines[0].strip():
        lines = lines[1:]
    if lines and lines[0].strip() == cmd:
        lines = lines[1:]
    while lines and lines[-1].startswith("ZERO MBB>"):
        lines = lines[:-1]
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("DONGLE_HOST", "zero-dongle-a12c.local"))
    ap.add_argument("--out", default=None, help="directory for the outputs (default logs/sweep-<host>-<stamp>)")
    ap.add_argument("--only", default=None, help="comma-separated subset of the command list")
    ap.add_argument("--timeout", type=float, default=8.0, help="seconds to wait for one command's prompt")
    ap.add_argument("--settle", type=float, default=0.4, help="quiet gap after the prompt that ends a read")
    ap.add_argument("--list", action="store_true", help="print the command list and exit")
    args = ap.parse_args()

    cmds = COMMANDS
    if args.only:
        cmds = [c.strip() for c in args.only.split(",") if c.strip()]
    if args.list:
        for c in cmds:
            print(c)
        return 0

    try:
        st = status(args.host)
    except Exception as e:
        print("sweep: no status from %s: %s" % (args.host, e))
        return 2
    print("sweep: %s fw %s boot %s, MBB %s" %
          (st.get("name"), st.get("fw"), st.get("boot"), "awake" if st.get("mbb_awake") else "asleep"))
    if not st.get("mbb_awake"):
        print("sweep: the MBB is asleep; key on or wait for a wake, then run again")
        return 3

    out = args.out or os.path.join("logs", "sweep-%s-%s" % (st.get("name") or args.host,
                                                           time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)

    try:
        sock = socket.create_connection((args.host, PORT), timeout=8)
    except OSError as e:
        print("sweep: no console at %s:%d: %s" % (args.host, PORT, e))
        return 2
    greeting = drain(sock, 1.5).decode("utf-8", "replace").strip().splitlines()
    print("sweep: %s" % (greeting[0] if greeting else "no greeting"))

    combined = []
    results = []
    silent = 0
    for cmd in cmds:
        raw, got_prompt = run_one(sock, cmd, args.timeout, args.settle)
        body = clean(raw, cmd)
        name = cmd.replace(" ", "_").replace("-", "") + ".txt"
        with open(os.path.join(out, name), "w") as f:
            f.write(body + "\n")
        combined.append("===== %s =====\n%s\n" % (cmd, body))
        results.append((cmd, len(body), got_prompt))
        print("  %-16s %6d bytes%s" % (cmd, len(body), "" if got_prompt else "   NO PROMPT"))
        sys.stdout.flush()
        # A wake window is short. Two silent commands in a row means the MBB
        # has gone, and waiting out the timeout on each of the rest wastes it.
        silent = silent + 1 if not got_prompt else 0
        if silent >= 2:
            print("sweep: no prompt twice; the MBB has gone, stopping here")
            break
    sock.close()

    with open(os.path.join(out, "sweep.txt"), "w") as f:
        f.write("\n".join(combined))

    empty = [c for c, n, _ in results if n == 0]
    noprompt = [c for c, _, p in results if not p]
    print("sweep: %d commands into %s" % (len(results), out))
    if empty:
        print("sweep: empty: %s" % ", ".join(empty))
    if noprompt:
        print("sweep: no prompt: %s" % ", ".join(noprompt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
