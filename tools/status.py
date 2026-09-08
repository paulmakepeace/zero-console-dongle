#!/usr/bin/env python3
"""One line of status per board, or a watch that reports changes.

Usage: tools/status.py [HOST ...] [--watch SECONDS] [--json]

HOST is a board name or address; default zero-dongle-a12c.local, or
DONGLE_HOST; "all" means every board in DONGLE_BOARDS (default: the bike
unit and the bench board). --watch polls and prints a line only when
something changed: boot count (a reboot), awake state, active file, WiFi
loss, or a UART loss counter, plus a heartbeat every ten polls. --json
prints the raw status.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

DEFAULT_BOARDS = "zero-dongle-a12c.local zero-dongle-ebdc.local"


def fetch(host, timeout=8):   # the board serves one client at a time, and a silent one can hold it five seconds
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=timeout) as r:
        return json.loads(r.read())


def line(s):
    uart = s.get("uart", {})
    wifi = s.get("wifi", {})
    up = int(s.get("uptime_s", 0))
    return ("%-16s fw %-6s boot %-4s %-9s up %2dh%02dm  %s line %s tx %s  ovf %s fe %s qd %s drop %s  "
            "heap_min %s  rssi %s  %s" % (
                s.get("name"), s.get("fw"), s.get("boot"), s.get("reset_reason"), up // 3600, up % 3600 // 60,
                "awake " if s.get("mbb_awake") else "asleep", "hi" if s.get("line_high") else "lo",
                "on" if s.get("tx_attached") else "off", uart.get("overflows"), uart.get("frame_errors"),
                uart.get("queue_drops"), s.get("dropped_lines"), s.get("heap_min_free"), wifi.get("rssi"),
                s.get("active") or "(no file)"))


def key(s):
    uart = s.get("uart", {})
    return (s.get("boot"), s.get("mbb_awake"), s.get("active"), uart.get("overflows"), uart.get("frame_errors"),
            uart.get("queue_drops"), s.get("dropped_lines"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("hosts", nargs="*")
    ap.add_argument("--watch", type=float, metavar="SECONDS")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    hosts = args.hosts or [os.environ.get("DONGLE_HOST", "zero-dongle-a12c.local")]
    if hosts == ["all"]:
        hosts = os.environ.get("DONGLE_BOARDS", DEFAULT_BOARDS).split()
    last = {}
    polls = 0
    while True:
        stamp = time.strftime("%H:%M:%S")
        for h in hosts:
            try:
                s = fetch(h)
            except Exception as exc:
                s = None
                text = "%-16s unreachable: %s" % (h, exc)
            if s is not None:
                text = json.dumps(s) if args.json else line(s)
            k = key(s) if s else ("down",)
            if not args.watch:
                print(text)
            elif k != last.get(h) or polls % 10 == 0:
                note = ""
                if h in last and s and last[h][0] != k[0]:
                    note = "  REBOOT"
                elif h in last and last[h] == ("down",) and s:
                    note = "  BACK"
                print(stamp, text + note, flush=True)
            last[h] = k
        if not args.watch:
            return
        polls += 1
        time.sleep(args.watch)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
