"""The archive service: the dongle's push endpoint, the ingest and the MCP
server, in one process on the NAS.

    PUT /push/<board>/<name>   a session file or a dictionary, as the flash holds it
    GET /health                the archive and database in one line of JSON
    /mcp                       the MCP endpoint, streamable HTTP

A session file lands in the dated archive first (DONGLE_ARCHIVE/<board>/YYYYMM/DD/),
then the ingest loads it into the database; the raw file is the truth and the
database is rebuilt from it with `logdb.py ingest --force`. A dictionary goes
under <board>/dicts/ by its id. Environment: DONGLE_ARCHIVE, DONGLE_DB,
DONGLE_TZ, DONGLE_HOST (the live tools), PUSH_MAX_BYTES.
"""
import importlib.util
import json
import logging
import os
import sys
import threading

from starlette.applications import Starlette
from starlette.concurrency import run_in_threadpool
from starlette.responses import JSONResponse, PlainTextResponse
from starlette.routing import Mount, Route
from mcp.server.transport_security import TransportSecuritySettings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import logdb  # noqa: E402
import zlog  # noqa: E402

ARCHIVE = os.environ.get("DONGLE_ARCHIVE", os.path.join(ROOT, "logs", "archive"))
DB_PATH = os.environ.get("DONGLE_DB", logdb.DEFAULT_DB)
TZ = os.environ.get("DONGLE_TZ", logdb.DEFAULT_TZ)
MAX_BYTES = int(os.environ.get("PUSH_MAX_BYTES", 4 * 1024 * 1024))

log = logging.getLogger("zongle")
ingest_lock = threading.Lock()   # one ingest at a time; the database has one writer


def _mcp():
    """The MCP server from tools/mcp/server.py, loaded by path: its module name is taken."""
    spec = importlib.util.spec_from_file_location("zongle_mcp", os.path.join(ROOT, "tools", "mcp", "server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.server


def _same(path, data):
    if not os.path.isfile(path) or os.path.getsize(path) != len(data):
        return False
    with open(path, "rb") as f:
        return f.read() == data


def store(path, data):
    """Write data at path unless it is already there. A different file of the same name is kept beside it with a
    -2 suffix as evidence and is not ingested: a session name is unique, so this is damage, not a second session.
    Returns (outcome, path): stored, unchanged or variant."""
    if _same(path, data):
        return "unchanged", path
    if not os.path.exists(path):
        zlog.write_atomic(path, data)
        return "stored", path
    stem, ext = path, ""
    for e in (".log.z", ".log.gz", ".log", ".txt"):
        if path.endswith(e):
            stem, ext = path[:-len(e)], e
            break
    n = 2
    while True:
        cand = "%s-%d%s" % (stem, n, ext)
        if not os.path.exists(cand):
            zlog.write_atomic(cand, data)
            log.warning("%s differs from the file stored under that name; kept as %s, not ingested", path, cand)
            return "variant", cand
        if _same(cand, data):
            return "variant", cand   # seen before, and still not the session under that name
        n += 1


def ingest_path(path):
    with ingest_lock:
        db = logdb.open_db(DB_PATH)
        try:
            return logdb.ingest(db, [path], TZ)
        finally:
            db.close()


async def push(request):
    board = request.path_params["board"]
    name = request.path_params["name"]
    if not zlog.name_ok(board) or not zlog.name_ok(name):
        return PlainTextResponse("bad name", 400)
    body = await request.body()
    if len(body) > MAX_BYTES:
        return PlainTextResponse("too large", 413)
    did = zlog.dict_id_of_name(name)
    if did is not None:
        if not zlog.dict_matches(body, did):
            return PlainTextResponse("dictionary does not match its id", 400)
        outcome, _ = await run_in_threadpool(store, os.path.join(ARCHIVE, board, "dicts", "%08x.txt" % did), body)
        return PlainTextResponse(outcome, 200)
    if not name.endswith(zlog.SESSION_EXT):
        return PlainTextResponse("not a session file", 400)
    path = os.path.join(ARCHIVE, board, zlog.archive_relpath(name))
    outcome, path = await run_in_threadpool(store, path, body)   # the raw bytes first, whatever else happens
    if outcome == "variant":
        return PlainTextResponse(outcome, 200)
    if name.endswith(".z"):
        needed = zlog.dictionary_id(body)
        if needed is not None and zlog.load_dict(os.path.join(ARCHIVE, board, "dicts"), needed) is None:
            return PlainTextResponse("need dict-%08x.txt" % needed, 409)   # kept as raw; ingested once the dictionary arrives
    try:
        counts = await run_in_threadpool(ingest_path, path)
    except Exception as exc:   # the raw file is safe; the ingest can be rerun
        log.exception("ingest of %s failed", path)
        return PlainTextResponse("%s; ingest failed: %s" % (outcome, exc), 200)
    log.info("push %s/%s: %s, %s", board, name, outcome, json.dumps(counts))
    return PlainTextResponse("%s; %s" % (outcome, ", ".join("%s %d" % kv for kv in counts.items() if kv[1])), 200)


async def health(request):
    def count():
        db = logdb.open_db(DB_PATH)
        try:
            return db.execute("SELECT count(*) FROM sessions").fetchone()[0]
        finally:
            db.close()
    n = await run_in_threadpool(count)
    return JSONResponse({"ok": True, "sessions": n, "archive": ARCHIVE, "db": DB_PATH})


mcp_app = _mcp().streamable_http_app(
    transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=False))   # on the LAN, by IP or NAS name
app = Starlette(
    routes=[
        Route("/health", health),
        Route("/push/{board}/{name}", push, methods=["PUT", "POST"]),
        Mount("/", app=mcp_app),
    ],
    lifespan=mcp_app.router.lifespan_context,
)
