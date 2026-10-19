# Design notes

## A file server, not a driver

`vtfs` is a user-level 9P server, which is the normal way to add a facility on
Plan 9. A kernel driver would have to parse JSON/VT output with no floating
point, could not be replaced without a reboot, and would live in every
process's namespace with one global policy. A user-level server owns a
namespace, can be mounted per session, and dies without taking the machine
with it.

There is also no pty. The child's stdio is a pair of pipes, so the terminal is
emulated in the server. That is an advantage here: the server can publish a
**cell grid**, which is what a model wants, instead of a stream of escape
sequences.

## Processes

```
main
 ├─ start() x8
 │   ├─ rc child      (pipes on 0/1/2, RFMEM then exec)
 │   └─ reader        (RFMEM: shares the grid, reads the output pipe)
 ├─ postmountsrv     (RFMEM: mount server, /srv/vt)
 └─ listensrv        (RFMEM: optional TCP listener)
       └─ per connection: srv() (RFMEM)
             └─ per request: srvwork() (RFMEM)
```

`RFMEM` means **share** the memory image in Plan 9; without it the new
process gets a copy-on-write copy. Getting this backwards makes the reader
update a private grid and the server never see output.

## The per-request fd trap

lib9p handles each request in a forked process. File descriptors are
per-process, so a pipe created while handling request *N* does not exist in
the process that handles request *N+1*. The first version created sessions
lazily and stored `s->in`/`s->out`; later writes went to stale descriptor
numbers.

The fix is to create all pipes, children and readers in `main`, before
`postmountsrv`/`listensrv` fork the servers. Request handlers then inherit
the descriptor numbers and the shared `Session` state.

`reset`/`start` cannot re-create a pipe from a request handler for the same
reason. A session-manager process is the proper solution; for now sessions
are started once and `intr` may leave one dead.

## Why `screen` does not wait

`srvwork` holds `srv->slock` while a callback runs, so a blocking read would
stall `ctl` on the same connection. Instead:

- `status` publishes `seq` (monotonic output counter) and `idle` (ms since
  the last output).
- The client writes a command and polls until `seq` is stable and `idle`
  passes a threshold.
- `screen` is then a pure snapshot and can be retried freely.

This also keeps the server's request latency bounded.

For non-interactive work, `terminal_exec` does better: `status` exposes
`out`, the monotonic count of raw output bytes. The client records `out`,
runs the command followed by `echo MARKER $status`, waits for the marker in
the raw `cons` stream, and returns the bytes between the two. That gives
clean output and a real status without screen scraping. The raw ring is
256 KiB; older bytes are dropped and reported as `truncated`.

## Restart wrapper

`rc` is not run directly. A supervisor process holds the session's stdin and
stdout pipes, forks `rc`, and waits. When `rc` exits — `exit`, a fatal note,
or an explicit restart — the supervisor forks a new `rc` on the same pipes.
The session therefore survives without recreating descriptors, which is
what a manager process would otherwise have to solve. `terminal_kill` kills
the supervisor as well; `terminal_restart` kills only the current `rc`.

The supervisor updates the shared `pid` and `seq` fields before and after
each child, so `status` and `interrupt` always see the current shell.

## Attach token

`-T file` reads a token and makes the `Srv.attach` callback require it as the
9P attach name (`aname`). The MCP client reads the same token from
`~/.config/vtfs/token` and sends it in `Tattach`. Plan 9 clients pass it as
the third argument to `mount`. This is a bearer token: it authenticates the
attach but does not encrypt traffic, so pair it with a trusted network or a
VPN/TLS layer.

## Interrupting the right process

`postnote(PNGROUP, rcpid, ...)` kills the non-interactive shell along with the
command. `intr` instead scans `/proc/*/ppid` for direct children of the
session's `rc` and sends `interrupt` to each with `postnote(PNPROC, ...)`.
Pipelines are covered because every child is hit. If no child is running,
nothing happens and the shell survives.

## Terminal parser

The parser is a small state machine over runes:

- ground: printable, CR, LF, BS, TAB, BEL
- ESC: `[`, `]`, `(`, `)`, `7`, `8`, `D`, `M`, `E`, `c`
- CSI: cursor motion (`A B C D E F G d H f`), erase (`J K X`), insert /
  delete (`@ P L M`), scroll (`S T`), cursor report (`n 6`), save/restore
  (`s u`), alt-screen (`?1049 h l`); SGR and modes are ignored
- OSC: consumed until BEL or ST
- UTF-8 is decoded with `chartorune`; a partial rune is buffered

Scrollback is a ring of 100 rows. `screen` trims trailing blanks per line.

## Traps found while building this

1. `rc -i` reads `/dev/cons`, not stdin. Use plain `rc` when stdio is a pipe.
2. `RFMEM` shares memory; without it the reader's updates are private.
3. lib9p forks per request; fds must exist before the server forks.
4. `srv->slock` serializes requests; never block in `read`.
5. `clone` must honor `offset`, or `cat clone` allocates twice.
6. On a 9front live ISO, `9660srv` rejects xorriso's Rock Ridge records and
   the ISO must omit `;1` version suffixes (`-compliance
   omit_version:no_force_dots -rockridge off -joliet off`).
