#!/usr/bin/env python3
"""Tiny GDB remote-protocol client for RPCS3's stub (127.0.0.1:2345): sample the
main PPU thread a few times and print pc and the call chain (stack back chain).
Closing the connection ends RPCS3's stub thread (and freezes the emulation),
so use it once, on a run that is stuck anyway. Usage: rsp.py [samples]"""
import socket, sys, time

def pkt(data):
    return b'$' + data + b'#' + ('%02x' % (sum(data) & 0xff)).encode()

class RSP:
    def __init__(self):
        self.s = socket.create_connection(('127.0.0.1', 2345), timeout=15)
        self.buf = b''
    def reply(self):
        while True:
            i = self.buf.find(b'$')
            j = self.buf.find(b'#', i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                data = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                self.s.sendall(b'+')
                return data.decode(errors='replace')
            self.buf += self.s.recv(65536)
    def cmd(self, data):
        self.s.sendall(pkt(data.encode()))
        return self.reply()
    def reg(self, n):
        return int(self.cmd('p%x' % n), 16)
    def mem64(self, addr):
        r = self.cmd('m%x,8' % addr)
        return int(r, 16) if len(r) == 16 else 0

n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
r = RSP()
print('stop:', r.cmd('?'))
for k in range(n):
    r.cmd('Hg1000000')
    pc, lr, sp = r.reg(64), r.reg(67), r.reg(1)
    chain, fp = [], sp
    for _ in range(24):
        fp = r.mem64(fp)
        if not fp or fp > 0xffffffff:
            break
        chain.append(r.mem64(fp + 16))
    print('sample %d: pc %x lr %x chain %s' % (k, pc, lr, ' '.join('%x' % c for c in chain if c)))
    if k + 1 < n:
        r.s.sendall(pkt(b'c'))
        time.sleep(0.3)
        r.s.sendall(b'\x03')
        r.reply()
