#!/usr/bin/env python3
"""Load the dongle's pulled session logs into SQLite, and query them.

Usage: tools/logdb.py [--db logs/dongle.db] [--tz America/Los_Angeles] ingest DIR|FILE...
       tools/logdb.py [--db ...] sessions|names|events|series NAME|search QUERY [--since S] [--until U]

The schema and what each table is for are in docs/data-model.md. Ingest is
idempotent per file: a file already loaded at the same size is skipped, one
that grew is reloaded. Stamps are kept as the dongle wrote them, local time in
its zone, and also as an epoch computed in --tz. Standard library only, so
the puller's host can run it; the MCP server in tools/mcp imports it.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
from datetime import datetime, timedelta

try:
    from zoneinfo import ZoneInfo
except ImportError:   # Python < 3.9
    ZoneInfo = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_DB = os.path.join(ROOT, "logs", "dongle.db")
DEFAULT_LOGS = os.path.join(ROOT, "logs", "dongle")
DEFAULT_TZ = "America/Los_Angeles"   # the firmware's TZ_DEFAULT, PST8PDT

ISO = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}$")
BOOT = re.compile(r"^u\d{6}\.\d{3}$")
MBB = re.compile(r"^(?:DEBUG:)?\s*(\d\d)/(\d\d)/(\d{4}) (\d\d):(\d\d):(\d\d)\.(\d{3}) - ?(.*)$")
PROMPT = "ZERO MBB>"
COMMANDS = ("status", "charging", "bms", "pdu", "in", "faults", "bms interface", "controller",
            "msc", "dash info", "ccm", "obd", "performance", "state", "stats", "config")
SESSION_START = re.compile(r"dongle: session start(?:, (?P<board>[\w-]+), id (?P<sid>b\d+-\d+), part (?P<part>\d+))?"
                           r", boot (?P<boot>\d+)(?: \((?P<reason>[^)]*)\))?, time (?P<clock>\w+), fw (?P<fw>\S+?)(?:,|$)")

SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY, file TEXT UNIQUE NOT NULL, size INTEGER NOT NULL,
    board TEXT, session_id TEXT, part INTEGER, boot INTEGER, boot_reason TEXT, fw TEXT, clock TEXT,
    started TEXT, ended TEXT, lines INTEGER, batches INTEGER, events INTEGER, ingested REAL);
CREATE TABLE IF NOT EXISTS lines (
    id INTEGER PRIMARY KEY, session INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    seq INTEGER NOT NULL, t TEXT, epoch REAL, kind TEXT NOT NULL, batch INTEGER, command TEXT, mbb_t TEXT, text TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS lines_t ON lines(t);
CREATE INDEX IF NOT EXISTS lines_kind_t ON lines(kind, t);
CREATE INDEX IF NOT EXISTS lines_session ON lines(session, seq);
CREATE TABLE IF NOT EXISTS batches (
    id INTEGER PRIMARY KEY, session INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    seq INTEGER NOT NULL, t TEXT, epoch REAL, source TEXT NOT NULL, commands INTEGER);
CREATE TABLE IF NOT EXISTS commands (
    id INTEGER PRIMARY KEY, session INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    batch INTEGER NOT NULL REFERENCES batches(id) ON DELETE CASCADE, name TEXT NOT NULL, t TEXT, epoch REAL, output TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS commands_name_t ON commands(name, t);
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY, session INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    batch INTEGER, command TEXT, line INTEGER, t TEXT, epoch REAL, name TEXT NOT NULL, value REAL NOT NULL, valid INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS readings_name_t ON readings(name, t);
CREATE VIRTUAL TABLE IF NOT EXISTS lines_fts USING fts5(text, content='lines', content_rowid='id');
"""


def open_db(path=DEFAULT_DB):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.executescript(SCHEMA)
    return db


# --- parsing -----------------------------------------------------------------

def parse_number(s):
    """A console figure: signed, up to three decimals, ended by a comma, space or the end. None if not one."""
    m = re.match(r"^([-+]?\d+(?:\.\d+)?)(?:[,\s]|$)", s)
    return float(m.group(1)) if m else None


def parse_row(text):
    """A comma-table or dash-list row to (name, value, valid), mirroring firmware/src/pure/rows.h."""
    s = text.lstrip(" \t")
    if not s:
        return None
    if s.startswith("- "):
        parts = s[2:].split(None, 2)
        if len(parts) < 2:
            return None
        v = parse_number(parts[1])
        return (parts[0], v, True) if v is not None else None
    if "," not in s:
        return None
    name, rest = s.split(",", 1)
    name = name.rstrip()
    if not name or " " in name and len(name) > 40:
        return None
    rest = rest.lstrip()
    v = parse_number(rest)
    if v is None:
        return None
    fields = [f.strip() for f in rest.split(",")]
    valid = not (len(fields) >= 3 and fields[2] == "No")
    return (name, v, valid)


def load_units():
    """Name to unit from the firmware's whitelist, so the archive and the board agree."""
    path = os.path.join(ROOT, "firmware", "src", "readings.cpp")
    try:
        src = open(path, encoding="utf-8").read()
    except OSError:
        return {}
    return {n: (g, u) for n, g, u in re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]*)"\}', src)}


def split_stamp(raw):
    """(stamp, text) for a log line; stamp None when the line has no recognisable one."""
    stamp, _, text = raw.partition(" ")
    if ISO.match(stamp) or BOOT.match(stamp):
        return stamp, text
    return None, raw


class Ingester:
    def __init__(self, db, tz=DEFAULT_TZ):
        self.db = db
        self.zone = ZoneInfo(tz) if ZoneInfo else None

    def epoch(self, stamp):
        if not stamp or not ISO.match(stamp):
            return None
        dt = datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%S.%f")
        if self.zone:
            dt = dt.replace(tzinfo=self.zone)
        return dt.timestamp()

    def ingest_file(self, path):
        """Load one file. Returns 'loaded', 'reloaded' or 'unchanged'."""
        name = os.path.basename(path)
        size = os.path.getsize(path)
        old = self.db.execute("SELECT id, size FROM sessions WHERE file=?", (name,)).fetchone()
        if old and old["size"] == size:
            return "unchanged"
        if old:
            self.db.execute("DELETE FROM sessions WHERE id=?", (old["id"],))
        with open(path, encoding="utf-8", errors="replace") as f:
            raw_lines = [l.rstrip("\r\n") for l in f]
        sid = self.db.execute("INSERT INTO sessions(file, size, ingested) VALUES(?,?,?)",
                              (name, size, time.time())).lastrowid
        self._load_lines(sid, raw_lines)
        return "reloaded" if old else "loaded"

    def _load_lines(self, sid, raw_lines):
        db = self.db
        meta = {}
        started = ended = None
        clock = "boot"
        batch = None          # current batch id
        batch_seq = 0
        cmd = None            # (name, start line id, stamp, [output lines])
        await_echo = False    # a prompt just closed a command; the next line is an echo or the batch is over
        n_events = 0

        def close_cmd():
            nonlocal cmd
            if cmd and batch:
                name, stamp, out = cmd
                cid = db.execute("INSERT INTO commands(session, batch, name, t, epoch, output) VALUES(?,?,?,?,?,?)",
                                 (sid, batch, name, stamp, self.epoch(stamp), "\n".join(l for _, l in out) + "\n")).lastrowid
                rows = []
                for line_id, text in out:
                    r = parse_row(text)
                    if r:
                        rows.append((sid, batch, name, line_id, stamp, self.epoch(stamp), r[0], r[1], int(r[2])))
                if rows:
                    db.executemany("INSERT INTO readings(session, batch, command, line, t, epoch, name, value, valid) VALUES(?,?,?,?,?,?,?,?,?)", rows)
                db.execute("UPDATE batches SET commands=commands+1 WHERE id=?", (batch,))
            cmd = None

        def open_batch(stamp, source):
            nonlocal batch, batch_seq
            close_cmd()
            batch_seq += 1
            batch = db.execute("INSERT INTO batches(session, seq, t, epoch, source, commands) VALUES(?,?,?,?,?,0)",
                               (sid, batch_seq, stamp, self.epoch(stamp), source)).lastrowid

        def end_batch():
            nonlocal batch, await_echo
            close_cmd()
            batch = None
            await_echo = False

        for seq, raw in enumerate(raw_lines, 1):
            stamp, text = split_stamp(raw)
            if stamp and ISO.match(stamp):
                clock = "wall"
                started = started or stamp
                ended = stamp
            else:
                stamp = None   # a boot-relative uSSSSSS.mmm stamp is not a time; it must not sort into ranges
            s = text.strip()
            kind = "mbb"
            mbb_t = None
            in_batch_cmd = None

            m = MBB.match(text)
            if m:
                kind = "event"
                mbb_t = "%s-%s-%sT%s:%s:%s.%s" % (m.group(3), m.group(1), m.group(2), m.group(4), m.group(5), m.group(6), m.group(7))
                n_events += 1
                if await_echo:
                    end_batch()
            elif s.startswith("dongle:") or s.startswith("[dongle:"):
                kind = "dongle"
                n_events += 1
                if s == "dongle: poll":
                    open_batch(stamp, "poll")
                    await_echo = True
                elif s.startswith("dongle: session"):
                    end_batch()
                    sm = SESSION_START.search(s)
                    if sm and not meta:
                        meta = {k: v for k, v in sm.groupdict().items() if v is not None}
            elif s.startswith(PROMPT):
                kind = "prompt"
                rest = s[len(PROMPT):].strip()
                if batch:
                    close_cmd()
                    await_echo = True
                    in_batch_cmd = None
                if rest in COMMANDS:   # an echo sharing the prompt's line
                    if not batch:
                        open_batch(stamp, "console")
                    cmd = (rest, stamp, [])
                    await_echo = False
                    kind = "echo"
            elif s in COMMANDS and (await_echo or not batch):
                if not batch:
                    open_batch(stamp, "console")
                kind = "echo"
                cmd = (s, stamp, [])
                await_echo = False
            elif batch:
                if await_echo:
                    end_batch()   # the batch ended and the MBB printed something on its own
                elif cmd:
                    kind = "output"
                    in_batch_cmd = cmd[0]

            line_id = db.execute(
                "INSERT INTO lines(session, seq, t, epoch, kind, batch, command, mbb_t, text) VALUES(?,?,?,?,?,?,?,?,?)",
                (sid, seq, stamp, self.epoch(stamp), kind, batch if kind in ("echo", "output", "prompt") else None,
                 in_batch_cmd or (cmd[0] if kind == "echo" and cmd else None), mbb_t, text)).lastrowid
            if kind == "output":
                cmd[2].append((line_id, text))
        end_batch()

        db.execute("""UPDATE sessions SET board=?, session_id=?, part=?, boot=?, boot_reason=?, fw=?, clock=?,
                      started=?, ended=?, lines=?, batches=?, events=? WHERE id=?""",
                   (meta.get("board"), meta.get("sid"), meta.get("part"), meta.get("boot"), meta.get("reason"),
                    meta.get("fw"), clock, started, ended, len(raw_lines), batch_seq, n_events, sid))


def ingest(db, paths, tz=DEFAULT_TZ):
    """Load every .log under the given files and directories. Returns counts by outcome."""
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in names if n.endswith(".log")]
        elif p.endswith(".log"):
            files.append(p)
    files.sort(key=os.path.basename)
    ing = Ingester(db, tz)
    counts = {"loaded": 0, "reloaded": 0, "unchanged": 0, "dropped": 0}
    for f in files:
        with db:
            counts[ing.ingest_file(f)] += 1
    if any(os.path.isdir(p) for p in paths):   # a directory is the whole truth: a session whose file is gone goes too
        present = {os.path.basename(f) for f in files}
        with db:
            for r in db.execute("SELECT id, file FROM sessions").fetchall():
                if r["file"] not in present:
                    db.execute("DELETE FROM sessions WHERE id=?", (r["id"],))
                    counts["dropped"] += 1
    if counts["loaded"] or counts["reloaded"] or counts["dropped"]:
        with db:
            db.execute("INSERT INTO lines_fts(lines_fts) VALUES('rebuild')")
    return counts


# --- queries -----------------------------------------------------------------

def when(s, tz=DEFAULT_TZ):
    """A range bound: an ISO prefix as given, or -7d / -12h / -30m relative to now in the dongle's zone."""
    if not s:
        return None
    m = re.match(r"^-(\d+)([dhm])$", s)
    if not m:
        return s
    n, unit = int(m.group(1)), m.group(2)
    delta = timedelta(**{{"d": "days", "h": "hours", "m": "minutes"}[unit]: n})
    now = datetime.now(ZoneInfo(tz)) if ZoneInfo else datetime.now()
    return (now - delta).strftime("%Y-%m-%dT%H:%M:%S.000")


def _range(col, since, until, args):
    sql = ""
    if since:
        sql += " AND %s >= ?" % col
        args.append(when(since))
    if until:
        sql += " AND %s < ?" % col
        args.append(when(until))
    return sql


def sessions(db, since=None, until=None, limit=50):
    args = []
    rows = db.execute("SELECT * FROM sessions WHERE 1=1" + _range("started", since, until, args) +
                      " ORDER BY started DESC, file DESC LIMIT ?", args + [limit]).fetchall()
    out = []
    for r in rows:
        states = [x[0] for x in db.execute(
            "SELECT text FROM lines WHERE session=? AND kind='event' AND text LIKE '%State change from%' ORDER BY seq", (r["id"],))]
        states = [re.sub(r".*State change from (\w+) to (\w+).*", r"\1>\2", s) for s in states]
        d = dict(r)
        d.pop("ingested", None)
        d["states"] = states
        out.append(d)
    return out


def events(db, since=None, until=None, pattern=None, kinds=("event", "dongle"), limit=200):
    args = [*kinds]
    sql = "SELECT id, session, t, mbb_t, kind, text FROM lines WHERE kind IN (%s)" % ",".join("?" * len(kinds))
    sql += _range("t", since, until, args)
    if pattern:
        sql += " AND text LIKE ?"
        args.append("%" + pattern + "%")
    sql += " ORDER BY t DESC, id DESC LIMIT ?"
    args.append(limit)
    rows = [dict(r) for r in db.execute(sql, args)]
    rows.reverse()
    return rows


def reading_names(db, pattern=None):
    units = load_units()
    args = []
    sql = "SELECT name, command, COUNT(*) n, MIN(t) first, MAX(t) last FROM readings WHERE 1=1"
    if pattern:
        sql += " AND name LIKE ?"
        args.append("%" + pattern + "%")
    sql += " GROUP BY name, command ORDER BY name, command"
    out = []
    for r in db.execute(sql, args):
        d = dict(r)
        last = db.execute("SELECT value FROM readings WHERE name=? AND command=? ORDER BY t DESC LIMIT 1", (r["name"], r["command"])).fetchone()
        d["last_value"] = last[0] if last else None
        g = units.get(r["name"])
        d["group"], d["unit"] = (g if g else (None, None))
        out.append(d)
    return out


def series(db, name, since=None, until=None, command=None, step_s=0, limit=500, valid_only=True):
    args = [name]
    sql = "SELECT t, epoch, value, command, valid FROM readings WHERE name=?" + _range("t", since, until, args)
    if command:
        sql += " AND command=?"
        args.append(command)
    if valid_only:
        sql += " AND valid=1"
    sql += " ORDER BY t"
    rows = db.execute(sql, args).fetchall()
    units = load_units().get(name)
    out = {"name": name, "unit": units[1] if units else None, "samples": len(rows), "points": []}
    if not rows:
        return out
    vals = [r["value"] for r in rows]
    out.update(min=min(vals), max=max(vals), first=rows[0]["t"], last=rows[-1]["t"])
    cmds = sorted({r["command"] for r in rows})
    if len(cmds) > 1 and not command:
        out["commands"] = cmds
    pts = []
    if step_s and step_s > 0:
        bucket = None
        acc = []
        for r in rows:
            b = int(r["epoch"] // step_s) if r["epoch"] else None
            if b != bucket and acc:
                pts.append({"t": acc[0]["t"], "v": round(sum(a["value"] for a in acc) / len(acc), 3), "n": len(acc)})
                acc = []
            bucket = b
            acc.append(r)
        if acc:
            pts.append({"t": acc[0]["t"], "v": round(sum(a["value"] for a in acc) / len(acc), 3), "n": len(acc)})
    else:
        pts = [{"t": r["t"], "v": r["value"]} for r in rows]
    if len(pts) > limit:
        out["note"] = "%d points, showing the last %d; pass step_s to downsample" % (len(pts), limit)
        pts = pts[-limit:]
    out["points"] = pts
    return out


def snapshot(db, command, at=None, session=None):
    args = [command]
    sql = "SELECT c.id, c.session, c.batch, c.name, c.t, c.output, s.file FROM commands c JOIN sessions s ON s.id=c.session WHERE c.name=?"
    if session:
        sql += " AND (c.session=? OR s.session_id=? OR s.file=?)"
        args += [session if str(session).isdigit() else -1, session, session]
    if at:
        sql += " AND c.t <= ? ORDER BY c.t DESC LIMIT 1"
        args.append(when(at))
    else:
        sql += " ORDER BY c.t DESC LIMIT 1"
    r = db.execute(sql, args).fetchone()
    return dict(r) if r else None


def search(db, query, since=None, until=None, limit=100):
    args = []
    rng = _range("l.t", since, until, args)
    try:
        q = '"%s"' % query.replace('"', '""')
        rows = db.execute("SELECT l.id, l.session, l.t, l.kind, l.command, l.text FROM lines_fts f JOIN lines l ON l.id=f.rowid "
                          "WHERE lines_fts MATCH ?" + rng + " ORDER BY l.t DESC LIMIT ?", [q] + args + [limit]).fetchall()
    except sqlite3.OperationalError:
        rows = []
    if not rows:
        rows = db.execute("SELECT l.id, l.session, l.t, l.kind, l.command, l.text FROM lines l WHERE l.text LIKE ?" + rng +
                          " ORDER BY l.t DESC LIMIT ?", ["%" + query + "%"] + args + [limit]).fetchall()
    out = [dict(r) for r in rows]
    out.reverse()
    return out


def session_lines(db, session, offset=0, limit=200, kinds=None):
    args = [session if str(session).isdigit() else -1, session, session]
    s = db.execute("SELECT id FROM sessions WHERE id=? OR session_id=? OR file=?", args).fetchone()
    if not s:
        return {"error": "no such session"}
    args = [s["id"]]
    sql = "SELECT seq, t, kind, command, text FROM lines WHERE session=?"
    if kinds:
        sql += " AND kind IN (%s)" % ",".join("?" * len(kinds))
        args += list(kinds)
    sql += " ORDER BY seq LIMIT ? OFFSET ?"
    args += [limit, offset]
    return {"session": s["id"], "lines": [dict(r) for r in db.execute(sql, args)]}


# --- command line ------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--tz", default=DEFAULT_TZ)
    ap.add_argument("--since")
    ap.add_argument("--until")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--step", type=int, default=0, help="series: seconds per point")
    ap.add_argument("verb", choices=["ingest", "sessions", "names", "events", "series", "search", "snapshot"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args(argv)
    db = open_db(a.db)
    if a.verb == "ingest":
        print(json.dumps(ingest(db, a.args or [DEFAULT_LOGS], a.tz)))
        return 0
    if a.verb == "sessions":
        out = sessions(db, a.since, a.until, a.limit)
    elif a.verb == "names":
        out = reading_names(db, a.args[0] if a.args else None)
    elif a.verb == "events":
        out = events(db, a.since, a.until, a.args[0] if a.args else None, limit=a.limit)
    elif a.verb == "series":
        out = series(db, a.args[0], a.since, a.until, step_s=a.step, limit=a.limit)
    elif a.verb == "search":
        out = search(db, " ".join(a.args), a.since, a.until, a.limit)
    else:
        out = snapshot(db, a.args[0], a.args[1] if len(a.args) > 1 else None)
    json.dump(out, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
