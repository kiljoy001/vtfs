#!/usr/bin/env python3
"""
vtfs MCP server.

Exposes a persistent 9front terminal as MCP tools. The terminal lives on
dev9p (or whatever VTFS_ADDR points at); this process is only a 9P client.
"""

import os
import sys
import time
import logging

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p9client import P9
from mcp.server.fastmcp import FastMCP

logging.basicConfig(level=logging.INFO, stream=sys.stderr)
log = logging.getLogger("vtfs-mcp")

mcp = FastMCP("vtfs")
DEFAULT_ADDR = os.environ.get("VTFS_ADDR", "dev9p:5640")


def _addr():
    a = DEFAULT_ADDR
    if a.startswith("tcp!"):
        a = a[4:]
    host, _, port = a.partition(":")
    return (host or "dev9p"), int(port or 5640)


def _conn():
    host, port = _addr()
    last = None
    for _ in range(3):
        try:
            return P9(host=host, port=port)
        except Exception as e:  # connection refused, EOF, protocol error
            last = e
            time.sleep(0.2)
    raise RuntimeError(f"cannot connect to vtfs at {host}:{port}: {last}")


def _status(p, session):
    raw = p.readfile(f"/{session}/status").decode("utf-8", "replace")
    d = {}
    for line in raw.strip().splitlines():
        if " " in line:
            k, v = line.split(" ", 1)
            d[k] = v
    return d


def _screen(p, session):
    return p.readfile(f"/{session}/screen").decode("utf-8", "replace")


def _wait(p, session, quiet_ms=150, timeout=15.0, last_seq=None):
    """Poll status until seq is stable and idle >= quiet_ms, or timeout."""
    deadline = time.time() + timeout
    timed_out = False
    st = {}
    while time.time() < deadline:
        st = _status(p, session)
        seq = st.get("seq")
        try:
            idle = int(st.get("idle", "-1"))
        except ValueError:
            idle = -1
        if st.get("state") == "dead":
            return st, False
        if seq == last_seq and idle >= quiet_ms:
            return st, False
        last_seq = seq
        time.sleep(0.05)
    return _status(p, session), True


def _fmt(session, st, screen, timed_out=False, note=""):
    flag = " TIMEOUT" if timed_out else ""
    head = (f"[session {session} {st.get('state','?')} "
            f"seq={st.get('seq','?')} idle={st.get('idle','?')}ms{flag}]")
    if note:
        head += f" {note}"
    return head + "\n" + screen


@mcp.tool()
def terminal_new() -> dict:
    """Allocate a new persistent terminal session and return its number."""
    p = _conn()
    try:
        sid = int(p.readfile("/clone").decode().strip())
        return {"session": sid, "status": _status(p, sid)}
    finally:
        p.close()


@mcp.tool()
def terminal_list() -> list:
    """List the configured terminal sessions and their state."""
    out = []
    p = _conn()
    try:
        for i in range(1, 9):
            try:
                st = _status(p, i)
                out.append({"session": i, "state": st.get("state"),
                            "pid": st.get("pid"), "seq": st.get("seq")})
            except Exception:
                pass
    finally:
        p.close()
    return out


@mcp.tool()
def terminal_run(session: int, command: str, timeout: float = 15.0,
                 quiet_ms: int = 150) -> str:
    """Run a command in a persistent terminal and return the settled screen.

    The command is typed into the session and a newline is appended. The call
    waits until output has been quiet for quiet_ms, or timeout seconds. State
    (cwd, environment, functions, background jobs) persists between calls.
    Use terminal_send for interactive prompts that do not end in a newline.
    """
    p = _conn()
    try:
        if not command.endswith("\n"):
            command += "\n"
        p.writefile(f"/{session}/input", command.encode())
        st, timed_out = _wait(p, session, quiet_ms=quiet_ms, timeout=timeout)
        return _fmt(session, st, _screen(p, session), timed_out)
    finally:
        p.close()


@mcp.tool()
def terminal_send(session: int, text: str, enter: bool = True) -> str:
    """Send raw text to a session without waiting.

    Useful for prompts, password entry, editors, and programs that expect
    keystrokes rather than a command line. Set enter=False to send no newline.
    """
    p = _conn()
    try:
        data = text.encode()
        if enter:
            data += b"\n"
        p.writefile(f"/{session}/input", data)
        return f"sent {len(data)} bytes to session {session}"
    finally:
        p.close()


@mcp.tool()
def terminal_screen(session: int) -> str:
    """Read the current 24x80 text screen, without escape sequences."""
    p = _conn()
    try:
        return _screen(p, session)
    finally:
        p.close()


@mcp.tool()
def terminal_status(session: int) -> dict:
    """Read state, pid, cursor, output sequence, and idle time for a session."""
    p = _conn()
    try:
        return _status(p, session)
    finally:
        p.close()


@mcp.tool()
def terminal_scroll(session: int, lines: int = 40) -> str:
    """Read the last N lines of scrollback."""
    p = _conn()
    try:
        raw = p.readfile(f"/{session}/scroll").decode("utf-8", "replace")
        all_lines = raw.splitlines()
        return "\n".join(all_lines[-max(1, lines):])
    finally:
        p.close()


@mcp.tool()
def terminal_interrupt(session: int) -> str:
    """Send an interrupt to the session's process group."""
    p = _conn()
    try:
        p.writefile(f"/{session}/ctl", b"intr\n")
        return f"interrupted session {session}"
    finally:
        p.close()


@mcp.tool()
def terminal_clear(session: int) -> str:
    """Clear the terminal screen."""
    p = _conn()
    try:
        p.writefile(f"/{session}/ctl", b"clear\n")
        return f"cleared session {session}"
    finally:
        p.close()


@mcp.tool()
def terminal_resize(session: int, rows: int = 24, cols: int = 80) -> str:
    """Resize the emulated screen (programs are not notified)."""
    p = _conn()
    try:
        p.writefile(f"/{session}/ctl", f"resize {rows} {cols}\n".encode())
        return f"resized session {session} to {rows}x{cols}"
    finally:
        p.close()


if __name__ == "__main__":
    mcp.run()
