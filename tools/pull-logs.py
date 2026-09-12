#!/usr/bin/env python3
"""Fetch the dongle's log files over WiFi and delete them from the dongle
once safely stored.

Usage: tools/pull-logs.py [--host zero-dongle-a12c.local] [--dest DIR] [--keep]

Each board names itself from the last four hex digits of its MAC; the
default host is this bike's unit and DONGLE_HOST in the environment
overrides it. Files go to logs/dongle/NAME/, one directory per board, unless
--dest names a directory, which is then used as given.

Skips the file the dongle is still writing. A `.log.z` (zlib, with the
dongle's own dictionary named in its header) or `.log.gz` file is inflated
and stored as the plain `.log`; one without its trailer, cut off by a power
loss, is stored as far as it decodes and reported as truncated. Dictionaries
are fetched from the dongle by id when first needed and kept under
`dicts/` beside the files; the dongle's `dict-*` files are never deleted
by this script. A
file is deleted from the dongle only after the download's size matches
what the dongle reported, the stream decodes, and the file and its
directory entry are on disk. --keep downloads without
deleting. One run at a time per destination; a second run exits at once.
Exit status is non-zero if the dongle's status or listing could not be read
or any file failed. Needs Python 3.6 or later and nothing outside the
standard library.
"""
import argparse
import fcntl
import glob
import http.client
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlog import NAME_OK, dictionary_id, inflate   # noqa: E402


def warn(text):
    print(text, file=sys.stderr, flush=True)


class HttpFail(Exception):
    """An HTTP error with the dongle's own reason text."""
    def __init__(self, code, reason):
        super().__init__("%d %s" % (code, reason))
        self.code = code


def fetch(url, method="GET", timeout=60, tries=2):
    req = urllib.request.Request(url, method=method)
    if method != "GET":
        req.add_header("X-Dongle", "1")   # state changes need this; a cross-site form cannot send it
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as exc:
            raise HttpFail(exc.code, exc.read().decode("utf-8", "replace").strip()) from None
        except (OSError, ValueError, http.client.HTTPException) as exc:   # timeouts, resets, short bodies
            if attempt + 1 == tries:
                raise
            warn("pull-logs: %s %s: %s; retrying" % (method, url, exc))
            time.sleep(2)


def dictionary(base, dest, did):
    """The dictionary with this id, from the local cache or the dongle."""
    cache = os.path.join(dest, "dicts")
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, "%08x.txt" % did)
    if os.path.exists(path):
        with open(path, "rb") as f:
            d = f.read()
    else:
        d = fetch("%s/logs/dict-%08x.txt" % (base, did), timeout=30)
        if zlib.adler32(d) & 0xFFFFFFFF != did:
            raise ValueError("the dongle's dict-%08x.txt does not match its id" % did)
        tmp = "%s.part.%d" % (path, os.getpid())
        with open(tmp, "wb") as f:
            f.write(d)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    return d


def resolve(host):
    """One name lookup per run: mDNS can take seconds, and the firmware ignores the Host header."""
    try:
        infos = socket.getaddrinfo(host, 80, socket.AF_INET, socket.SOCK_STREAM)
        return infos[0][4][0]
    except OSError:
        return host


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=os.environ.get("DONGLE_HOST", "zero-dongle-a12c.local"))
    ap.add_argument("--dest", default=None, help="directory, used as given (default logs/dongle/NAME)")
    ap.add_argument("--keep", action="store_true", help="do not delete from the dongle")
    args = ap.parse_args()
    base = "http://%s" % resolve(args.host)

    try:
        st = json.loads(fetch(base + "/api/status", timeout=15))
    except Exception as exc:
        sys.exit("pull-logs: status of %s not readable: %s" % (args.host, exc))
    fs, uart = st.get("fs", {}), st.get("uart", {})
    if not fs.get("ok", True):
        warn("pull-logs: WARNING the dongle reports no working filesystem")
    if fs.get("formats"):
        warn("pull-logs: note: the dongle has formatted its log area %s time(s)" % fs["formats"])
    if st.get("dropped_lines"):
        warn("pull-logs: WARNING %s line(s) dropped on the dongle since it booted" % st["dropped_lines"])
    if uart.get("overflows") or uart.get("queue_drops") or uart.get("frame_errors"):
        warn("pull-logs: note: UART overruns %s, frame errors %s, queue drops %s since boot"
             % (uart.get("overflows"), uart.get("frame_errors"), uart.get("queue_drops")))
    if args.dest is None:
        board = st.get("name")
        if not isinstance(board, str) or not NAME_OK.match(board):
            sys.exit("pull-logs: the dongle's name %r is not usable as a directory" % (board,))
        args.dest = os.path.join(repo, "logs", "dongle", board)
    os.makedirs(args.dest, exist_ok=True)

    lock = open(os.path.join(args.dest, ".pull-lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        warn("pull-logs: another pull is running for %s" % args.dest)
        sys.exit(0)
    for stale in glob.glob(os.path.join(args.dest, "*.part.*")):
        os.remove(stale)

    try:
        files = json.loads(fetch(base + "/logs", timeout=15))
        if not isinstance(files, list) or not all(isinstance(f, dict) for f in files):
            raise ValueError("listing is not a list of objects")
    except Exception as exc:
        sys.exit("pull-logs: cannot list %s: %s" % (args.host, exc))

    failed = 0
    got = 0
    for f in sorted(files, key=lambda x: str(x.get("name", ""))):
        name = f.get("name")
        if not isinstance(name, str) or not NAME_OK.match(name) or ".." in name:
            warn("FAIL  %r: name rejected" % (name,))
            failed += 1
            continue
        try:
            size = int(f.get("size"))
            if size < 0:
                raise ValueError
        except (TypeError, ValueError):
            warn("FAIL  %s: size %r rejected" % (name, f.get("size")))
            failed += 1
            continue
        if f.get("active"):
            print("skip  %s (active)" % name)
            continue
        if name.startswith("dict-"):
            continue   # fetched by id when a file needs it, never deleted from here
        try:
            data = fetch("%s/logs/%s" % (base, name))
        except HttpFail as exc:
            if exc.code == 409:
                print("skip  %s (became active)" % name)
            elif exc.code == 404:
                warn("LOST  %s: gone from the dongle since the listing (%s)" % (name, exc))
                failed += 1
            else:
                warn("FAIL  %s: %s" % (name, exc))
                failed += 1
            continue
        except Exception as exc:
            warn("FAIL  %s: %s" % (name, exc))
            failed += 1
            continue
        if len(data) != size:
            warn("FAIL  %s: got %d bytes, dongle reports %d" % (name, len(data), size))
            failed += 1
            continue
        if size == 0:
            print("note  %s is empty" % name)
        local = name
        raw = None
        if name.endswith(".gz") or name.endswith(".z"):
            local = name[:-3] if name.endswith(".gz") else name[:-2]
            raw = data
            zdict = None
            did = dictionary_id(raw)
            if did is not None:
                try:
                    zdict = dictionary(base, args.dest, did)
                except Exception as exc:
                    # Keep the bytes: the dictionary may turn up later; the dongle's copy stays.
                    keep = os.path.join(args.dest, name)
                    with open(keep, "wb") as f:
                        f.write(raw)
                    warn("FAIL  %s: needs dictionary %08x (%s); raw kept as %s, not deleted" % (name, did, exc, keep))
                    failed += 1
                    continue
            data, complete, note = inflate(raw, zdict)
            if note:
                warn("note  %s: %s; the raw .gz is kept beside the .log" % (name, note))
            elif not complete and size:
                print("note  %s is truncated; %d line(s) recovered" % (name, data.count(b"\n")))
            if not note:
                raw = None   # a clean stream: the plain file is the record

        def same(path):
            if not os.path.isfile(path):
                return False
            with open(path, "rb") as existing:
                return existing.read() == data

        dest = os.path.join(args.dest, local)
        try:
            if os.path.exists(dest) and not same(dest):
                stem, ext = os.path.splitext(dest)
                n = 2
                while os.path.exists("%s-%d%s" % (stem, n, ext)) and not same("%s-%d%s" % (stem, n, ext)):
                    n += 1
                dest = "%s-%d%s" % (stem, n, ext)
            if not same(dest):
                tmp = "%s.part.%d" % (dest, os.getpid())
                with open(tmp, "wb") as out:
                    out.write(data)
                    out.flush()
                    os.fsync(out.fileno())
                os.replace(tmp, dest)
                if raw is not None:   # the damaged stream itself, for a second look, beside its .log
                    rawdest = dest + (".gz" if name.endswith(".gz") else ".z")
                    rawtmp = "%s.part.%d" % (rawdest, os.getpid())
                    with open(rawtmp, "wb") as out:
                        out.write(raw)
                        out.flush()
                        os.fsync(out.fileno())
                    os.replace(rawtmp, rawdest)
                d = os.open(args.dest, os.O_RDONLY)   # the rename itself, before the dongle copy goes
                try:
                    os.fsync(d)
                finally:
                    os.close(d)
        except OSError as exc:
            warn("FAIL  %s: local write: %s" % (name, exc))
            failed += 1
            continue
        got += 1
        if args.keep:
            print("saved %s, %d bytes" % (name, size))
            continue
        try:
            try:
                fetch("%s/logs/%s" % (base, name), method="DELETE", timeout=15)
            except HttpFail as exc:
                if exc.code != 404:   # a lost reply to a delete that happened is not a failure
                    raise
            print("moved %s, %d bytes" % (name, size))
        except Exception as exc:
            warn("saved %s but delete failed: %s" % (name, exc))
            failed += 1
    print("pull-logs: %d file(s) fetched, %d failed" % (got, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
