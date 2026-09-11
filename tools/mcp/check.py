#!/usr/bin/env python3
"""Smoke-test the MCP server the way a client uses it: launch run.sh over
stdio, list the tools, call a few. Exit 0 when every call answers.

    tools/mcp/check.py            # against logs/dongle.db
    DONGLE_DB=... tools/mcp/check.py
    DONGLE_LIVE=1 tools/mcp/check.py   # also the live_* tools, with the board reachable
"""
import asyncio
import json
import os
import sys

from mcp.client.session import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client

HERE = os.path.dirname(os.path.abspath(__file__))


def show(name, result):
    """Print a call's result and return it as data: the structured result when the server sent one, else the text."""
    if result.is_error:
        raise SystemExit("%s: %s" % (name, "".join(getattr(c, "text", "") for c in result.content)))
    data = result.structured_content
    if isinstance(data, dict) and set(data) == {"result"}:   # a list or scalar return is wrapped
        data = data["result"]
    if data is None:
        data = "".join(getattr(c, "text", "") for c in result.content)
    text = data if isinstance(data, str) else json.dumps(data)
    print("%-14s %s%s" % (name, text[:160].replace("\n", " "), "..." if len(text) > 160 else ""))
    return data


async def main():
    params = StdioServerParameters(command=os.path.join(HERE, "run.sh"), env=dict(os.environ))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            tools = await s.list_tools()
            names = sorted(t.name for t in tools.tools)
            print("tools:", ", ".join(names))
            want = {"sessions", "events", "reading_names", "series", "snapshot", "search", "session_lines",
                    "live_status", "live_readings", "live_command", "ingest"}
            missing = want - set(names)
            if missing:
                raise SystemExit("missing tools: %s" % sorted(missing))
            sessions = show("sessions", await s.call_tool("sessions", {"limit": 3}))
            show("reading_names", await s.call_tool("reading_names", {"pattern": "12V"}))
            show("series", await s.call_tool("series", {"name": "State_of_Charge", "step_s": 3600}))
            show("events", await s.call_tool("events", {"pattern": "State change", "limit": 5}))
            show("search", await s.call_tool("search", {"query": "Hibernating", "limit": 2}))
            show("snapshot", await s.call_tool("snapshot", {"command": "bms"}))
            if sessions:
                show("session_lines", await s.call_tool("session_lines", {"session": str(sessions[0]["id"]), "limit": 3, "kinds": ["event", "dongle"]}))
            if os.environ.get("DONGLE_LIVE"):   # needs the board on the network
                show("live_status", await s.call_tool("live_status", {}))
                show("live_readings", await s.call_tool("live_readings", {}))
                show("live_command", await s.call_tool("live_command", {"name": "bms"}))
    print("check OK")


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
