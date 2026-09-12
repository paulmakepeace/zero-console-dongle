"""The dongle's session files as bytes: what a name may look like, which
dictionary a zlib stream names, how to inflate one that may be cut short, and
where a file lives in the dated archive. Standard library only; the puller,
the ingest and the archive service all import it.
"""
import os
import re
import zlib

NAME_OK = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")   # what the firmware accepts, see firmware/src/pure/names.h
SESSION = re.compile(r"^b\d{4}-\d{2,3}-(\d{8})-\d{6}(?:-[0-9a-f]{8})?\.log(?:\.z|\.gz)?\Z")
DICT = re.compile(r"^dict-([0-9a-f]{8})\.txt\Z")
SESSION_EXT = (".log", ".log.z", ".log.gz")


def name_ok(name):
    return isinstance(name, str) and bool(NAME_OK.match(name)) and ".." not in name


def dictionary_id(data):
    """The dictionary a zlib stream names in its header, or None."""
    if len(data) >= 6 and data[0] == 0x78 and data[1] & 0x20:
        return int.from_bytes(data[2:6], "big")
    return None


def dict_matches(data, did):
    """Whether these bytes are the dictionary with this id."""
    return zlib.adler32(data) & 0xFFFFFFFF == did


def dict_id_of_name(name):
    """The id a dict-XXXXXXXX.txt name carries, or None."""
    m = DICT.match(name)
    return int(m.group(1), 16) if m else None


def inflate(data, zdict=None):
    """A gzip or zlib stream to its bytes. Returns (bytes, complete, note): a
    stream cut off before its trailer still yields everything up to the last
    flush; a stream that fails part-way (a flash bit error) yields what
    decoded before the error, with a note saying so."""
    def fresh():
        if data[:2] == b"\x1f\x8b":
            return zlib.decompressobj(31)
        return zlib.decompressobj(15, zdict=zdict) if zdict is not None else zlib.decompressobj(15)
    d = fresh()
    try:
        out = d.decompress(data)   # one pass, the case every good file is
        return out, d.eof, "bytes after the gzip trailer" if d.unused_data else ""
    except zlib.error:
        pass
    d = fresh()
    out = b""
    for k in range(0, len(data), 512):   # in pieces, so a late error keeps the early bytes
        before = d.copy()
        try:
            out += d.decompress(data[k:k + 512])
        except zlib.error as exc:
            d = before   # back to the last good state, then byte by byte up to the error
            for b in range(k, min(k + 512, len(data))):
                try:
                    out += d.decompress(data[b:b + 1])
                except zlib.error:
                    break
            if "data check" in str(exc):
                return out, False, "%d byte(s) decoded but the trailer's check failed, so the content may be wrong anywhere: %s" % (len(out), exc)
            return out, False, "%d byte(s) decoded, then the stream fails: %s" % (len(out), exc)
    note = "bytes after the gzip trailer" if d.unused_data else ""
    return out, d.eof, note


def plain_name(name):
    """The .log name a compressed file inflates to."""
    if name.endswith(".gz"):
        return name[:-3]
    if name.endswith(".z"):
        return name[:-2]
    return name


def find_dicts_dir(path):
    """The dicts/ directory that serves a file: beside it or in an ancestor, or None."""
    d = os.path.dirname(os.path.abspath(path))
    while True:
        cand = os.path.join(d, "dicts")
        if os.path.isdir(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def load_dict(dicts_dir, did):
    """The dictionary with this id from a dicts/ directory, checked against its id; None if absent."""
    if not dicts_dir:
        return None
    path = os.path.join(dicts_dir, "%08x.txt" % did)
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        d = f.read()
    return d if dict_matches(d, did) else None


def inflate_file(path):
    """A session file on disk to (text bytes, complete, note), plain or compressed, its dictionary found beside it."""
    with open(path, "rb") as f:
        data = f.read()
    if not (path.endswith(".z") or path.endswith(".gz")):
        return data, True, ""
    did = dictionary_id(data)
    zdict = load_dict(find_dicts_dir(path), did) if did is not None else None
    if did is not None and zdict is None:
        return b"", False, "needs dictionary %08x" % did
    return inflate(data, zdict)


def archive_relpath(name):
    """YYYYMM/DD/name from the date in a session name; an undated name goes under 'undated'."""
    m = SESSION.match(name)
    if not m:
        return os.path.join("undated", name)
    ymd = m.group(1)
    return os.path.join(ymd[:6], ymd[6:8], name)


def write_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = "%s.part.%d" % (path, os.getpid())
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
