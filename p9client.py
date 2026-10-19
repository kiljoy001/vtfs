#!/usr/bin/env python3
"""Minimal 9P2000 client for vtfs: walk, open, read, write, clunk."""
import socket, struct, time, sys, os

Tversion, Rversion = 100, 101
Tauth, Rauth = 102, 103
Tattach, Rattach = 104, 105
Rerror = 107
Tflush, Rflush = 108, 109
Twalk, Rwalk = 110, 111
Topen, Ropen = 112, 113
Tcreate, Rcreate = 114, 115
Tread, Rread = 116, 117
Twrite, Rwrite = 118, 119
Tclunk, Rclunk = 120, 121
Tremove, Rremove = 122, 123
Tstat, Rstat = 124, 125
Twstat, Rwstat = 126, 127

OREAD, OWRITE, ORDWR = 0, 1, 2
NOFID = 0xffffffff

def S(s):
    b = s.encode()
    return struct.pack('<H', len(b)) + b

def D(b, o=0):
    n = struct.unpack_from('<H', b, o)[0]
    return b[o+2:o+2+n], o+2+n

class P9:
    def __init__(self, host='127.0.0.1', port=15640, uname='glenda', msize=8192,
                 aname=None):
        self.s = socket.create_connection((host, port), timeout=5)
        self.msize = msize
        self.tag = 1
        self.fid = 1
        if aname is None:
            aname = os.environ.get("VTFS_TOKEN", "")
            if not aname:
                path = os.environ.get("VTFS_TOKEN_FILE",
                                      os.path.expanduser("~/.config/vtfs/token"))
                try:
                    with open(path, "r", encoding="utf-8") as f:
                        aname = f.read().strip()
                except OSError:
                    aname = ""
        self.aname = aname
        self.rpc(Tversion, struct.pack('<I', msize) + S('9P2000'), tag=0xffff)
        self.rpc(Tattach, struct.pack('<II', 0, NOFID) + S(uname) + S(aname), fid=0)

    def rpc(self, typ, payload, tag=None, fid=None, expect=None):
        if tag is None:
            tag = self.tag; self.tag = (self.tag + 1) & 0xffff
        msg = struct.pack('<IBH', 7 + len(payload), typ, tag) + payload
        self.s.sendall(msg)
        hdr = self.recvn(7)
        size, rtyp, rtag = struct.unpack('<IBH', hdr)
        body = self.recvn(size - 7)
        if rtyp == Rerror:
            e, _ = D(body)
            raise OSError(f"9P error: {e.decode('utf-8','replace')}")
        if expect is not None and rtyp != expect:
            raise OSError(f"9P: expected {expect}, got {rtyp}")
        return body

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def recvn(self, n):
        b = b''
        while len(b) < n:
            d = self.s.recv(n - len(b))
            if not d:
                raise EOFError("9P connection closed")
            b += d
        return b

    def walk(self, fid, path):
        names = [p for p in path.split('/') if p]
        payload = struct.pack('<IIH', fid, self.fid, len(names))
        for n in names:
            payload += S(n)
        newfid = self.fid; self.fid += 1
        body = self.rpc(Twalk, payload, expect=Rwalk)
        nwqid = struct.unpack_from('<H', body)[0]
        if nwqid != len(names):
            raise OSError(f"walk {path}: got {nwqid}/{len(names)}")
        return newfid

    def open(self, fid, mode=OREAD):
        self.rpc(Topen, struct.pack('<IB', fid, mode), expect=Ropen)

    def read(self, fid, off=0, count=8192):
        body = self.rpc(Tread, struct.pack('<IQI', fid, off, count), expect=Rread)
        n = struct.unpack_from('<I', body)[0]
        return body[4:4+n]

    def read_range(self, path, off, count):
        """Read up to count bytes from an absolute offset."""
        f = self.walk(0, path)
        try:
            self.open(f, OREAD)
            out = b""
            while len(out) < count:
                d = self.read(f, off + len(out), min(8192, count - len(out)))
                if not d:
                    break
                out += d
            return out
        finally:
            self.clunk(f)

    def write(self, fid, data, off=0):
        body = self.rpc(Twrite, struct.pack('<IQI', fid, off, len(data)) + data, expect=Rwrite)
        return struct.unpack_from('<I', body)[0]

    def clunk(self, fid):
        self.rpc(Tclunk, struct.pack('<I', fid), expect=Rclunk)

    def readfile(self, path):
        f = self.walk(0, path)
        try:
            self.open(f, OREAD)
            out = b''
            off = 0
            while True:
                d = self.read(f, off)
                if not d:
                    break
                out += d; off += len(d)
            return out
        finally:
            self.clunk(f)

    def writefile(self, path, data):
        f = self.walk(0, path)
        try:
            self.open(f, OWRITE)
            n = self.write(f, data)
            if n != len(data):
                raise OSError(f"short write {n}/{len(data)}")
        finally:
            self.clunk(f)

def run(session, cmd, quiet_ms=150, timeout=10):
    p = P9()
    p.writefile(f'/{session}/input', (cmd + '\n').encode())
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        st = dict(line.split(' ', 1) for line in p.readfile(f'/{session}/status').decode().strip().split('\n') if ' ' in line)
        seq, idle = st.get('seq'), st.get('idle')
        if seq == last and idle is not None and int(idle) > quiet_ms:
            break
        last = seq
        time.sleep(0.05)
    screen = p.readfile(f'/{session}/screen').decode('utf-8', 'replace')
    st = p.readfile(f'/{session}/status').decode()
    return screen, st

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("usage: p9client.py SESSION COMMAND...")
        sys.exit(1)
    screen, st = run(sys.argv[1], ' '.join(sys.argv[2:]))
    print(screen)
    print('--- status ---')
    print(st)
