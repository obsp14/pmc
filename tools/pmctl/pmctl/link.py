"""
MCTP over serial (DSP0253) - host side, written independently of the C
core so the two implementations check each other.

Works on any tty: the simulator's PTY, the Nucleo's ST-LINK virtual COM
port (/dev/ttyACM0), or a USB-UART adapter. Uses termios directly, so no
pyserial dependency.
"""
from __future__ import annotations

import os
import select
import termios
import time
import tty

FLAG, ESC, REV = 0x7E, 0x7D, 0x01
SOM, EOM, TO = 0x80, 0x40, 0x08
BTU = 64


def fcs16(data: bytes, fcs: int = 0xFFFF) -> int:
    for b in data:
        fcs ^= b
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if fcs & 1 else fcs >> 1
    return fcs


def frame(pkt: bytes) -> bytes:
    body = bytes([REV, len(pkt)]) + pkt
    f = fcs16(body)
    body += bytes([f >> 8, f & 0xFF])
    out = bytearray([FLAG])
    for b in body:
        if b in (FLAG, ESC):
            out += bytes([ESC, b ^ 0x20])
        else:
            out.append(b)
    out.append(FLAG)
    return bytes(out)


class Deframer:
    def __init__(self):
        self.buf = bytearray()
        self.in_frame = False
        self.esc = False
        self.errors = 0

    def feed(self, data: bytes) -> list[bytes]:
        pkts = []
        for b in data:
            if b == FLAG:
                if self.in_frame and len(self.buf) >= 2:
                    p = self._finish()
                    if p is not None:
                        pkts.append(p)
                self.buf = bytearray()
                self.in_frame = True
                self.esc = False
                continue
            if not self.in_frame:
                continue
            if b == ESC:
                self.esc = True
                continue
            if self.esc:
                b ^= 0x20
                self.esc = False
            self.buf.append(b)
        return pkts

    def _finish(self):
        b = bytes(self.buf)
        if len(b) < 2 + 4 + 2 or b[0] != REV or b[1] != len(b) - 4:
            self.errors += 1
            return None
        if fcs16(b[:-2]) != (b[-2] << 8 | b[-1]):
            self.errors += 1
            return None
        return b[2:-2]


class Link:
    """One MCTP endpoint on a tty: sends messages, reassembles replies."""

    def __init__(self, dev: str, own_eid: int = 8, baud: int = 115200):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, f"B{baud}", termios.B115200)
        attrs[4] = attrs[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.eid = own_eid
        self.df = Deframer()
        self.tag = 0
        self.rasm: dict = {}
        self.inbox: list[tuple[int, int, bool, bytes]] = []

    def close(self) -> None:
        os.close(self.fd)

    # ---- transmit ----
    def send(self, dest: int, msg: bytes, tag_owner: bool, tag: int) -> None:
        seq = 0
        for off in range(0, len(msg), BTU):
            chunk = msg[off:off + BTU]
            flags = (SOM if off == 0 else 0) | (EOM if off + BTU >= len(msg) else 0)
            flags |= (seq & 3) << 4 | (TO if tag_owner else 0) | (tag & 7)
            os.write(self.fd, frame(bytes([0x01, dest, self.eid, flags]) + chunk))
            seq += 1

    def new_tag(self) -> int:
        self.tag = (self.tag + 1) & 7
        return self.tag

    # ---- receive ----
    def _pump(self, timeout: float) -> None:
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        for pkt in self.df.feed(data):
            self._packet(pkt)

    def _packet(self, pkt: bytes) -> None:
        if len(pkt) < 5 or (pkt[0] & 0x0F) != 1:
            return
        src, flags = pkt[2], pkt[3]
        key = (src, flags & 7, bool(flags & TO))
        if flags & SOM:
            self.rasm[key] = bytearray()
        if key not in self.rasm:
            return
        self.rasm[key] += pkt[4:]
        if flags & EOM:
            self.inbox.append((src, flags & 7, bool(flags & TO), bytes(self.rasm.pop(key))))

    def recv(self, pred, timeout: float = 2.0):
        """Wait for a message matching pred(src, tag, tag_owner, msg)."""
        end = time.monotonic() + timeout
        while True:
            for i, m in enumerate(self.inbox):
                if pred(*m):
                    return self.inbox.pop(i)
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError("no response from device")
            self._pump(min(left, 0.05))

    def request(self, dest: int, msg: bytes, timeout: float = 2.0) -> bytes:
        tag = self.new_tag()
        self.send(dest, msg, True, tag)
        _, _, _, rsp = self.recv(lambda s, t, to, m: t == tag and not to, timeout)
        return rsp
