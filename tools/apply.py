#!/usr/bin/env python3
"""Apply exact text replacements to source files, all or nothing, loudly.

Usage: tools/apply.py [SPEC.json]        (or the spec on stdin)
       tools/apply.py --check SPEC.json  (say what would change, write nothing)

The spec is JSON, a list of files each with a list of edits:

  [{"file": "firmware/src/poller.cpp",
    "edits": [{"old": "exact text", "new": "replacement"},
              {"old": "...", "new": "...", "count": 2}]}]

Every `old` must appear exactly once in its file, or exactly `count` times if
given. A miss, or an unexpected number of matches, aborts the whole run before
anything is written and names what did not match. Nothing is ever half applied.

Why this exists: hundreds of one-off python heredocs did this job during
2026-09, each re-implementing the same all-or-nothing helper, and each
printing its failure into a scrollback rather than into an exit code. Twice a
fix was reported as applied when the edit had silently not landed. This exits
non-zero and prints a diff, so the claim and the change cannot diverge.

It does not replace reading the file. A replacement is decided from the whole
file, not from the window that located it: a 17-line window is what put a
flash commit inside a directory walk on 2026-09-09.
"""
import difflib
import io
import json
import os
import sys


def load(path):
    text = sys.stdin.read() if path is None else io.open(path, encoding="utf-8").read()
    try:
        spec = json.loads(text)
    except json.JSONDecodeError as e:
        sys.exit("apply: the spec is not JSON: %s" % e)
    if isinstance(spec, dict):
        spec = [spec]
    return spec


def main():
    args = [a for a in sys.argv[1:]]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    spec = load(args[0] if args else None)

    planned, problems = [], []
    for entry in spec:
        path = entry.get("file")
        if not path:
            problems.append("an entry has no file")
            continue
        if not os.path.exists(path):
            problems.append("%s: no such file" % path)
            continue
        before = io.open(path, encoding="utf-8").read()
        after = before
        for i, e in enumerate(entry.get("edits", []), 1):
            old, new, want = e.get("old"), e.get("new"), e.get("count", 1)
            if old is None or new is None:
                problems.append("%s edit %d: needs both old and new" % (path, i))
                continue
            seen = after.count(old)
            if seen != want:
                head = " ".join(old.split())[:70]
                problems.append("%s edit %d: found %d, wanted %d: %s" % (path, i, seen, want, head))
                continue
            after = after.replace(old, new, want)
        if after != before:
            planned.append((path, before, after))

    if problems:
        for p in problems:
            print("apply: " + p, file=sys.stderr)
        sys.exit("apply: %d problem(s); nothing written" % len(problems))

    if not planned:
        sys.exit("apply: every replacement is already in place; nothing to do")

    for path, before, after in planned:
        diff = difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                    "a/" + path, "b/" + path)
        sys.stdout.writelines(diff)
    if check:
        print("apply: --check, nothing written")
        return
    for path, _, after in planned:
        io.open(path, "w", encoding="utf-8").write(after)
    print("apply: %d file(s) written" % len(planned))


if __name__ == "__main__":
    main()
