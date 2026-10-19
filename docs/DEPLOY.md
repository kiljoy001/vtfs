# Deploying on a 9front host

This is the layout used on `dev9p` (9front amd64).

## Build and install

From a drawterm or console session on the host:

```rc
mkdir /usr/scott/src/vtfs
cp /mnt/term/home/scott/Repo/vtfs/vtfs.c /usr/scott/src/vtfs/vtfs.c
cd /usr/scott/src/vtfs
6c vtfs.c
6l -o vtfs vtfs.6 /amd64/lib/lib9p.a
cp vtfs /usr/scott/bin/amd64/vtfs
chmod +x /usr/scott/bin/amd64/vtfs
```

`/mnt/term` is drawterm's export of the client filesystem.

## Start at login

`deploy/vtfs-start.rc` is installed as
`/usr/scott/bin/rc/vtfs-start` and referenced from `$home/lib/profile`:

```rc
if(test -x $home/bin/rc/vtfs-start)
	$home/bin/rc/vtfs-start
```

The script is idempotent: it returns immediately if `/srv/vt` exists. It
starts the mount server and the TCP listener with an attach token:

```rc
$home/bin/amd64/vtfs -s vt -M -a 'tcp!*!5640' -T $home/lib/vtfs.token &
```

Create the token once on the host and copy it to dev9p mode 600:

```sh
openssl rand -hex 24 > ~/.config/vtfs/token
```

```rc
cp /mnt/term/home/scott/.config/vtfs/token $home/lib/vtfs.token
chmod 600 $home/lib/vtfs.token
```

`-M` skips the server's local mount (which cannot supply the token). A local
client mounts the posted srv with the token as the 9P attach name:

```rc
mount /srv/vt /mnt/vt `{cat $home/lib/vtfs.token}
```

The listener binds all interfaces. The token is authentication, not
encryption; restrict it to a trusted network or put VPN/TLS in front.

## MCP registration

The host-side `vtfs_mcp.py` defaults to `dev9p:5640`. Register it with each
harness; examples:

OpenCode (`~/.config/opencode/opencode.json`, JSONC):

```json
"vtfs": {
  "type": "local",
  "command": ["/usr/bin/python3", "/home/scott/Repo/vtfs/vtfs_mcp.py"],
  "enabled": true
}
```

Claude Code / Claude Desktop (`mcpServers`):

```json
"vtfs": {
  "command": "/usr/bin/python3",
  "args": ["/home/scott/Repo/vtfs/vtfs_mcp.py"],
  "env": {"VTFS_ADDR": "dev9p:5640"}
}
```

Codex (`~/.codex/config.toml`):

```toml
[mcp_servers.vtfs]
command = "/usr/bin/python3"
args = [ "/home/scott/Repo/vtfs/vtfs_mcp.py" ]
env = { VTFS_ADDR = "dev9p:5640" }
startup_timeout_sec = 30
# token is read from ~/.config/vtfs/token (or VTFS_TOKEN / VTFS_TOKEN_FILE)
```

Gemini CLI (`~/.gemini/settings.json`) and Antigravity
(`~/.gemini/config/mcp_config.json`) use the same `mcpServers` object.
Muse adds `"mode": "optional"` to the entry.

## Verify

```sh
VTFS_ADDR=dev9p:5640 python3 /home/scott/Repo/vtfs/vtfs_mcp.py   # MCP over stdio
python3 /home/scott/Repo/vtfs/p9client.py 1 'echo hello'         # plain 9P
```

A healthy session answers `terminal_status` with `state running`, a `pid`,
and a `seq` that changes when output is produced. `terminal_exec` returns
`{"status": "0", "output": ...}` on success. `terminal_restart` re-execs rc
in place; `terminal_kill` stops the wrapper and shell.

## Skill

`skills/plan9-rc/SKILL.md` is an agent skill covering rc syntax, vtfs, and the
common bash-to-rc failures. Copy it into each harness's skills directory:

```sh
for d in ~/.claude/skills ~/.codex/skills ~/.gemini/skills ~/.gemini/antigravity/skills; do
  mkdir -p "$d/plan9-rc"
  cp skills/plan9-rc/SKILL.md "$d/plan9-rc/SKILL.md"
done
```
