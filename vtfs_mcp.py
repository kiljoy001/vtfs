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
import re
import secrets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p9client import P9
from mcp.server.fastmcp import FastMCP

logging.basicConfig(level=logging.INFO, stream=sys.stderr)
log = logging.getLogger("vtfs-mcp")

mcp = FastMCP("vtfs")
DEFAULT_ADDR = os.environ.get("VTFS_ADDR", "dev9p:5640")
RAWMAX = 256 * 1024
ANSI = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-9;?]*[ -/]*[@-~]")


def _addr():
    a = DEFAULT_ADDR
    if a.startswith("tcp!"):
        a = a[4:]
    host, _, port = a.partition(":")
    return (host or "dev9p"), int(port or 5640)


def _rc_guard(command):
    """Reject && and || outside quotes; rc parses them as background/pipe."""
    q = False
    i = 0
    while i < len(command):
        c = command[i]
        if c == "'":
            if q and i + 1 < len(command) and command[i + 1] == "'":
                i += 2
                continue
            q = not q
            i += 1
            continue
        if not q and command[i:i + 2] in ("&&", "||"):
            return ("Plan 9 rc does not support && or ||. "
                    "Use ';' to sequence commands, or 'if(cmd)' to test status. "
                    "(rc reads && as backgrounding the left command.)")
        i += 1
    return None


def _rejected(session, why):
    return {"session": session, "status": "rejected", "output": why,
            "timed_out": False, "truncated": False, "state": "rejected"}


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

    The shell is Plan 9 rc, not POSIX sh: use ';' not '&&', '$status' not
    '$?', and rc quoting ('single quotes'; '' for a literal quote). There is
    no exit-status field; append '; echo STATUS $status' if you need one.
    """
    why = _rc_guard(command)
    if why:
        return f"[rejected] {why}"

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
    The shell is Plan 9 rc, not POSIX sh (use ';' not '&&').
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
def terminal_exec(session: int, command: str, timeout: float = 30.0) -> dict:
    """Run a command and return its clean output and rc $status.

    Unlike terminal_run, this reads the raw output stream, so the result is
    just what the command wrote (ANSI escapes stripped), not a 24x80 screen
    snapshot. A marker line after the command carries rc's $status. The
    shell is Plan 9 rc: use ';' not '&&', '$status' not '$?'.

    Output is capped at 256 KiB; check the 'truncated' field. Background
    commands (ending in '&') return immediately and their later output is
    not included. Use terminal_screen or terminal_scroll to inspect.
    """
    why = _rc_guard(command)
    if why:
        return _rejected(session, why)

    p = _conn()
    try:
        st0 = _status(p, session)
        out0 = int(st0.get("out", "0"))
        marker = "__VTFS_" + secrets.token_hex(6) + "__"
        p.writefile(f"/{session}/input", (command.rstrip("\n") + "\n").encode())
        p.writefile(f"/{session}/input", f"echo {marker} $status\n".encode())

        deadline = time.time() + timeout
        found = False
        st = st0
        while time.time() < deadline:
            st = _status(p, session)
            out = int(st.get("out", "0"))
            if out > out0:
                start = max(out0, out - 65536)
                tail = p.read_range(f"/{session}/cons", start, out - start)
                if marker.encode() in tail:
                    found = True
                    break
            if st.get("state") == "dead":
                break
            time.sleep(0.05)

        st = _status(p, session)
        out = int(st.get("out", "0"))
        avail = max(0, out - out0)
        truncated = avail > RAWMAX
        start = max(out0, out - RAWMAX) if truncated else out0
        data = p.read_range(f"/{session}/cons", start, max(0, out - start))
        text = ANSI.sub("", data.decode("utf-8", "replace"))
        text = text.replace("\r\n", "\n").replace("\r", "\n")

        idx = text.rfind(marker)
        if idx >= 0:
            status = text[idx + len(marker):].split("\n", 1)[0].strip()
            if status == "":
                status = "0"  # rc uses an empty string for success
            output = text[:idx].rstrip("\n")
        else:
            status = "dead" if st.get("state") == "dead" else "unknown"
            output = text.rstrip("\n")

        return {
            "session": session,
            "status": status,
            "output": output,
            "timed_out": (not found) and st.get("state") != "dead",
            "truncated": truncated,
            "state": st.get("state"),
        }
    finally:
        p.close()


def _shquote(s):
    return "'" + s.replace("'", "''") + "'"


@mcp.tool()
def terminal_put(session: int, path: str, content: str,
                 append: bool = False) -> dict:
    """Write text to a file on the Plan 9 machine (text only, not binary).

    Content is emitted as a block of echo statements and redirected to path.
    NUL bytes are rejected; use hget, 9P, or a real file server for binaries.
    Set append=True to add to an existing file.
    """
    if "\x00" in content:
        return {"status": "rejected", "output": "NUL bytes are not supported",
                "path": path, "bytes": 0}
    if len(content.encode()) > 8192:
        return {"status": "rejected", "output": "content too large (8 KiB max)",
                "path": path, "bytes": 0}
    lines = content.splitlines()
    if not lines:
        if append:
            return {"status": "0", "output": "", "path": path, "bytes": 0}
        cmd = "cp /dev/null " + _shquote(path)
    else:
        body = "; ".join("echo " + _shquote(line) for line in lines)
        cmd = "{ " + body + "; } " + (">>" if append else ">") + " " + _shquote(path)
    r = terminal_exec(session, cmd)
    r["path"] = path
    r["bytes"] = len(content.encode())
    return r


@mcp.tool()
def terminal_get(session: int, path: str) -> dict:
    """Read a text file from the Plan 9 machine (text only, not binary)."""
    r = terminal_exec(session, "cat " + _shquote(path))
    r["path"] = path
    r["content"] = r.pop("output")
    return r


@mcp.tool()
def terminal_interrupt(session: int) -> dict:
    """Interrupt the session's foreground command, not the shell itself."""
    p = _conn()
    try:
        p.writefile(f"/{session}/ctl", b"intr\n")
        time.sleep(0.15)
        return _status(p, session)
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
