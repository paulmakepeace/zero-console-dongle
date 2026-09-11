#!/usr/bin/env python3
"""The dongle's MCP server: the SQLite archive and the live board as tools.

Run by tools/mcp/run.sh over stdio, registered for Claude Code in .mcp.json at
the repo root. Environment: DONGLE_DB (default logs/dongle.db), DONGLE_LOGS
(default logs/dongle, where pull-logs.py puts the inflated sessions),
DONGLE_HOST (default zero-dongle-a12c.local), DONGLE_TZ. What the tools answer
and why they exist is in docs/data-model.md.
"""
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import logdb  # noqa: E402

from mcp.server.mcpserver import MCPServer  # noqa: E402

DB_PATH = os.environ.get("DONGLE_DB", logdb.DEFAULT_DB)
LOGS = os.environ.get("DONGLE_LOGS", logdb.DEFAULT_LOGS)
HOST = os.environ.get("DONGLE_HOST", "zero-dongle-a12c.local")
TZ = os.environ.get("DONGLE_TZ", logdb.DEFAULT_TZ)

server = MCPServer(
    "zongle",
    instructions=(
        "The Zero SR/S console dongle's archive and live state. Times are the dongle's local stamps "
        "(ISO, %s); range arguments take an ISO prefix like 2026-09-10 or a relative -7d, -12h, -30m. "
        "Start with sessions() or reading_names() to see what exists; series() for a figure over time; "
        "events() for the bike's own narration; snapshot() for a command's full text; search() when you "
        "do not know where something is. live_* tools need the dongle on the network." % TZ
    ),
)


def db():
    return logdb.open_db(DB_PATH)


@server.tool()
def sessions(since: str | None = None, until: str | None = None, limit: int = 50) -> list[dict]:
    """Sessions (one per MBB wake, ride or charge) in a range, newest first, with the state changes each saw."""
    return logdb.sessions(db(), since, until, limit)


@server.tool()
def events(since: str | None = None, until: str | None = None, pattern: str | None = None,
           include_dongle: bool = True, limit: int = 200) -> list[dict]:
    """The bike's event log: the MBB's own stamped lines (state changes, contactors, LSS, faults, hibernate) and the dongle's markers, oldest first."""
    kinds = ("event", "dongle") if include_dongle else ("event",)
    return logdb.events(db(), since, until, pattern, kinds, limit)


@server.tool()
def reading_names(pattern: str | None = None) -> list[dict]:
    """Every figure the archive has parsed from the console tables: name, the command it comes from, sample count, first and last time, last value, unit when known."""
    return logdb.reading_names(db(), pattern)


@server.tool()
def series(name: str, since: str | None = None, until: str | None = None, command: str | None = None,
           step_s: int = 0, limit: int = 500) -> dict:
    """One reading over time. step_s averages into buckets of that many seconds; command disambiguates a name printed by more than one command."""
    return logdb.series(db(), name, since, until, command, step_s, limit)


@server.tool()
def snapshot(command: str, at: str | None = None, session: str | None = None) -> dict | None:
    """The full text of a console command's output (status, bms, in, ccm, faults, ...) nearest before a time, or the latest; session narrows to one session by id, name or file."""
    return logdb.snapshot(db(), command, at, session)


@server.tool()
def search(query: str, since: str | None = None, until: str | None = None, limit: int = 100) -> list[dict]:
    """Full-text search over every captured line, oldest first. A phrase match, falling back to a substring match."""
    return logdb.search(db(), query, since, until, limit)


@server.tool()
def session_lines(session: str, offset: int = 0, limit: int = 200, kinds: list[str] | None = None) -> dict:
    """A stretch of one session's transcript by id, session id or file name; kinds filters to event, dongle, mbb, echo, output or prompt."""
    return logdb.session_lines(db(), session, offset, limit, kinds)


def _get(path: str, timeout: float = 8.0) -> str:
    with urllib.request.urlopen("http://%s%s" % (HOST, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


@server.tool()
def live_status() -> dict:
    """The dongle's /api/status now: board, MBB awake, pack summary, sleep state, storage, time source."""
    return json.loads(_get("/api/status"))


@server.tool()
def live_readings() -> list[dict]:
    """The dongle's /api/readings now: the whitelisted figures with unit and age in seconds."""
    return json.loads(_get("/api/readings"))


@server.tool()
def live_command(name: str) -> str:
    """The last output of a polled command as the dongle holds it (status, charging, bms, pdu, in, faults, bms interface, controller, msc, dash info, ccm, obd, performance)."""
    return _get("/api/cmd/" + urllib.request.quote(name))


@server.tool()
def ingest(directory: str | None = None) -> dict:
    """Load pulled .log files not yet in the archive (default: the puller's directory), dropping sessions whose file is gone. Returns counts loaded, reloaded, unchanged, dropped."""
    return logdb.ingest(db(), [directory or LOGS], TZ)


if __name__ == "__main__":
    server.run("stdio")
