"""The archive service against a temporary archive and database: the push
route's outcomes, the dated layout, the ingest, and the MCP endpoint."""
import importlib
import json
import os
import sqlite3
import sys
import zlib

import pytest
from starlette.testclient import TestClient

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DICT = b"ZERO MBB> \nstatus\n            State_of_Charge,         54,          %,      Yes,         0\n"
DICT_ID = zlib.adler32(DICT) & 0xFFFFFFFF
BOARD = "zero-dongle-test"
LOG = ("2026-09-11T00:00:00.000 dongle: session start, %s, id b0001-001, part 1, boot 1 (power-on), time wall, fw 0.12.0\n"
       "2026-09-11T00:00:01.000 09/11/2026 00:00:01.000 - State change from STRT to PWSU\n"
       "2026-09-11T00:00:02.000 dongle: poll\n"
       "2026-09-11T00:00:02.100 ZERO MBB> status\n"
       "2026-09-11T00:00:02.200             State_of_Charge,         54,          %%,      Yes,         0\n"
       "2026-09-11T00:00:02.300 ZERO MBB> \n"
       "2026-09-11T00:00:03.000 dongle: session end\n") % BOARD
NAME = "b0001-001-20260911-000000-%08x.log.z" % DICT_ID


def compress(text, zdict):
    c = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_DEFAULT_STRATEGY, zdict)
    return c.compress(text.encode()) + c.flush()


@pytest.fixture
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("DONGLE_ARCHIVE", str(tmp_path / "archive"))
    monkeypatch.setenv("DONGLE_DB", str(tmp_path / "dongle.db"))
    sys.path.insert(0, os.path.join(ROOT, "server"))
    import app as mod
    mod = importlib.reload(mod)
    with TestClient(mod.app) as c:
        c.archive = tmp_path / "archive"
        c.db = tmp_path / "dongle.db"
        yield c


def put(client, name, body):
    return client.put("/push/%s/%s" % (BOARD, name), content=body)


def test_session_before_its_dictionary_is_refused_then_accepted(client):
    body = compress(LOG, DICT)
    r = put(client, NAME, body)
    assert r.status_code == 409 and r.text == "need dict-%08x.txt" % DICT_ID
    assert (client.archive / BOARD / "202609" / "11" / NAME).read_bytes() == body   # the raw is kept even so
    assert json.loads(client.get("/health").text)["sessions"] == 0
    r = put(client, "dict-%08x.txt" % DICT_ID, DICT)
    assert r.status_code == 200 and r.text == "stored"
    assert (client.archive / BOARD / "dicts" / ("%08x.txt" % DICT_ID)).read_bytes() == DICT
    r = put(client, NAME, body)   # raw already there from the 409, now ingested that the dictionary has come
    assert r.status_code == 200 and "loaded 1" in r.text
    assert (client.archive / BOARD / "202609" / "11" / NAME).read_bytes() == body
    db = sqlite3.connect(str(client.db))
    rows = db.execute("SELECT file, board, session_id, lines, events FROM sessions").fetchall()
    assert rows == [(NAME[:-2], BOARD, "b0001-001", 7, 4)]
    assert db.execute("SELECT name, value FROM readings").fetchall() == [("State_of_Charge", 54.0)]
    assert db.execute("SELECT count(*) FROM lines_fts WHERE lines_fts MATCH 'PWSU'").fetchone()[0] == 1


def test_the_same_file_again_is_unchanged_and_a_different_one_gets_a_suffix(client):
    put(client, "dict-%08x.txt" % DICT_ID, DICT)
    body = compress(LOG, DICT)
    assert put(client, NAME, body).text.startswith("stored")
    assert put(client, NAME, body).text.startswith("unchanged")
    other = compress(LOG.replace("54", "55"), DICT)
    assert put(client, NAME, other).text.startswith("stored")
    day = client.archive / BOARD / "202609" / "11"
    assert {p.name for p in day.iterdir()} == {NAME, NAME[:-6] + "-2.log.z"}
    assert put(client, "dict-%08x.txt" % DICT_ID, DICT).text == "unchanged"


def test_a_plain_log_and_a_gzip_are_taken_too(client):
    r = put(client, "b0002-001-20260911-010000.log", LOG.encode())
    assert r.status_code == 200 and "loaded 1" in r.text
    import gzip
    r = put(client, "b0003-001-20260911-020000.log.gz", gzip.compress(LOG.encode()))
    assert r.status_code == 200 and "loaded 1" in r.text
    assert json.loads(client.get("/health").text)["sessions"] == 2


def test_bad_names_and_bad_dictionaries_are_refused(client):
    assert put(client, "..%2fetc", b"x").status_code in (400, 404)
    assert client.put("/push/%s/notes.txt" % BOARD, content=b"x").status_code == 400
    assert put(client, "dict-deadbeef.txt", DICT).status_code == 400
    assert client.put("/push/bad%20board/b0001-001-20260911-000000.log", content=b"x").status_code in (400, 404)


def test_mcp_endpoint_answers_initialize(client):
    req = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
           "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}}}
    r = client.post("/mcp", json=req, headers={"Accept": "application/json, text/event-stream"})
    assert r.status_code == 200, r.text
    assert "zongle" in r.text
