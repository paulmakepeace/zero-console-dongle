#!/usr/bin/env python3
"""Bench regression for the dongle with the CP2102 adapter standing in for the
MBB: adapter TXD to the dongle's pin 8 input, adapter RXD to its pin 9 output.

Usage: tools/bench.py [SCENARIO ...|quick|auto|all] [--host H] [--adapter DEV]

Scenarios run in the order given. quick is every scenario but lightsleep,
about a minute and a half; auto picks the scenarios for the source files
changed since the last tag, committed or not, from the map at the end of
this file; all is everything, about five minutes.

  roundtrip  a line from the adapter reaches a TCP console client, and a
             console keystroke reaches the adapter with the CR the MBB wants
  break      the transmit pin lets go within a second of the line going low
             as the status poll sees it (the pin itself in about 100 ms),
             while the awake flag is still true, and takes input again after
  sleep      a 6 s low takes the MBB to asleep, closes the session file, and
             the next high wakes it and opens a new one
  poll       with the adapter answering as the MBB, a requested poll fills
             every command's output, keeps the transmit pin held across the
             batch, passes an unsolicited line through to the log, and logs
             the outputs behind a dongle: poll line
  storage    the MBB's own statement that the bike is parked arms the sleep
             at once whatever the days count, a key-on forgets it until the
             MBB restates it, and DIS or Inactive hands the decision back to
             the days rule
  lightsleep about three minutes: with the grace and the use window set
             short for the run, a
             fake "Hibernating for 150 sec" then a held low makes the
             dongle sleep and be back before the MBB is due; a second
             cycle wakes it on pin 8 instead; a console client and the
             file listing are served after the resume; the heap is
             compared across both cycles

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
    # The board's web server serves one client at a time and gives a silent
    # connection five seconds before moving on, so a request can wait that long.
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=8) as r:
        return json.loads(r.read())


def up(ip):
    """Whether the web server answers a connect, within two seconds: places the
    moments the board leaves and returns without the status call's timeout.
    After a sleep the host has to resolve the board's address again, which
    has taken it up to fifteen seconds on this LAN, so the moment of return
    is the host's view, not the board's."""
    try:
        socket.create_connection((ip, 80), timeout=2).close()
        return True
    except OSError:
        return False


def clock_step_after_sleep(live):
    """The NTP step the board logged after its last timer wake, in seconds,
    or None if it has not landed yet."""
    import re
    m = list(re.finditer(r"dongle: slept \d+ s, woke on timer", live))
    if not m:
        return None
    after = live[m[-1].end():]
    n = re.search(r"stepped ([+-]?\d+\.\d) s", after)
    return float(n.group(1)) if n else None


def status_retry(host, tries=3):
    """For moments the board may be busy: a timeout is retried, and reported."""
    for attempt in range(tries):
        try:
            return status(host)
        except Exception as exc:
            print("  note status request %d failed: %s" % (attempt + 1, exc))
            if attempt + 1 == tries:
                raise
            time.sleep(2)


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
    # With nothing answering the adapter, the schedule's batches spend 48 s
    # of every 60 timing out, holding the transmit pin and taking whatever
    # arrives as the current command's answer. Wait for one to end; the open
    # console then keeps the next from starting.
    t0 = time.time()
    while status(host)["poll"]["active"] and time.time() - t0 < 60:
        time.sleep(1)
    time.sleep(2.5)           # the batch's own release NUL follows its end by the hold
    ad.reset_input_buffer()   # that, and a command the batch sent into silence
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
    # A poll command typed on the console is kept as the batch would keep it,
    # from the answer going by; the bench answers as the MBB.
    c.sendall(b"bms\n")
    time.sleep(0.5)
    ad.read(100)   # the command, as the MBB would see it
    ad.write(b"bms\r\n" + ANSWERS[b"bms"] + b"ZERO MBB> ")
    # The prompt has no line end: the dongle writes it after two seconds of
    # silence, and only then is the answer complete.
    t0 = time.time()
    while time.time() - t0 < 8:
        row = {r["name"]: r for r in json.loads(get(host, "/api/cmd")[1])}["bms"]
        if row["ok"] and 0 <= row["age_s"] <= 8:
            break
        time.sleep(0.5)
    code, body = get(host, "/api/cmd/bms")
    check(code == 200 and "soc" in body and row["ok"] and 0 <= row["age_s"] <= 8,
          "a bms typed on the console was kept %.1f s after the answer, age %s s: %r" % (time.time() - t0, row["age_s"], body[:40]))
    # Two commands typed inside the prompt's flush window: the second's echo
    # shares the first's prompt line, and both must be kept.
    time.sleep(3)   # past the first watch's prompt and the store's quiet commit
    ad.reset_input_buffer()
    c.sendall(b"pdu\nin\n")
    time.sleep(0.6)
    ad.read(100)
    ad.write(b"pdu\r\n" + ANSWERS[b"pdu"] + b"ZERO MBB> in\r\n" + ANSWERS[b"in"] + b"ZERO MBB> ")
    t0 = time.time()
    while time.time() - t0 < 8:
        rows = {r["name"]: r for r in json.loads(get(host, "/api/cmd")[1])}
        if all(rows[n]["ok"] and 0 <= rows[n]["age_s"] <= 8 for n in ("pdu", "in")):
            break
        time.sleep(0.5)
    check(all(rows[n]["ok"] and 0 <= rows[n]["age_s"] <= 8 for n in ("pdu", "in")),
          "two commands typed back to back were both kept: pdu age %s, in age %s" % (rows["pdu"]["age_s"], rows["in"]["age_s"]))
    body = get(host, "/api/cmd/in")[1]
    check("12V_Battery" in body and "Total_Current" not in body, "the second's slot holds its own answer, not the first's: %r" % body[:40])
    time.sleep(2.5)
    ad.read(10)   # the hold's release NUL, so the next scenario starts clean
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
    try:
        t0 = time.time()
        released = None
        while time.time() - t0 < 1.2:
            s = status(host)
            if not s["tx_attached"]:
                released = time.time() - t0
                break
        # Three low samples 20 ms apart plus the HTTP polls that see it; the hold it cuts short is 2 s.
        check(released is not None and released < 1.0, "released %s after the line dropped" %
              ("%.0f ms" % (released * 1000) if released else "never"))
        s = status(host)
        check(not s["line_high"] and s["mbb_awake"], "line low, awake flag still true")
        c.sendall(b"z\n")
        time.sleep(0.3)
        note = c.recv(200)
        check(b"not sent" in note, "input during the low was refused: %r" % note)
    finally:
        fcntl.ioctl(ad.fd, TIOCCBRK)   # the line comes back whatever happened above
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
    try:
        time.sleep(6.5)
        s = status_retry(host)   # while the line is still low; a high wakes it again within 60 ms
        check(not s["mbb_awake"], "asleep after 6 s low")
    finally:
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


# The fake MBB's answers: the bike's own, one file per command, in tools/mbb-answers.
ANSWERS = {}
for _f in glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mbb-answers", "*.txt")):
    with open(_f, "rb") as _fh:
        ANSWERS[os.path.basename(_f)[:-4].encode()] = _fh.read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
LAST_CMD = b"performance"   # the poller's last command: the only answer the 40-line live window still shows at the batch's end
# One unsolicited line the MBB might print mid-answer, for the pass-through check.
UNSOLICITED = b"DEBUG:   09/07/2026 21:58:20.935  x.c : line 650 - Control flags changed\r\n"


class FakeMbb(threading.Thread):
    """Answers commands on the adapter like the MBB until stopped."""

    def __init__(self, ad):
        super().__init__(daemon=True)
        self.ad = ad
        self.stop = threading.Event()
        self.seen = []
        self.first_delay = 0.0   # a pause before the first answer, for a look at the log before the batch fills it

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
                if len(self.seen) == 0 and self.first_delay:
                    time.sleep(self.first_delay)
                self.seen.append(cmd)
                answer = ANSWERS.get(cmd, b"unknown command\r\n")
                if cmd == LAST_CMD:   # an unsolicited line lands in the middle of the last answer, between two of its lines, where the live window still shows it
                    cut = answer.find(b"\r\n", len(answer) // 2) + 2
                    answer = answer[:cut] + UNSOLICITED + answer[cut:]
                self.ad.write(cmd + b"\r\n" + answer + b"ZERO MBB> ")


def post(host, path, body=None):
    req = urllib.request.Request("http://%s%s" % (host, path), data=body, method="POST")
    req.add_header("X-Dongle", "1")
    for attempt in range(4):   # the board may be rejoining WiFi after a wake
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                return r.read().decode()
        except OSError as exc:
            if attempt == 3:
                raise
            print("  note POST %s attempt %d failed (%s); retrying" % (path, attempt + 1, exc))
            time.sleep(3)


def get(host, path):
    with urllib.request.urlopen("http://%s%s" % (host, path), timeout=8) as r:
        return r.status, r.read().decode()


def t_poll(host, ad):
    print("poll")
    mbb = FakeMbb(ad)
    mbb.first_delay = 1.5
    mbb.start()
    try:
        # A scheduled batch may be in flight, timing out command by command
        # with nothing answering: an open console ends it after the current
        # command. Then the schedule goes off, so the batches here are the
        # requested ones; the interval is put back at the end.
        c = console(host)
        interval = json.loads(get(host, "/api/settings")[1])["poll"]
        post(host, "/api/settings", b"poll=0")   # off before the console closes, or the schedule starts one in the gap
        t0 = time.time()
        while status(host)["poll"]["active"] and time.time() - t0 < 60:
            time.sleep(1)
        c.close()
        time.sleep(3)
        mbb.seen.clear()   # the batch just waited out was answered too; the first delay applies to the requested one
        print("  ", post(host, "/api/cmd/poll"))
        time.sleep(0.4)
        live = get(host, "/live")[1]
        tail = live.rstrip().split("\n")[-3:]   # a clock note may land beside it
        check(any(l.endswith("dongle: poll") for l in tail), "the batch is announced in the log before its first answer: %r" % tail)
        held = False
        t0 = time.time()
        while time.time() - t0 < 60:
            s = status(host)
            if s["poll"]["active"] and s["tx_attached"]:
                held = True
            if not s["poll"]["active"] and len(mbb.seen) >= 13:
                break
            time.sleep(0.3)
        check(held, "transmit pin held while the batch ran")
        check(len(mbb.seen) >= 13, "all thirteen commands reached the adapter: %r" % mbb.seen[:13])
        lst = json.loads(get(host, "/api/cmd")[1])
        check(all(c["ok"] for c in lst), "every command closed on its prompt: %r" % [(c["name"], c["ok"]) for c in lst])
        code, bms = get(host, "/api/cmd/bms")
        check(code == 200 and "soc" in bms and "bms" not in bms.splitlines()[0], "bms output kept without its echo: %r" % bms[:60])
        s = status(host)
        import re
        want_state = re.search(rb"Bike State:\s*(\S+)", ANSWERS[b"status"]).group(1).decode()
        want_soc = int(re.search(rb"\n\s*\d+\s+(\d+)\s*%", ANSWERS[b"status"]).group(1))
        check(s["pack"]["soc"] == want_soc and s["pack"]["bike_state"] == want_state,
              "status carries soc %s and state %r (the answer says %d and %r)" % (s["pack"]["soc"], s["pack"]["bike_state"], want_soc, want_state))
        time.sleep(3.5)
        check(not status(host)["tx_attached"], "transmit pin released after the batch")
        live = get(host, "/live")[1]   # the last 40 lines: the end of the batch
        check("Control flags changed" in live, "the unsolicited line inside a response reached the log")
        check("total_Whr" in live and "filt_batt_ma" in live, "the responses themselves reached the log")
        rd = {r["n"]: r for r in json.loads(get(host, "/api/readings")[1])}
        want = {"Motor_Temp": 35, "lowest_cell_voltage_mv": 3976, "Lean": -135, "Odometer_km": 14532, "12V_Battery": 13127, "cell_signal_percent": 0, "Active_DTCs": 1, "total_Whr": 1331983}
        got = {k: rd.get(k, {}).get("v") for k in want}
        check(got == want and all(0 <= rd[k]["age_s"] <= 90 for k in want),
              "the readings carry the figures from eight outputs, fresh: %r" % got)
        check(rd["max_charge_voltage"]["v"] == 117.6 and rd["max_charge_voltage"]["u"] == "V", "a decimal figure keeps its decimals: %r" % rd.get("max_charge_voltage"))
        check("gps_longitude_radians" not in rd and "unit_id" not in rd, "the fix and the unit id are not readings")
        check("Pilot_Current" not in rd, "a row the MBB marks invalid is not a reading")
        # A console client leaves with the MBB's last prompt still in the
        # framer and a batch due at once: the prompt must not close the
        # batch's first command on its own echo.
        time.sleep(4)
        c = console(host)
        print("  ", post(host, "/api/cmd/poll"))   # held while the client is on
        ad.write(b"ZERO MBB> ")                    # no line end: the framer holds it
        time.sleep(0.3)
        c.close()
        mbb.seen.clear()
        t0 = time.time()
        while time.time() - t0 < 60:
            s = status(host)
            if not s["poll"]["active"] and len(mbb.seen) >= 13:
                break
            time.sleep(0.3)
        rows = {r["name"]: r for r in json.loads(get(host, "/api/cmd")[1])}
        body = get(host, "/api/cmd/status")[1]
        check(len(mbb.seen) >= 13 and rows["status"]["ok"] and "Bike State" in body and rows["status"]["bytes"] > 1000,
              "a stale prompt in the framer did not close the batch's first command: status %d bytes" % rows["status"]["bytes"])
        try:
            get(host, "/api/cmd/nope")
            check(False, "unknown command is 404")
        except urllib.error.HTTPError as e:
            check(e.code == 404, "unknown command is 404")
    finally:
        mbb.stop.set()
        mbb.join(1)
        try:
            post(host, "/api/settings", ("poll=%d" % interval).encode())
        except Exception as exc:
            print("  note the poll interval was NOT put back to %s: %s" % (interval, exc))


def t_storage(host, ad):
    print("storage")
    post(host, "/api/settings", b"sleep=1&sleep_days=3")

    def say(line):
        ad.write(line + b"\r\n")
        time.sleep(0.6)
        return status(host)["sleep"]

    s = say(b"LTSM state: INIT to DIS")
    check(s["storage"] == "off" and not s["armed"], "storage mode off from the LTSM line, not armed: %r" % s)
    s = say(b"LTSM state: INIT to EN")   # EN is the state bms shows with the mode on; anything but DIS counts
    check(s["storage"] == "on" and s["armed"], "an LTSM state other than DIS arms the sleep at once: %r" % s)
    s = say(b"DEBUG:   09/07/2026 21:58:20.935  x.c : line 734 - Key Sw = ON")
    check(s["storage"] == "unknown" and not s["armed"], "a key-on forgets storage mode until the MBB restates it: %r" % s)
    s = say(b"     - storage mode      Active")
    check(s["storage"] == "on" and s["armed"], "the bms row arms it too: %r" % s)
    s = say(b"     - storage mode      Inactive")
    check(s["storage"] == "off" and not s["armed"], "Inactive hands the decision back to the days rule: %r" % s)


def t_lightsleep(host, ad):
    print("lightsleep (about three minutes)")
    # Sleep whenever the MBB does, with a 10 s grace: the same code path as
    # the hour-long sleep, with numbers the bench can wait out.
    post(host, "/api/settings", b"sleep=1&sleep_days=0&sleep_grace=10&use_s=5")
    try:
        _t_lightsleep(host, ad)
    finally:
        post(host, "/api/settings", b"sleep_days=3&sleep_grace=120&use_s=600")


def _t_lightsleep(host, ad):
    HIB = 150
    ip = socket.gethostbyname(host)
    ad.write(b"Saving Stats, Hibernating for %d sec\r\n" % HIB)
    time.sleep(0.5)
    before = status(host)
    heap0 = (before["heap_free"], before["heap_max_alloc"])
    fcntl.ioctl(ad.fd, TIOCSBRK)
    t0 = time.time()
    try:
        time.sleep(7)
        check(not status_retry(host)["mbb_awake"], "asleep behind the held low")
        # Off the network once the grace has run from the asleep edge, then up
        # again before the MBB is due: nine tenths of the wait slept, and the
        # sleep timer's clock inside the margin the tenth leaves.
        gone = back = None
        while time.time() - t0 < HIB + 20:
            if up(ip):
                if gone is not None:
                    back = time.time() - t0
                    break
            elif gone is None:
                gone = time.time() - t0
            time.sleep(1)
        check(gone is not None and 13 <= gone <= 30, "went to sleep %s after the low (grace 10 s from the asleep edge)" % ("%d s" % gone if gone else "never"))
        check(back is not None, "back on the network %s after the low (the MBB is due at %d s)" % ("%d s" % back if back else "never", HIB))
        s = status_retry(host)
        planned = s["sleep"]["last_slept_s"]
        # The plan leaves a tenth of what remained (gone is the host's view, up
        # to two seconds late), and the board's own measure of the sleep clock
        # is the NTP step after the wake, which the margin's tenth must cover.
        # The host's return time also holds the LAN's address lookup.
        check(gone is not None and 0 < planned <= 0.9 * (HIB - gone + 3), "slept for nine tenths of what remained at most: %d s planned with about %d s to go" % (planned, HIB - (gone or 0)))
        step = None
        for _ in range(4):
            step = clock_step_after_sleep(get(host, "/live")[1])
            if step is not None:
                break
            time.sleep(2)
        check(step is not None and abs(step) <= planned * 0.10, "the sleep timer's clock ran within the margin: NTP stepped %s s after %d s planned" % (step, planned))
        check(s["sleep"]["count"] == before["sleep"]["count"] + 1 and s["sleep"]["last_wake"] == "timer" and s["sleep"]["slept_for_due"],
              "one sleep, woken by the timer, and no second plan on the remainder: %r" % s["sleep"])
        check(s["boot"] == before["boot"], "no reboot across the sleep")
    finally:
        fcntl.ioctl(ad.fd, TIOCCBRK)
    time.sleep(1.5)
    s = status_retry(host)
    check(s["mbb_awake"], "awake again with the line high")
    # A second cycle ended early by pin 8 rising.
    ad.write(b"Saving Stats, Hibernating for %d sec\r\n" % HIB)
    time.sleep(0.5)
    before = status(host)
    fcntl.ioctl(ad.fd, TIOCSBRK)
    t0 = time.time()
    try:
        gone = None
        while time.time() - t0 < 60:
            try:
                status(host)
                time.sleep(2)
            except Exception:
                gone = time.time() - t0
                break
        check(gone is not None, "second sleep started")
        time.sleep(10)
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
            time.sleep(2)
    check(back is not None, "back on the network %s after pin 8 rose" % ("%.0f s" % back if back else "never"))
    if back:
        check(s["sleep"]["last_wake"] == "pin 8" and s["mbb_awake"], "woken by pin 8 and awake: %r" % s["sleep"])
        ad.write(b"DEBUG: 09/07/2026 19:57:00.000 after the pin 8 wake\r\n")
        time.sleep(3)
        check("after the pin 8 wake" in get(host, "/live")[1], "a line after the pin 8 wake reaches the log")
        c = socket.create_connection((host, 6638), timeout=5)   # the services after a resume, not only the status route
        time.sleep(0.5)
        greet = c.recv(500)
        c.close()
        check(b"console" in greet, "a console client is greeted after the resume: %r" % greet[:40])
        code, listing = get(host, "/logs")
        check(code == 200 and listing.startswith("["), "the file listing answers after the resume")
        s = status(host)
        heap1 = (s["heap_free"], s["heap_max_alloc"])
        # Free heap moves by 10 KB with the poller's outputs and the sockets of
        # the moment; the largest free block is the fragmentation figure.
        check(heap1[0] > heap0[0] - 12288 and heap1[1] > heap0[1] - 6144,
              "two sleep cycles cost the heap nothing lasting: free %d -> %d, largest block %d -> %d" % (heap0[0], heap1[0], heap0[1], heap1[1]))
        check(s["wifi"]["resume_failures"] == 0, "the WiFi driver restarted cleanly each time")


# Which scenarios a change to each source file can break. A file not listed
# gets everything. The order of ORDER is the order they run in: poll before
# sleep, so no fresh awake edge is left to settle after.
ORDER = ["roundtrip", "break", "poll", "storage", "sleep", "lightsleep"]
TOUCHES = {
    "mbb_uart": ["roundtrip", "break", "sleep"],
    "console": ["roundtrip", "lightsleep"],
    "poller": ["poll", "roundtrip"],
    "readings": ["poll"], "rows": ["poll"],
    "mbb_parse": ["poll", "storage"],
    "store": ["poll", "sleep"],
    "zstream": ["sleep"], "dictkeeper": ["sleep"], "names": ["sleep"], "framer": ["roundtrip", "sleep"],
    "sleep": ["storage", "lightsleep"], "hibernate": ["storage", "lightsleep"],
    "clock": ["lightsleep"], "mbb_time": ["sleep"],
    "wlan": ["lightsleep"], "http": ["poll", "lightsleep"], "settings": ["storage", "lightsleep"],
    "json_escape": ["poll"], "util": ["poll"],
}


def changed_scenarios():
    """The scenarios for the firmware files changed since the last tag, the working tree included."""
    import subprocess
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tag = subprocess.run(["git", "describe", "--tags", "--abbrev=0"], cwd=root, capture_output=True, text=True).stdout.strip()
    files = subprocess.run(["git", "diff", "--name-only", tag], cwd=root, capture_output=True, text=True).stdout.split()
    files += subprocess.run(["git", "ls-files", "--others", "--exclude-standard"], cwd=root, capture_output=True, text=True).stdout.split()   # new files not yet added
    picked = set()
    for f in files:
        if not f.startswith("firmware/src/"):
            continue
        stem = os.path.basename(f).split(".")[0]
        if stem == "config":   # a version bump alone touches nothing
            diff = subprocess.run(["git", "diff", tag, "--", f], cwd=root, capture_output=True, text=True).stdout
            if not any(l.startswith(("+#", "-#")) and "FW_VERSION" not in l for l in diff.splitlines()):
                continue
        if stem in ("main", "config"):
            return ORDER
        picked.update(TOUCHES.get(stem, ORDER))
    return [s for s in ORDER if s in picked]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("test", nargs="*", default=["all"], choices=ORDER + ["quick", "auto", "all"])
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
    tests = {"roundtrip": t_roundtrip, "break": t_break, "poll": t_poll, "storage": t_storage, "sleep": t_sleep, "lightsleep": t_lightsleep}
    names = []
    for t in args.test:
        if t == "all": names += ORDER
        elif t == "quick": names += [s for s in ORDER if s != "lightsleep"]
        elif t == "auto":
            picked = changed_scenarios()
            print("bench: auto picked %s" % (", ".join(picked) if picked else "nothing: no firmware source changed since the last tag"))
            names += picked
        else: names.append(t)
    names = [s for s in ORDER if s in names]
    for name in names:
        try:
            tests[name](args.host, ad)
        except Exception as exc:
            check(False, "%s raised %s: %s" % (name, type(exc).__name__, exc))
        ad.reset_input_buffer()
    print("bench: %d check(s) failed" % len(fails) if fails else "bench: all checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
