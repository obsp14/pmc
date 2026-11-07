"""
High-level client: MCTP control, PLDM base / platform / OEM, and the
Update Agent side of PLDM firmware update (DSP0267).
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

from .link import Link

CTRL, PLDM = 0x00, 0x01
T_BASE, T_PLAT, T_FWUP, T_OEM = 0x00, 0x02, 0x05, 0x3F
STATES = {0: "IDLE", 1: "LEARN_COMPONENTS", 2: "READY_XFER", 3: "DOWNLOAD",
          4: "VERIFY", 5: "APPLY", 6: "ACTIVATE"}
SENSOR_STATES = {0: "unknown", 1: "normal", 5: "lower-warning", 6: "lower-critical",
                 8: "upper-warning", 9: "upper-critical"}
SEV = {0: "info", 1: "corrected", 2: "recoverable", 3: "FATAL"}
SRC = {1: "boot", 2: "watchdog", 3: "sensor", 4: "link", 5: "fwup", 6: "test"}


class PldmError(Exception):
    pass


@dataclass
class Reading:
    sensor: int
    value: int
    state: str
    operational: str


@dataclass
class LogRecord:
    seq: int
    timestamp_ms: int
    boot: int
    code: int
    data: int
    source: str
    severity: str
    count: int


class Device:
    def __init__(self, link: Link, eid: int = 0):
        self.link = link
        self.eid = eid
        self.iid = 0

    # ---------------- MCTP control ----------------
    def _ctrl(self, cmd: int, data: bytes = b"") -> bytes:
        self.iid = (self.iid + 1) & 0x1F
        rsp = self.link.request(self.eid, bytes([CTRL, 0x80 | self.iid, cmd]) + data)
        if rsp[3] != 0:
            raise PldmError(f"MCTP control cmd {cmd:#x}: completion code {rsp[3]:#x}")
        return rsp[4:]

    def set_eid(self, eid: int) -> int:
        r = self._ctrl(0x01, bytes([0x00, eid]))
        self.eid = r[1]
        return self.eid

    def get_eid(self) -> int:
        return self._ctrl(0x02)[0]

    def message_types(self) -> list[int]:
        r = self._ctrl(0x05)
        return list(r[1:1 + r[0]])

    def mctp_version(self) -> str:
        r = self._ctrl(0x04, b"\xff")
        v = r[1:5]
        return f"{v[0] & 0xF}.{v[1] & 0xF}.{v[2] & 0xF}"

    # ---------------- PLDM ----------------
    def _pldm(self, ptype: int, cmd: int, data: bytes = b"", timeout: float = 2.0) -> bytes:
        self.iid = (self.iid + 1) & 0x1F
        rsp = self.link.request(self.eid, bytes([PLDM, 0x80 | self.iid, ptype, cmd]) + data, timeout)
        if len(rsp) < 5:
            raise PldmError("short PLDM response")
        if rsp[4] != 0:
            raise PldmError(f"PLDM type {ptype} cmd {cmd:#x}: completion code {rsp[4]:#x}")
        return rsp[5:]

    def tid(self) -> int:
        return self._pldm(T_BASE, 0x02)[0]

    def pldm_types(self) -> list[int]:
        bits = int.from_bytes(self._pldm(T_BASE, 0x04)[:8], "little")
        return [i for i in range(64) if bits >> i & 1]

    def pldm_version(self, ptype: int) -> str:
        r = self._pldm(T_BASE, 0x03, struct.pack("<IBB", 0, 1, ptype))
        v = r[5:9]
        crc = struct.unpack("<I", r[9:13])[0]
        import zlib
        if zlib.crc32(v) != crc:
            raise PldmError("version CRC mismatch")
        return f"{v[0] & 0xF}.{v[1] & 0xF}.{v[2] & 0xF}"

    def sensor(self, sid: int) -> Reading:
        r = self._pldm(T_PLAT, 0x11, struct.pack("<HB", sid, 0))
        val = struct.unpack("<i", r[6:10])[0]
        return Reading(sid, val, SENSOR_STATES.get(r[3], str(r[3])),
                       "enabled" if r[1] == 0 else "unavailable")

    # ---------------- OEM ----------------
    def log_count(self) -> int:
        return struct.unpack("<I", self._pldm(T_OEM, 0x01))[0]

    def log_entry(self, idx: int) -> LogRecord:
        r = self._pldm(T_OEM, 0x02, struct.pack("<I", idx))
        magic, seq, ts, boot, code, data, src, sev, cnt, crc = struct.unpack("<IIIIIIHBBI", r[:32])
        import zlib
        if magic != 0x52415331 or zlib.crc32(r[:28]) != crc:
            raise PldmError("corrupt log record")
        return LogRecord(seq, ts, boot, code, data, SRC.get(src, str(src)),
                         SEV.get(sev, str(sev)), cnt)

    def inject(self, source: int, severity: int, code: int) -> None:
        self._pldm(T_OEM, 0x03, struct.pack("<HBI", source, severity, code))

    def link_stats(self) -> dict:
        r = struct.unpack("<6I", self._pldm(T_OEM, 0x04))
        return dict(zip(("frames_ok", "fcs_errors", "len_errors", "framing_errors",
                         "messages", "dropped"), r))

    # ---------------- firmware update (Update Agent) ----------------
    def fw_status(self) -> dict:
        r = self._pldm(T_FWUP, 0x1B)
        return {"state": STATES.get(r[0], r[0]), "previous": STATES.get(r[1], r[1]),
                "progress": r[4], "reason": r[5]}

    def fw_update(self, image: bytes, max_xfer: int = 256, progress=None,
                  activate: bool = True) -> str:
        """Run the full DSP0267 flow. The device pulls data with
        RequestFirmwareData; we answer, then follow its Transfer/Verify/
        ApplyComplete notifications. Returns the final outcome."""
        ver = b"pmc-1.0"
        self._pldm(T_FWUP, 0x10, struct.pack("<IHBHBB", max_xfer, 1, 1, 0, 1, len(ver)) + ver)
        self._pldm(T_FWUP, 0x13, struct.pack("<BHHBIBB", 0x05, 0x000A, 1, 0, 1, 1, len(ver)) + ver)
        self._pldm(T_FWUP, 0x14, struct.pack("<HHBIIIBB", 0x000A, 1, 0, 1, len(image), 0, 1,
                                             len(ver)) + ver)
        verify_ok = None
        while True:
            src, tag, to, msg = self.link.recv(
                lambda s, t, o, m: o and len(m) >= 4 and m[0] == PLDM and m[2] == T_FWUP,
                timeout=5.0)
            cmd = msg[3]
            rsp = bytearray([PLDM, msg[1] & 0x1F, T_FWUP, cmd, 0])
            if cmd == 0x15:                                   # RequestFirmwareData
                off, n = struct.unpack("<II", msg[4:12])
                if off + n > len(image):
                    rsp[4] = 0x82                              # DATA_OUT_OF_RANGE
                else:
                    rsp += image[off:off + n]
                if progress:
                    progress(off + n, len(image))
            elif cmd == 0x16:                                 # TransferComplete
                pass
            elif cmd == 0x17:                                 # VerifyComplete
                verify_ok = msg[4] == 0
            self.link.send(src, bytes(rsp), False, tag)
            if cmd == 0x17 and not verify_ok:
                return "rejected: device verification failed"
            if cmd == 0x18:                                   # ApplyComplete
                if msg[4] != 0:
                    return "rejected: apply failed"
                break
        if activate:
            self._pldm(T_FWUP, 0x1A, b"\x01")
            return "activated: device is rebooting into the new image"
        return "applied: waiting for activation"


def sim_image(payload: bytes) -> bytes:
    """Image format the simulator accepts: payload || SHA-256(payload).
    (The real board uses an MCUboot-signed image instead.)"""
    return payload + hashlib.sha256(payload).digest()
