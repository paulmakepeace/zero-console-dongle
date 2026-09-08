#!/usr/bin/env python3
"""Bench regression for the dongle with the CP2102 adapter standing in for the
MBB: adapter TXD to the dongle's pin 8 input, adapter RXD to its pin 9 output.

Usage: tools/bench.py [roundtrip|break|sleep|all] [--host H] [--adapter DEV]

  roundtrip  a line from the adapter reaches a TCP console client, and a
             console keystroke reaches the adapter with the CR the MBB wants
  break      the transmit pin lets go within 200 ms of the line going low,
             while the awake flag is still true, and takes input again after
  sleep      a 6 s low takes the MBB to asleep, closes the session file, and
             the next high wakes it and opens a new one

Defaults: host zero-dongle-ebdc.local (DONGLE_HOST), adapter the first
/dev/cu.usbserial-* that is not the DevKit's own bridge (DONGLE_ADAPTER).
Exit 1 on any failed check. Never run against the bike: the break holds the
adapter's line low, which on the bike is the MBB's own output.
"""
import argparse
import fcntl
import glob
import json
import os
import socket
import sys
import time
import urllib.request

import serial

TIOCSBRK = 0x2000747B if sys.platform == "darwin" else 0x5427
TIOCCBRK = 0x2000747A if sys.platform == "darwin" else 0x5428
fails = []


def check(cond, what):
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        fails.append(what)


def status(host):
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=5) as r:
        return json.loads(r.read())


def console(host):
    c = socket.create_connection((host, 6638), timeout=5)
    time.sleep(0.4)
    c.recv(500)   # the banner
    return c


def adapter_open(dev):
    ad = serial.Serial(dev, 115200, timeout=0.2)
    time.sleep(0.5)          # the driver hands over stale bytes right after open
    ad.reset_input_buffer()
    return ad


def t_roundtrip(host, ad):
    print("roundtrip")
    c = console(host)
    ad.write(b"DEBUG: 09/07/2026 19:55:00.000 bench line\r\n")
    time.sleep(0.5)
    got = c.recv(500)
    check(b"bench line" in got, "adapter line reached the console: %r" % got)
    c.sendall(b"help\n")
    time.sleep(0.5)
    back = ad.read(100)
    check(back == b"help\r\n", "console input reached the adapter as CR LF: %r" % back)
    s = status(host)
    check(s["tx_attached"], "transmit pin attached after the write")
    time.sleep(2.5)
    s = status(host)
    check(not s["tx_attached"], "transmit pin released after the hold")
    check(ad.read(10) == b"\x00", "the release reached the adapter as one NUL")
    c.close()


def t_break(host, ad):
    print("break")
    c = console(host)
    c.sendall(b"help\n")
    time.sleep(0.2)
    ad.read(100)
    s = status(host)
    check(s["tx_attached"] and s["line_high"], "attached with the line high")
    fcntl.ioctl(ad.fd, TIOCSBRK)
    t0 = time.time()
    released = None
    while time.time() - t0 < 1.2:
        s = status(host)
        if not s["tx_attached"]:
            released = time.time() - t0
            break
    # Three low samples 20 ms apart plus the poll itself; the hold it cuts short is 2 s.
    check(released is not None and released < 0.5, "released %s after the line dropped" %
          ("%.0f ms" % (released * 1000) if released else "never"))
    s = status(host)
    check(not s["line_high"] and s["mbb_awake"], "line low, awake flag still true")
    c.sendall(b"z\n")
    time.sleep(0.3)
    note = c.recv(200)
    check(b"not sent" in note, "input during the low was refused: %r" % note)
    fcntl.ioctl(ad.fd, TIOCCBRK)
    time.sleep(0.3)
    ad.read(100)
    c.sendall(b"y\n")
    time.sleep(0.4)
    check(ad.read(100) == b"y\r\n", "input after the line returned went through")
    c.close()


def t_sleep(host, ad):
    print("sleep")
    ad.write(b"DEBUG: 09/07/2026 19:55:01.000 before sleep\r\n")
    time.sleep(0.5)
    before = status(host)
    check(before["mbb_awake"], "awake before")
    fcntl.ioctl(ad.fd, TIOCSBRK)
    time.sleep(6.5)
    s = status(host)   # while the line is still low; a high wakes it again within 60 ms
    check(not s["mbb_awake"], "asleep after 6 s low")
    fcntl.ioctl(ad.fd, TIOCCBRK)
    time.sleep(4)   # the close commits after the quiet period
    s = status(host)
    check(s["active"] == "", "session file closed: active is %r" % s["active"])
    ad.write(b"DEBUG: 09/07/2026 19:55:20.000 after wake\r\n")
    time.sleep(1.0)
    s = status(host)
    check(s["mbb_awake"], "awake again on the first line")
    check(s["awake_count"] == before["awake_count"] + 1, "awake count advanced")
    time.sleep(4)
    s = status(host)
    check(s["active"] not in ("", before["active"]), "a new session file opened: %r" % s["active"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("test", nargs="?", default="all", choices=["roundtrip", "break", "sleep", "all"])
    ap.add_argument("--host", default=os.environ.get("DONGLE_HOST", "zero-dongle-ebdc.local"))
    ap.add_argument("--adapter", default=os.environ.get("DONGLE_ADAPTER"))
    args = ap.parse_args()
    dev = args.adapter
    if not dev:
        found = [d for d in sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*")) if not d.endswith("-0001")]
        dev = found[0] if found else None
    if not dev:
        sys.exit("bench: no adapter found; pass --adapter")
    if "a12c" in args.host:
        sys.exit("bench: refusing to run against the bike unit")
    print("bench: %s through %s" % (args.host, dev))
    ad = adapter_open(dev)
    tests = {"roundtrip": t_roundtrip, "break": t_break, "sleep": t_sleep}
    for name in (tests if args.test == "all" else [args.test]):
        try:
            tests[name](args.host, ad)
        except Exception as exc:
            check(False, "%s raised %s: %s" % (name, type(exc).__name__, exc))
        ad.reset_input_buffer()
    print("bench: %d check(s) failed" % len(fails) if fails else "bench: all checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
