#!/usr/bin/env python3
"""Bench regression for the dongle with the CP2102 adapter standing in for the
MBB: adapter TXD to the dongle's pin 8 input, adapter RXD to its pin 9 output.

Usage: tools/bench.py [roundtrip|break|sleep|poll|lightsleep|all] [--host H] [--adapter DEV]

  roundtrip  a line from the adapter reaches a TCP console client, and a
             console keystroke reaches the adapter with the CR the MBB wants
  break      the transmit pin lets go within 200 ms of the line going low,
             while the awake flag is still true, and takes input again after
  sleep      a 6 s low takes the MBB to asleep, closes the session file, and
             the next high wakes it and opens a new one
  poll       with the adapter answering as the MBB, a requested poll fills
             every command's output, keeps the transmit pin held across the
             batch, passes an unsolicited line through to the log, and keeps
             the outputs themselves out of the log
  lightsleep about six minutes: a fake "Hibernating for 300 sec" then a held
             low makes the dongle sleep after its grace period and wake on
             its timer before the MBB is due; a second cycle wakes it on
             pin 8 instead

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
import threading

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


# Canned answers for the poll test; the responder echoes the command first
# and ends with the prompt, as the MBB does.
ANSWERS = {
    b"status": b" Bike State: CHRG\r\n BMS | SOC |  Pack V\r\n   2   86 %  108555 mV\r\n",
    b"charging": b"         EVSE_Command,          1,       Yes\r\n",
    b"bms": b"BMS 2 status data:\r\n     - pack voltage      108558\r\n     - soc               86\r\n",
    b"pdu": b" PDU channels\r\nDEBUG:   09/07/2026 21:58:20.935  x.c : line 650 - Control flags changed\r\n ch 1 12000 mV\r\n",
    b"in": b" Key_On, 1\r\n",
    b"faults": b" no faults\r\n",
}


class FakeMbb(threading.Thread):
    """Answers commands on the adapter like the MBB until stopped."""

    def __init__(self, ad):
        super().__init__(daemon=True)
        self.ad = ad
        self.stop = threading.Event()
        self.seen = []

    def run(self):
        line = b""
        while not self.stop.is_set():
            d = self.ad.read(64)
            if not d:
                continue
            line += d.replace(b"\x00", b"")
            while b"\n" in line:
                cmd, line = line.split(b"\n", 1)
                cmd = cmd.strip(b"\r")
                self.seen.append(cmd)
                self.ad.write(cmd + b"\r\n" + ANSWERS.get(cmd, b"unknown command\r\n") + b"ZERO MBB> ")


def post(host, path):
    req = urllib.request.Request("http://%s%s" % (host, path), method="POST")
    req.add_header("X-Dongle", "1")
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.read().decode()


def get(host, path):
    with urllib.request.urlopen("http://%s%s" % (host, path), timeout=5) as r:
        return r.status, r.read().decode()


def t_poll(host, ad):
    print("poll")
    mbb = FakeMbb(ad)
    mbb.start()
    try:
        post(host, "/api/settings")   # nothing to change; proves the header path
        print("  ", post(host, "/api/cmd/poll"))
        held = False
        t0 = time.time()
        while time.time() - t0 < 60:
            s = status(host)
            if s["poll"]["active"] and s["tx_attached"]:
                held = True
            if not s["poll"]["active"] and len(mbb.seen) >= 6:
                break
            time.sleep(0.3)
        check(held, "transmit pin held while the batch ran")
        check(len(mbb.seen) >= 6, "all six commands reached the adapter: %r" % mbb.seen[:6])
        lst = json.loads(get(host, "/api/cmd")[1])
        check(all(c["ok"] for c in lst), "every command closed on its prompt: %r" % [(c["name"], c["ok"]) for c in lst])
        code, bms = get(host, "/api/cmd/bms")
        check(code == 200 and "soc" in bms and "bms" not in bms.splitlines()[0], "bms output kept without its echo: %r" % bms[:60])
        s = status(host)
        check(s["pack"]["soc"] == 86 and s["pack"]["bike_state"] == "CHRG", "status carries soc %s and state %r" % (s["pack"]["soc"], s["pack"]["bike_state"]))
        time.sleep(3.5)
        check(not status(host)["tx_attached"], "transmit pin released after the batch")
        live = get(host, "/live")[1]
        check("Control flags changed" in live, "the unsolicited line inside a response reached the log")
        check("pack voltage" not in live and "Key_On" not in live, "the responses themselves stayed out of the log")
        try:
            get(host, "/api/cmd/nope")
            check(False, "unknown command is 404")
        except urllib.error.HTTPError as e:
            check(e.code == 404, "unknown command is 404")
    finally:
        mbb.stop.set()
        mbb.join(1)


def t_lightsleep(host, ad):
    print("lightsleep (about six minutes)")
    ad.write(b"Saving Stats, Hibernating for 300 sec\r\n")
    time.sleep(0.5)
    before = status(host)
    fcntl.ioctl(ad.fd, TIOCSBRK)
    t0 = time.time()
    try:
        time.sleep(7)
        check(not status(host)["mbb_awake"], "asleep behind the held low")
        gone = None
        while time.time() - t0 < 180:
            try:
                status(host)
                time.sleep(5)
            except Exception:
                gone = time.time() - t0
                break
        check(gone is not None and 110 < gone < 175, "went to sleep %s after the low (grace 120 s)" % ("%.0f s" % gone if gone else "never"))
        back = None
        while time.time() - t0 < 360:
            try:
                s = status(host)
                back = time.time() - t0
                break
            except Exception:
                time.sleep(5)
        check(back is not None and back < 320, "back on the network %s after the low (MBB due at 300 s)" % ("%.0f s" % back if back else "never"))
        if back:
            check(s["sleep"]["count"] == before["sleep"]["count"] + 1 and s["sleep"]["last_wake"] == "timer",
                  "one sleep, woken by the timer: %r" % s["sleep"])
            check(s["boot"] == before["boot"], "no reboot across the sleep")
    finally:
        fcntl.ioctl(ad.fd, TIOCCBRK)
    time.sleep(1.5)
    s = status(host)
    check(s["mbb_awake"], "awake again with the line high")
    # Second cycle: the same sleep, ended early by pin 8 rising.
    ad.write(b"Saving Stats, Hibernating for 300 sec\r\n")
    time.sleep(0.5)
    before = status(host)
    fcntl.ioctl(ad.fd, TIOCSBRK)
    t0 = time.time()
    try:
        gone = None
        while time.time() - t0 < 180:
            try:
                status(host)
                time.sleep(5)
            except Exception:
                gone = time.time() - t0
                break
        check(gone is not None, "second sleep started")
        time.sleep(20)
    finally:
        fcntl.ioctl(ad.fd, TIOCCBRK)   # pin 8 high: the wake source that is not the timer
    t1 = time.time()
    back = None
    while time.time() - t1 < 60:
        try:
            s = status(host)
            back = time.time() - t1
            break
        except Exception:
            time.sleep(3)
    check(back is not None, "back on the network %s after pin 8 rose" % ("%.0f s" % back if back else "never"))
    if back:
        check(s["sleep"]["last_wake"] == "pin 8" and s["mbb_awake"], "woken by pin 8 and awake: %r" % s["sleep"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("test", nargs="?", default="all", choices=["roundtrip", "break", "sleep", "poll", "lightsleep", "all"])
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
    tests = {"roundtrip": t_roundtrip, "break": t_break, "sleep": t_sleep, "poll": t_poll, "lightsleep": t_lightsleep}
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
