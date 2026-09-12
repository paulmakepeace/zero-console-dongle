#!/bin/sh
# The server's venv, made on first use with the mcp SDK and uvicorn; prints its python.
set -e
here=$(cd "$(dirname "$0")" && pwd)
venv="$here/.venv"
if [ ! -x "$venv/bin/python" ]; then
    py=""
    for c in python3.13 python3.12 python3.11 python3.10; do
        if command -v "$c" >/dev/null 2>&1; then py=$c; break; fi
    done
    [ -n "$py" ] || { echo "venv.sh: the mcp SDK needs Python 3.10 or later" >&2; exit 1; }
    "$py" -m venv "$venv" >&2
    "$venv/bin/pip" install -q -r "$here/../../server/requirements.txt" >&2
fi
echo "$venv/bin/python"
