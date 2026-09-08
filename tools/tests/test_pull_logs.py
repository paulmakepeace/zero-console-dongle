"""pull-logs.py against a fake dongle served in-process: python3 -m pytest tools/tests"""
import http.server
import importlib.machinery
import importlib.util
import json
import os
import threading
import urllib.request
import zlib

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_loader("pull_logs", importlib.machinery.SourceFileLoader(
    "pull_logs", os.path.join(HERE, "..", "pull-logs.py")))
pull_logs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pull_logs)


class FakeDongle:
    """The HTTP contract the puller relies on, with knobs for its failure modes."""

    def __init__(self):
        self.files = {}           # name -> bytes
        self.active = ""
        self.deleted = []
        self.status_code = 200
        self.name = "zero-dongle-test"
        self.short_body = set()   # names whose body is cut short
        self.gone = set()         # names that vanish between listing and fetch
        self.became_active = set()
        self.delete_fails = set()
        self.status = {"fs": {"ok": True, "formats": 0}, "dropped_lines": 0, "uart": {}}
        dongle = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def send(self, code, body, length=None):
                self.send_response(code)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(length if length is not None else len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/api/status":
                    if dongle.status_code != 200:
                        return self.send(dongle.status_code, b"no")
                    return self.send(200, json.dumps(dict(dongle.status, name=dongle.name)).encode())
                if self.path == "/logs":
                    listing = [{"name": n, "size": len(b), "active": n == dongle.active} for n, b in dongle.files.items()]
                    return self.send(200, json.dumps(listing).encode())
                if self.path.startswith("/logs/"):
                    n = self.path[6:]
                    if n in dongle.gone or n not in dongle.files:
                        return self.send(404, b"no such file")
                    if n == dongle.active or n in dongle.became_active:
                        return self.send(409, b"file is active; see /live")
                    body = dongle.files[n]
                    if n in dongle.short_body:
                        self.send(200, body[: len(body) // 2], length=len(body))
                        return self.wfile.flush()
                    return self.send(200, body)
                self.send(404, b"not found")

            def do_DELETE(self):
                if self.headers.get("X-Dongle") != "1":
                    return self.send(403, b"missing X-Dongle: 1 header")
                n = self.path[6:]
                if n in dongle.delete_fails:
                    return self.send(500, b"flash error")
                if n not in dongle.files:
                    return self.send(404, b"no such file")
                del dongle.files[n]
                dongle.deleted.append(n)
                self.send(200, b"deleted")

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.host = "127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()


@pytest.fixture
def dongle():
    d = FakeDongle()
    d.files = {"b0001-001-20260907-120000.log": b"one\n" * 100, "b0001-002-20260907-130000.log": b"two\n" * 50,
               "b0001-003-20260907-140000.log": b"active\n"}
    d.active = "b0001-003-20260907-140000.log"
    yield d
    d.close()


def run(dongle, tmp_path, *extra):
    """Run the puller in-process; returns (exit status, stdout, stderr)."""
    import sys, io
    argv = ["pull-logs.py", "--host", dongle.host, "--dest", str(tmp_path / "out"), *extra]
    out, err = io.StringIO(), io.StringIO()
    old = sys.argv, sys.stdout, sys.stderr
    sys.argv, sys.stdout, sys.stderr = argv, out, err
    try:
        pull_logs.main()
        code = 0
    except SystemExit as e:
        if isinstance(e.code, int):
            code = e.code
        else:   # sys.exit("message"): the interpreter would print it to stderr on the way out
            code = 1
            err.write(str(e.code) + "\n")
    finally:
        sys.argv, sys.stdout, sys.stderr = old
    return code, out.getvalue(), err.getvalue()


def test_moves_files_and_skips_the_active_one(dongle, tmp_path):
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    got = sorted(os.listdir(tmp_path / "out"))
    assert got == [".pull-lock", "b0001-001-20260907-120000.log", "b0001-002-20260907-130000.log"]
    assert (tmp_path / "out" / "b0001-001-20260907-120000.log").read_bytes() == b"one\n" * 100
    assert dongle.deleted == ["b0001-001-20260907-120000.log", "b0001-002-20260907-130000.log"]
    assert "skip  b0001-003-20260907-140000.log (active)" in out
    assert dongle.active in dongle.files


def test_keep_downloads_without_deleting(dongle, tmp_path):
    code, out, _ = run(dongle, tmp_path, "--keep")
    assert code == 0 and dongle.deleted == [] and "saved" in out


def test_short_body_is_a_failure_and_nothing_is_deleted(dongle, tmp_path):
    dongle.short_body.add("b0001-001-20260907-120000.log")
    code, out, err = run(dongle, tmp_path)
    assert code == 1
    assert "FAIL  b0001-001-20260907-120000.log" in err
    assert "b0001-001-20260907-120000.log" not in dongle.deleted
    assert "b0001-002-20260907-130000.log" in dongle.deleted     # the rest of the batch still ran
    assert not any(n.startswith("b0001-001") for n in os.listdir(tmp_path / "out"))


def test_file_that_became_active_is_a_skip_and_a_vanished_one_is_lost(dongle, tmp_path):
    dongle.became_active.add("b0001-001-20260907-120000.log")
    dongle.gone.add("b0001-002-20260907-130000.log")
    code, out, err = run(dongle, tmp_path)
    assert "skip  b0001-001-20260907-120000.log (became active)" in out
    assert "LOST  b0001-002-20260907-130000.log" in err
    assert code == 1 and dongle.deleted == []


def test_delete_failure_keeps_the_local_copy_and_exits_nonzero(dongle, tmp_path):
    dongle.delete_fails.add("b0001-001-20260907-120000.log")
    code, out, err = run(dongle, tmp_path)
    assert code == 1
    assert "delete failed: 500 flash error" in err     # the dongle's own reason text
    assert (tmp_path / "out" / "b0001-001-20260907-120000.log").exists()


def test_identical_existing_file_is_not_rewritten_and_a_different_one_gets_a_suffix(dongle, tmp_path):
    out = tmp_path / "out"
    out.mkdir()
    same = out / "b0001-001-20260907-120000.log"
    same.write_bytes(b"one\n" * 100)
    before = same.stat().st_mtime_ns
    different = out / "b0001-002-20260907-130000.log"
    different.write_bytes(b"older content\n")
    code, _, _ = run(dongle, tmp_path)
    assert code == 0
    assert same.stat().st_mtime_ns == before
    assert (out / "b0001-002-20260907-130000-2.log").read_bytes() == b"two\n" * 50
    assert different.read_bytes() == b"older content\n"


def test_unreadable_status_stops_before_listing(dongle, tmp_path):
    dongle.status_code = 500
    code, out, err = run(dongle, tmp_path)
    assert code != 0 and "status" in err and dongle.deleted == []


def test_bad_names_are_rejected_without_a_request(dongle, tmp_path):
    dongle.files["../etc/passwd"] = b"x"
    dongle.files[".hidden.log"] = b"x"
    code, out, err = run(dongle, tmp_path)
    assert code == 1
    assert "name rejected" in err
    assert "../etc/passwd" not in dongle.deleted and ".hidden.log" in dongle.files


def test_status_warnings_are_printed(dongle, tmp_path):
    dongle.status = {"fs": {"ok": False, "formats": 2}, "dropped_lines": 7, "uart": {"overflows": 3}}
    code, out, err = run(dongle, tmp_path, "--keep")
    assert "no working filesystem" in err and "formatted" in err and "7 line(s) dropped" in err and "overruns 3" in err


def test_second_concurrent_run_exits_at_once(dongle, tmp_path):
    import fcntl
    out = tmp_path / "out"
    out.mkdir()
    lock = open(out / ".pull-lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    code, _, err = run(dongle, tmp_path)
    assert code == 0 and "another pull is running" in err and dongle.deleted == []
    lock.close()


def test_default_dest_is_one_directory_per_board(dongle, tmp_path, monkeypatch):
    import sys, io
    monkeypatch.setattr(pull_logs.os.path, "abspath", lambda p: str(tmp_path / "tools" / "pull-logs.py"))
    monkeypatch.setattr(sys, "argv", ["pull-logs.py", "--host", dongle.host, "--keep"])
    monkeypatch.setattr(sys, "stdout", io.StringIO())
    monkeypatch.setattr(sys, "stderr", io.StringIO())
    with pytest.raises(SystemExit) as e:
        pull_logs.main()
    assert e.value.code == 0
    assert (tmp_path / "logs" / "dongle" / "zero-dongle-test" / "b0001-001-20260907-120000.log").exists()


def gz(payload, complete=True):
    """A gzip stream as the dongle writes it: a sync flush per commit, the trailer only at close."""
    c = zlib.compressobj(6, zlib.DEFLATED, 31)
    body = c.compress(payload) + c.flush(zlib.Z_SYNC_FLUSH)
    return body + c.flush() if complete else body


def test_gzip_file_is_inflated_and_stored_plain(dongle, tmp_path):
    dongle.files = {"b0002-001-20260908-000100.log.gz": gz(b"first\nsecond\n")}
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0002-001-20260908-000100.log").read_bytes() == b"first\nsecond\n"
    assert not (tmp_path / "out" / "b0002-001-20260908-000100.log.gz").exists()
    assert dongle.deleted == ["b0002-001-20260908-000100.log.gz"]


def test_truncated_gzip_is_kept_as_far_as_it_decodes(dongle, tmp_path):
    dongle.files = {"b0002-001-20260908-000100.log.gz": gz(b"one\ntwo\nthree\n", complete=False)}
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0002-001-20260908-000100.log").read_bytes() == b"one\ntwo\nthree\n"
    assert "truncated; 3 line(s) recovered" in out
    assert dongle.deleted == ["b0002-001-20260908-000100.log.gz"]


def test_damaged_gzip_is_salvaged_with_the_raw_kept(dongle, tmp_path):
    good = gz(b"one\ntwo\n", complete=False)
    dongle.files = {"b0002-001-20260908-000100.log.gz": good + b"\xff\xfe\xfd\xfc" * 200}
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0002-001-20260908-000100.log").read_bytes() == b"one\ntwo\n"
    assert (tmp_path / "out" / "b0002-001-20260908-000100.log.gz").exists()
    assert "8 byte(s) decoded, then the stream fails" in err
    assert dongle.deleted == ["b0002-001-20260908-000100.log.gz"]


def test_bytes_after_the_trailer_are_reported(dongle, tmp_path):
    dongle.files = {"b0002-001-20260908-000100.log.gz": gz(b"one\n") + b"junk"}
    code, out, err = run(dongle, tmp_path)
    assert code == 0 and "bytes after the gzip trailer" in err
    assert (tmp_path / "out" / "b0002-001-20260908-000100.log").read_bytes() == b"one\n"


def test_damaged_raw_follows_the_suffix_of_its_log(dongle, tmp_path):
    out = tmp_path / "out"
    out.mkdir()
    (out / "b0002-001-20260908-000100.log").write_bytes(b"different earlier content\n")
    dongle.files = {"b0002-001-20260908-000100.log.gz": gz(b"one\n", complete=False) + b"\xff" * 50}
    code, _, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (out / "b0002-001-20260908-000100-2.log").read_bytes() == b"one\n"
    assert (out / "b0002-001-20260908-000100-2.log.gz").exists()
    assert not (out / "b0002-001-20260908-000100.log.gz").exists()


def test_damaged_zlib_keeps_its_raw_with_the_zlib_suffix(dongle, tmp_path):
    dongle.files = {"b0004-001-20260908-000100.log.z": z(b"one\n", complete=False) + b"\xff" * 50}
    code, _, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0004-001-20260908-000100.log.z").exists()
    assert (tmp_path / "out" / "b0004-001-20260908-000100.log").read_bytes() == b"one\n"


DICT = b"******************************************************************\n*              Zero Motorcycles MBB                         *\nReset Source: Hib Wake RTC, Power-On, Supply WD, Power Val\n"


def z(payload, zdict=None, complete=True):
    """A zlib stream as the dongle writes it: FDICT naming the dictionary, a sync flush per commit."""
    c = zlib.compressobj(6, zlib.DEFLATED, 15, zdict=zdict) if zdict else zlib.compressobj(6, zlib.DEFLATED, 15)
    body = c.compress(payload) + c.flush(zlib.Z_SYNC_FLUSH)
    return body + c.flush() if complete else body


def test_zlib_file_with_a_dictionary_fetches_it_once_and_caches_it(dongle, tmp_path):
    did = zlib.adler32(DICT) & 0xFFFFFFFF
    dongle.files = {"dict-%08x.txt" % did: DICT,
                    "b0003-001-20260908-110000.log.z": z(b"line one\n" + DICT, DICT),
                    "b0003-002-20260908-120000.log.z": z(b"line two\n", DICT)}
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0003-001-20260908-110000.log").read_bytes() == b"line one\n" + DICT
    assert (tmp_path / "out" / "b0003-002-20260908-120000.log").read_bytes() == b"line two\n"
    assert (tmp_path / "out" / "dicts" / ("%08x.txt" % did)).read_bytes() == DICT
    assert "dict-%08x.txt" % did in dongle.files           # never deleted by the puller
    assert sorted(dongle.deleted) == ["b0003-001-20260908-110000.log.z", "b0003-002-20260908-120000.log.z"]
    # a second run with a new file uses the cache: remove the dongle's copy and it still works
    del dongle.files["dict-%08x.txt" % did]
    dongle.files["b0003-003-20260908-130000.log.z"] = z(b"line three\n", DICT)
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0003-003-20260908-130000.log").read_bytes() == b"line three\n"


def test_zlib_file_whose_dictionary_is_missing_is_a_failure_and_kept(dongle, tmp_path):
    dongle.files = {"b0003-001-20260908-110000.log.z": z(b"line one\n", DICT)}
    code, out, err = run(dongle, tmp_path)
    assert code == 1 and "needs dictionary" in err and dongle.deleted == []
    assert (tmp_path / "out" / "b0003-001-20260908-110000.log.z").read_bytes() == z(b"line one\n", DICT)   # the raw is kept locally too


def test_zlib_file_without_a_dictionary_and_a_truncated_one(dongle, tmp_path):
    dongle.files = {"b0003-001-20260908-110000.log.z": z(b"a\nb\n"),
                    "b0003-002-20260908-120000.log.z": z(b"c\nd\ne\n", complete=False)}
    code, out, err = run(dongle, tmp_path)
    assert code == 0, err
    assert (tmp_path / "out" / "b0003-001-20260908-110000.log").read_bytes() == b"a\nb\n"
    assert (tmp_path / "out" / "b0003-002-20260908-120000.log").read_bytes() == b"c\nd\ne\n"
    assert "truncated; 3 line(s) recovered" in out
