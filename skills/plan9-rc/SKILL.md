---
name: plan9-rc
description: >
  Use when running commands on Plan 9 or 9front, or when using the vtfs
  terminal tools (terminal_exec, terminal_run, terminal_screen) to drive a
  9front machine. Covers the rc shell, which is not bash: quoting, lists,
  redirection, $status, control flow, and common 9front commands. Trigger on
  9front, Plan 9, rc, vtfs, drawterm, /sys/src, /amd64/bin, bind/mount, or a
  command failing with "syntax error" after using &&, $?, or 2>&1.
license: MIT
---

# Running commands on Plan 9 / 9front

The shell is **rc**, not bash. Most failures from an LLM are bash syntax, not
the operating system. Read this before the first command.

## First rule: rc is not bash

| bash | rc |
|---|---|
| `cmd1 && cmd2` | `cmd1; cmd2` (or `if(cmd1) cmd2`) |
| `cmd1 \|\| cmd2` | `if(! cmd1) cmd2` |
| `$?` | `$status` |
| `$(cmd)` | `` `{cmd} `` |
| `export X=1` | `X=1` (no export) |
| `cmd 2>&1` | `cmd >[2=1]` |
| `cmd 2>file` | `cmd >[2]file` |
| `"$var"` | `$"var` (single string) |
| `"a b"` | `'a b'` |
| `echo "it's"` | `echo 'it''s'` |
| `for x in a b; do ...; done` | `for(x in a b) ...` |
| `if [ -f x ]; then ...; fi` | `if(test -f x) ...` |
| `while [ ... ]; do ...; done` | `while(test ...) ...` |
| `function f { ...; }` | `fn f { ... }` |
| `$1`, `$@` | `$1`, `$*` |
| `$#` | `$#*` (argument count) |
| `head -10 f` | `sed 10q f` |
| `sudo ...` | no root; there is no sudo |
| `bash script` | `rc script` |

`&&` and `||` are **not** operators in rc. `&&` is read as `&` (background)
followed by `&`, so the left command runs in the background and the right one
runs immediately: wrong order, wrong status, no error. `||` is read as a
pipe. A trailing `;` is fine. `terminal_exec` rejects both before sending.

## Quoting and lists

rc has lists, not strings-with-spaces:

```rc
files=(a.c b.c c.c)
echo $#files          # 3
echo $files(1)        # a.c
echo $files           # a.c b.c c.c  (list expands to words)
echo $"files          # a.c b.c c.c  (one string)
```

Single quotes group words. A literal quote is doubled: `'don''t'`.

```rc
echo 'two words'
echo $#*             # number of arguments
```

## Variables and environment

```rc
name=value           # no spaces around =
path=(/bin /usr/$user/bin/$objtype)
echo $home
```

There is no `export`. Variables set in the shell are visible to children.

## Control flow

```rc
if(test -f /nope)
	echo exists
if not
	echo missing

if(! test -d /tmp/x)
	mkdir /tmp/x

if(~ $x '*.c')
	echo c-file

for(f in *.c)
	echo $f

for(i in `{ls /sys/src})
	echo $i

while(test -f /tmp/busy)
	sleep 1

fn greet {
	echo hello $1
}
greet world
```

`~` is pattern matching, not a path. `if(cmd)` runs the command and tests its
status. `$status` is a **string**; the empty string means success, and a
failure is a message such as `test 123: false`. `terminal_exec` normalizes
success to `"0"` and returns the error string otherwise.

## Redirection and pipes

```rc
cmd >out
cmd >>out
cmd <in
cmd >[2=1]            # stderr to stdout
cmd >[2]err           # stderr to file
cmd | tee log
cmd |[2] grep err     # pipe stderr
cmd &                 # background
```

## Plan 9 / 9front specifics

- Paths: `/sys/src`, `/amd64/bin`, `/usr/$user`, `$home`.
- Binaries are static; build with `6c`, `6l`, `mk`.
- `bind old new` and `mount /srv/x /mnt/x` change a namespace; there is no
  global root and no `sudo`.
- `ns` shows the current namespace.
- `/dev/sysname`, `/dev/swap`, `/proc/N/status`, `/net` are files.
- `ps` lists processes. `kill` may not exist; use `echo kill > /proc/PID/note`.
- Editors: `sam`, `ed`, `acme`; for scripts, `ed` or `sed` is easier.
- Fetch files with `hget URL` or `hget -o file URL`.
- `head` and `tail` are often absent; use `sed 10q` and `sed -n '5,$p'`.
- `wc`, `grep`, `sed`, `awk`, `tee`, `sort`, `uniq` are present.

## Driving a terminal with vtfs

Prefer `terminal_exec` for one-shot commands. It returns clean output and the
rc exit status; it is not a screen scrape.

```
terminal_exec(session, "cd /sys/src; ls | sed 5q")
-> {"status": "0", "output": "...", "timed_out": false, "truncated": false}
```

Use `terminal_run` when you need the visible screen or are driving an
interactive program; it returns the 24x80 screen including old output.
`terminal_screen` reads the screen, `terminal_scroll` the last N lines,
`terminal_send` sends keystrokes to a prompt, and `terminal_interrupt`
interrupts the foreground command without killing the shell.

Sessions are persistent shells. `cd`, variables, functions and background
jobs survive between calls. `terminal_new` allocates another session; use it
if a session reports `state dead`. There is no automatic restart.

Rules:

1. Use `;` to sequence; never `&&` or `||`.
2. Do not append `; echo $status` to `terminal_exec`; the tool already returns
   the status. Success is `"0"`; failures are rc's error strings.
3. Background jobs (`cmd &`) return immediately; use `terminal_screen` later.
4. Long output is capped at 256 KiB; check `truncated`.
5. `terminal_interrupt` targets the foreground child, not the shell. If
   nothing is running it does nothing; do not use `kill` on the shell.
6. Do not run `exit`, `reboot`, `mkfs`, or `rm -rf /` without explicit
   confirmation.

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| left command runs in background / wrong order | `&&` is `&` in rc | use `;` or `if(cmd)` |
| `$?` prints nothing | rc has no `$?` | use `$status` |
| `export: not found` | rc has no export | `X=value` |
| `$(` syntax error | rc has no `$()` | `` `{cmd} `` |
| `2>&1` syntax error | rc redirection | `>[2=1]` |
| `head: not found` | not installed | `sed 10q` |
| `sudo: not found` | no root | use the files you have |
| `permission denied` | file mode or namespace | `ls -l`, `ns` |
| command never returns | long-running or waiting for input | `terminal_interrupt`, then inspect `terminal_screen` |
| empty output, `status unknown` | shell died or marker lost | `terminal_status`, then `terminal_new` |

## Minimal recipes

```rc
# inspect
cd /sys/src; ls | sed 10q
grep -n 'fn main' /sys/src/cmd/rc/*.c | sed 10q

# build a Plan 9 program
cd /tmp
6c hello.c
6l -o hello hello.6

# build with lib9p
6c vtfs.c
6l -o vtfs vtfs.6 /amd64/lib/lib9p.a

# fetch
hget -o /tmp/x.c http://10.0.2.2:8000/x.c

# process inspection
ps | grep rc
cat /proc/1/status
netstat -n

# namespace
ns
bind /usr/scott/bin/amd64 /bin
```
