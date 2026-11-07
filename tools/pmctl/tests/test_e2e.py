"""
End-to-end: the Python host stack talks to the C core running in pmc-sim
over a pseudo-terminal - the same path as the real board's USB serial.
"""
import os
import subprocess
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SIM = os.path.join(ROOT, "build", "pmc-sim")

from pmctl.client import Device, PldmError, sim_image  # noqa: E402
from pmctl.link import Deframer, Link, fcs16, frame  # noqa: E402

pytestmark = pytest.mark.skipif(not os.path.exists(SIM), reason="build/pmc-sim not built")


class Sim:
    def __init__(self, state):
        self.p = subprocess.Popen([SIM, "--state", str(state)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        line = self.p.stdout.readline().split()
        assert line[0] == "PTY", line
        self.pty = line[1]
        self.dev = Device(Link(self.pty))
        self.dev.set_eid(0x1D)

    def stop(self):
        self.dev.link.close()
        if self.p.poll() is None:
            self.p.terminate()
        self.p.wait(timeout=5)


@pytest.fixture
def sim(tmp_path):
    s = Sim(tmp_path)
    yield s
    s.stop()


def test_framing_matches_c():
    assert fcs16(b"123456789") == 0x6F91
    pkt = bytes([1, 0x1D, 8, 0xC8, 0x7E, 0x7D, 0x00])
    d = Deframer()
    assert d.feed(b"\x00\x13" + frame(pkt)) == [pkt]


def test_discovery(sim):
    d = sim.dev
    assert d.get_eid() == 0x1D
    assert d.message_types() == [0, 1]
    assert d.mctp_version() == "1.3.1"
    assert d.tid() == 1
    assert d.pldm_types() == [0, 2, 5, 63]
    assert d.pldm_version(2) == "1.2.0"


def test_sensors_and_threshold_event(sim, tmp_path):
    d = sim.dev
    t = d.sensor(1)
    assert 300 <= t.value <= 400 and t.state == "normal"
    assert 3250 <= d.sensor(2).value <= 3350
    with pytest.raises(PldmError):
        d.sensor(7)
    n0 = d.log_count()
    (tmp_path / "temp_override").write_text("880")      # 88.0 degC
    time.sleep(0.6)
    assert d.sensor(1).state == "upper-critical"
    assert d.log_count() == n0 + 1
    rec = d.log_entry(0)
    assert rec.source == "sensor" and rec.severity == "recoverable" and rec.data == 880


def test_log_survives_restart(tmp_path):
    s = Sim(tmp_path)
    s.dev.inject(6, 3, 0xBEEF)
    for _ in range(5):
        s.dev.inject(4, 1, 0x0042)      # coalesced link errors
    assert s.dev.log_entry(0).count == 5
    s.stop()
    s = Sim(tmp_path)                    # "reboot"
    try:
        recs = [s.dev.log_entry(i) for i in range(s.dev.log_count())]
        codes = [(r.code, r.boot, r.count) for r in recs]
        assert (0xBEEF, 1, 1) in codes
        assert (0x42, 1, 4) in codes          # last persisted power of two
        assert recs[0].source == "boot" and recs[0].boot == 2
    finally:
        s.stop()


def test_firmware_update_good(sim, tmp_path):
    payload = os.urandom(5000)
    seen = []
    res = sim.dev.fw_update(sim_image(payload), progress=lambda a, b: seen.append(a))
    assert res.startswith("activated"), res
    assert seen[-1] == 5032
    assert (tmp_path / "fw_slot_b.bin").read_bytes() == sim_image(payload)
    assert sim.p.wait(timeout=5) == 3          # simulator "rebooted"


def test_firmware_update_bad_image_rejected(sim):
    img = bytearray(sim_image(os.urandom(3000)))
    img[100] ^= 1
    res = sim.dev.fw_update(bytes(img))
    assert res.startswith("rejected"), res
    st = sim.dev.fw_status()
    assert st["state"] == "IDLE" and st["reason"] == 3
    assert any(sim.dev.log_entry(i).source == "fwup" for i in range(sim.dev.log_count()))
    assert sim.p.poll() is None                 # still running the old image


def test_link_noise_resync(sim):
    os.write(sim.dev.link.fd, b"\x7e\x01\x40garbage\x7e\x55\x7e")   # junk on the wire
    assert sim.dev.tid() == 1                                       # still works
    st = sim.dev.link_stats()
    assert st["frames_ok"] > 0
    assert st["fcs_errors"] + st["len_errors"] + st["framing_errors"] >= 1
