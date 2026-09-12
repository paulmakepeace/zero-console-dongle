#!/bin/sh
# Start the MCP server over stdio, making its venv on first run. Everything
# but the protocol goes to stderr: stdout is the MCP channel.
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec "$("$here/venv.sh")" "$here/server.py" "$@"
