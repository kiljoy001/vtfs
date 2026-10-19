# vtfs

A persistent terminal for agents, exposed as 9P files.

`vtfs` is a user-level 9P file server for Plan 9 and 9front. It runs a
long-lived `rc` shell and publishes the terminal as files: type into `input`,
read the rendered screen from `screen`, interrupt with `ctl`, and poll
`status` for a sequence number and idle time. There is no pty and no
per-command `exec`: the session is the object, and it survives between
requests.

This is deliberately *not* a shell tool. It is a terminal that happens to be
drivable by a program.

```
/mnt/vt/
  clone                 read: allocate a session id
  1/
    input               write: type a command line
    cons                write: same as input; read: raw output ring
    screen              read: 24x80 text, no escape sequences
    scroll              read: scrollback ring, oldest first
    ctl                 write: start reset intr kill free echo
                               resize R C clear quiet MS timeout MS
    status              read: state pid rows cols cursor seq idle
```

## Why

Agents need state, not one-shot commands. A terminal gives them:

- **Persistence.** `cd`, environment, functions and running processes stay
  alive between tool calls.
- **Observation.** `screen` is an idempotent snapshot of what a human would
  see, not a byte stream full of cursor escapes.
- **Interruption.** `echo intr > ctl` sends an interrupt to the session's
  process group.
- **Retry safety.** Reading `screen` twice does not consume anything.
- **Sharing.** Any number of 9P clients can mount the same session. A human
  can watch in `rio` while the model types.

## Quiescence

lib9p serializes request handling per connection, so `screen` must not block
waiting for the shell to settle. Instead `status` exposes:

```
state running
pid 474
seq 4
idle 1981
```

A client writes a command, then polls `status` until `seq` has stopped
changing and `idle` exceeds a threshold (default 120 ms), then reads
`screen`. That is the whole contract. `p9client.py` implements it.

`terminal_exec` takes the cleaner path: it records the raw byte offset
(`out` in `status`), appends a marker line carrying `$status`, and reads the
raw stream from that offset. The result is command output, not a screen
snapshot, and success is normalized to `"0"`. rc's empty success string is
the reason for that normalization.

## Build

On 9front amd64:

```rc
6c vtfs.c
6l -o vtfs vtfs.6 /amd64/lib/lib9p.a
```

or:

```rc
mk
```

The result links only against `lib9p` and `libc`; the reader and session
processes use `rfork`, not `libthread`.

## Run

```rc
vtfs -s vt -m /mnt/vt &
mount /srv/vt /mnt/vt

cat /mnt/vt/clone
echo 'echo hello' > /mnt/vt/1/input
sleep 1
cat /mnt/vt/1/screen
```

Flags:

```
-D            lib9p request tracing
-s name       /srv name (default vt)
-m mtpt       mountpoint (default /mnt/vt)
-a addr       also listen for 9P, e.g. 'tcp!*!5640'
```

## MCP server

`vtfs_mcp.py` exposes the terminal as MCP tools:

| tool | purpose |
|---|---|
| `terminal_new` | allocate a session |
| `terminal_list` | show sessions and state |
| `terminal_exec` | run a command, return clean output and `$status` |
| `terminal_run` | type a command, wait for quiet, return the screen |
| `terminal_send` | send raw keystrokes without waiting |
| `terminal_screen` | snapshot the visible screen |
| `terminal_status` | state, pid, cursor, seq, idle |
| `terminal_scroll` | tail of scrollback |
| `terminal_interrupt` | interrupt the foreground child, not the shell |
| `terminal_clear` | clear the screen |
| `terminal_resize` | resize the emulated grid |

It is a 9P client, not a shell wrapper. Point it at a listener with:

```sh
VTFS_ADDR=dev9p:5640 python3 vtfs_mcp.py
```

`p9client.py` is the same client without MCP; it is useful for testing and
for embedding the quiescence loop:

```sh
python3 p9client.py 1 'ls /sys/src | sed 5q'
```

On a real deployment put `tlssrv`/`factotum` in front of the listener; there
is no authentication in `vtfs` itself.

## Limitations

- Sessions are started before the server forks and cannot be restarted
  in place. `terminal_interrupt` now targets the foreground child, so the
  usual cause of a dead shell is gone; use `terminal_new` if one dies. A
  session-manager process is the next step.
- `terminal_exec` rejects `&&` and `||`: rc parses `&&` as backgrounding the
  left command, which is worse than a syntax error. Long output is capped at
  256 KiB; check `truncated`.
- Programs that insist on opening `/dev/cons` directly will see the machine
  console, not this terminal. stdio-based programs (`rc`, `ls`, `sed`,
  compilers) work.
- The VT parser covers the common subset: cursor motion, erase, insert /
  delete, scroll, SGR ignored, OSC ignored. It is not a complete xterm.
- The TCP listener works with Plan 9's own client; QEMU user-mode
  `hostfwd` was observed to reset connections after attach. Use a tap or
  bridged interface, or run the bridge inside the guest.

## Layout

```
vtfs.c        the server
vtfs_mcp.py   MCP tools over stdio
p9client.py   host-side 9P client + quiescence loop
mkfile        9front build
deploy/       start script for a user profile
docs/DESIGN.md  internals and the Plan 9 traps
docs/VM.md      reproducing the serial-console test VM
docs/DEPLOY.md  installing and registering on a 9front host
skills/plan9-rc  rc + vtfs skill for agent harnesses
```

## License

MIT. See `LICENSE`.
