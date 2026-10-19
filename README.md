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

## Host-side bridge

`p9client.py` is a minimal 9P2000 client that walks, opens, reads and writes
the tree, plus the quiescence loop. It is the piece an MCP server wraps:

```sh
python3 p9client.py 1 'ls /sys/src | sed 5q'
```

It speaks to a `vtfs -a` listener, or to any 9P transport that exposes the
tree. On a real deployment put `tlssrv`/`factotum` in front of the listener;
there is no authentication in `vtfs` itself.

## Limitations

- The child is plain `rc`, so `intr` terminates the shell rather than
  returning to a prompt. Giving `rc` a private `/dev/cons` is the fix.
- Sessions are started before the server forks and cannot be restarted
  in place. A session-manager process is the next step.
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
p9client.py   host-side 9P client + quiescence loop
mkfile        9front build
docs/DESIGN.md  internals and the Plan 9 traps
docs/VM.md      reproducing the serial-console test VM
```

## License

MIT. See `LICENSE`.
