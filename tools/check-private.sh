#!/bin/sh
# Refuse a commit that would put a private identifier into a tracked file.
#
# Patterns live in .private-patterns at the repo root, one extended regex per
# line, and that file is git-ignored because the identifiers themselves are
# what must not be tracked. With no arguments the staged files are checked,
# which is the pre-commit hook's job:
#
#   ln -sf ../../tools/check-private.sh .git/hooks/pre-commit
#
# With file arguments those files are checked whole. Exit 1 on a hit, 2 when
# the patterns file is missing, since a gate with nothing to look for would
# pass everything.
set -u
repo=$(git rev-parse --show-toplevel 2>/dev/null) || repo=$(cd "$(dirname "$0")/.." && pwd)   # $0 is the hook symlink when run by git
patterns="$repo/.private-patterns"
[ -s "$patterns" ] || { echo "check-private: no patterns at $patterns; nothing is being checked" >&2; exit 2; }

status=0
if [ $# -gt 0 ]; then
    for f in "$@"; do
        LC_ALL=C grep -nHaEf "$patterns" -- "$f" && status=1
    done
else
    git -C "$repo" diff --cached --name-only --diff-filter=ACMR -z | while IFS= read -r -d '' f; do
        git -C "$repo" show ":$f" | LC_ALL=C grep -naEf "$patterns" | sed "s|^|$f:|"
    done | grep . && status=1
fi
[ $status = 0 ] || echo "check-private: a private identifier is in the text above; remove it before committing" >&2
exit $status
