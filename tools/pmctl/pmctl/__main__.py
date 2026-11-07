"""
pmctl - talk to the management controller over MCTP/PLDM.

  python3 -m pmctl --dev /dev/ttyACM0 info
  python3 -m pmctl --dev /dev/pts/5 sensors [--watch]
  python3 -m pmctl --dev ... log
  python3 -m pmctl --dev ... update image.bin [--sim-image]
  python3 -m pmctl --dev ... stats
"""
from __future__ import annotations

import argparse
import sys
import time

from .client import Device, sim_image
from .link import Link

NAMES = {1: ("temperature", 0.1, "degC"), 2: ("voltage", 1, "mV")}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="pmctl")
    ap.add_argument("--dev", required=True)
    ap.add_argument("--eid", type=lambda s: int(s, 0), default=0x1D,
                    help="EID to assign to the device (bus-owner role)")
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("info")
    s = sp.add_parser("sensors"); s.add_argument("--watch", action="store_true")
    sp.add_parser("log")
    sp.add_parser("stats")
    u = sp.add_parser("update"); u.add_argument("image")
    u.add_argument("--sim-image", action="store_true",
                   help="append SHA-256 (simulator image format)")
    u.add_argument("--no-activate", action="store_true")
    a = ap.parse_args(argv)

    dev = Device(Link(a.dev))
    dev.set_eid(a.eid)          # we act as MCTP bus owner and assign the EID

    if a.cmd == "info":
        print(f"EID            : {dev.get_eid():#04x}")
        print(f"MCTP version   : {dev.mctp_version()}")
        print(f"message types  : {dev.message_types()}  (0 control, 1 PLDM)")
        print(f"PLDM TID       : {dev.tid()}")
        print(f"PLDM types     : {dev.pldm_types()}")
        for t, n in ((0, "base"), (2, "platform"), (5, "fw update")):
            print(f"  {n:<12} : v{dev.pldm_version(t)}")
        print(f"firmware update: {dev.fw_status()}")
    elif a.cmd == "sensors":
        while True:
            for sid, (name, scale, unit) in NAMES.items():
                r = dev.sensor(sid)
                print(f"{name:<12} {r.value * scale:8.1f} {unit:<5} {r.state} ({r.operational})")
            if not a.watch:
                break
            time.sleep(1)
            print()
    elif a.cmd == "log":
        n = dev.log_count()
        print(f"{n} record(s), newest first")
        for i in range(n):
            r = dev.log_entry(i)
            print(f"  #{r.seq:<4} boot {r.boot:<3} t={r.timestamp_ms / 1000:8.1f}s "
                  f"{r.severity:<11} {r.source:<8} code={r.code:#06x} data={r.data} x{r.count}")
    elif a.cmd == "stats":
        for k, v in dev.link_stats().items():
            print(f"{k:<15}: {v}")
    elif a.cmd == "update":
        img = open(a.image, "rb").read()
        if a.sim_image:
            img = sim_image(img)

        def prog(done, total):
            print(f"\r  transferring {done}/{total} bytes ({100 * done // total}%)",
                  end="", flush=True)
        print(f"updating with {len(img)} bytes")
        res = dev.fw_update(img, progress=prog, activate=not a.no_activate)
        print(f"\n{res}")
        return 0 if not res.startswith("rejected") else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
