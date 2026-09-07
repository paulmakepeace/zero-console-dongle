#!/usr/bin/env python3
"""Fetch the dongle's log files over WiFi and delete them from the dongle
once safely stored.

Usage: tools/pull-logs.py [--host zero-dongle.local] [--dest logs/dongle] [--keep]

Skips the file the dongle is still writing. A file is deleted from the dongle
only after the download's size matches what the dongle reported. --keep
downloads without deleting. Exit status is non-zero if the dongle could not
be reached or any file failed.
"""
import argparse
import json
import os
import re
import sys
import urllib.request

NAME_OK = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")


def fetch(url, method="GET", timeout=60):
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="zero-dongle.local")
    ap.add_argument("--dest", default=os.path.join(repo, "logs", "dongle"))
    ap.add_argument("--keep", action="store_true", help="do not delete from the dongle")
    args = ap.parse_args()
    base = "http://%s" % args.host
    os.makedirs(args.dest, exist_ok=True)

    try:
        files = json.loads(fetch(base + "/logs", timeout=15))
    except Exception as exc:
        sys.exit("pull-logs: cannot list %s: %s" % (base, exc))

    failed = 0
    got = 0
    for f in sorted(files, key=lambda x: x["name"]):
        name, size = str(f.get("name", "")), int(f.get("size", -1))
        if not NAME_OK.match(name) or ".." in name:
            print("FAIL  %r: name rejected" % name)
            failed += 1
            continue
        if f.get("active"):
            print("skip  %s (active)" % name)
            continue
        dest = os.path.join(args.dest, name)
        try:
            data = fetch("%s/logs/%s" % (base, name))
        except Exception as exc:
            print("FAIL  %s: %s" % (name, exc))
            failed += 1
            continue
        if len(data) != size:
            print("FAIL  %s: got %d bytes, dongle reports %d" % (name, len(data), size))
            failed += 1
            continue
        if os.path.exists(dest) and os.path.getsize(dest) != len(data):
            stem, ext = os.path.splitext(dest)
            n = 2
            while os.path.exists("%s-%d%s" % (stem, n, ext)):
                n += 1
            dest = "%s-%d%s" % (stem, n, ext)
        tmp = dest + ".part"
        with open(tmp, "wb") as out:
            out.write(data)
        os.replace(tmp, dest)
        got += 1
        if args.keep:
            print("saved %s, %d bytes" % (name, size))
            continue
        try:
            fetch("%s/logs/%s" % (base, name), method="DELETE", timeout=15)
            print("moved %s, %d bytes" % (name, size))
        except Exception as exc:
            print("saved %s but delete failed: %s" % (name, exc))
            failed += 1
    print("pull-logs: %d file(s) fetched, %d failed" % (got, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
