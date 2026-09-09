#!/usr/bin/env python3
"""Does waking the MBB, and holding it awake, make the cellular module check in?

Usage: tools/ccm-wake-experiment.py [--host H] [--hold S] [--every S]
                                   [--cycles N] [--out FILE]

Each cycle posts /api/wake with a hold (pin 9 high wakes a hibernating MBB
and keeps it awake for the hold), then samples the board every 10 s for
the hold plus a minute: MBB awake, the transmit pin, and the readings the
poller takes from ccm and in while the MBB is up: cell registration,
Starcom connection, signal, GPS validity, the heartbeat SOC and the 12 V
rails. Between cycles it samples once a minute, so the MBB's own hibernate
and the hourly wakes are on the same record. One CSV line per sample, to
--out (default logs/ccm-wake-<date>.csv). Compare the registration and
connection columns against the app's "checked in" stamp afterwards.

The bike must be off and the MBB hibernating; the dongle on mains.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

READ = ["cell_network_registration", "connected_to_starcom", "cell_signal_percent", "gps_is_valid", "hb_soc",
        "12V_Battery", "DC-DC"]


def get(host, path, timeout=8):
    with urllib.request.urlopen("http://%s%s" % (host, path), timeout=timeout) as r:
        return json.loads(r.read())


def post(host, path):
    req = urllib.request.Request("http://%s%s" % (host, path), data=b"", method="POST")
    req.add_header("X-Dongle", "1")
    with urllib.request.urlopen(req, timeout=8) as r:
        return r.read().decode()


def sample(host, phase, out):
    row = {"t": time.strftime("%Y-%m-%dT%H:%M:%S"), "phase": phase}
    try:
        s = get(host, "/api/status")
        row.update(awake=int(s["mbb_awake"]), tx=int(s["tx_attached"]), boot=s["boot"], uptime=s["uptime_s"],
                   wake_hold_s=s.get("wake_hold_s", ""))
        r = {x["n"]: x for x in get(host, "/api/readings")}
        for n in READ:
            x = r.get(n)
            row[n] = x["v"] if x else ""
            row[n + "_age"] = x["age_s"] if x else ""
    except Exception as exc:
        row["error"] = str(exc)[:60]
    line = ",".join(str(row.get(k, "")) for k in COLS)
    out.write(line + "\n"); out.flush()
    print(line); sys.stdout.flush()


COLS = ["t", "phase", "awake", "tx", "boot", "uptime", "wake_hold_s"] + [c for n in READ for c in (n, n + "_age")] + ["error"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("DONGLE_HOST", "192.168.0.156"))
    ap.add_argument("--hold", type=int, default=300)
    ap.add_argument("--every", type=int, default=1800)
    ap.add_argument("--cycles", type=int, default=8)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out_path = a.out or os.path.join("logs", "ccm-wake-%s.csv" % time.strftime("%Y%m%d-%H%M"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "a") as out:
        out.write(",".join(COLS) + "\n")
        for cycle in range(a.cycles):
            t0 = time.time()
            sample(a.host, "before-%d" % cycle, out)
            try:
                print("wake:", post(a.host, "/api/wake?hold=%d" % a.hold))
            except Exception as exc:
                print("wake failed:", exc)
            end = time.time() + a.hold + 60
            while time.time() < end:
                sample(a.host, "hold-%d" % cycle, out)
                time.sleep(10)
            while time.time() - t0 < a.every:
                sample(a.host, "between-%d" % cycle, out)
                time.sleep(60)
    print("done:", out_path)


if __name__ == "__main__":
    main()
