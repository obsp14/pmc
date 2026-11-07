# pmc: a Zephyr platform management controller

A small **management controller**, the kind of chip that sits beside a big
SoC or server CPU and watches, protects and updates it. It runs on an
**STM32F446RE** (NUCLEO board) under **Zephyr RTOS** and speaks the
industry-standard server management protocols:

* **MCTP over serial** (DMTF DSP0236 / DSP0253) to a host
* **PLDM** (DSP0240 / DSP0248 / DSP0267) on top of it, for discovery,
  sensor monitoring and **firmware update**
* a **persistent RAS error log** that survives resets and power cuts
* **MCUboot**-verified A/B firmware updates with trial boot and automatic rollback
* a **Linux hwmon kernel driver** so a Raspberry Pi host sees the board in `sensors`

```
            USB (ST-LINK VCP)                          I2C (0x42)
 Laptop  <------------------->  NUCLEO-F446RE  <----------------->  Raspberry Pi
 pmctl:  MCTP/PLDM over serial   Zephyr: threads,   register map     pmc-hwmon.ko
 info, sensors, log, update      watchdog, NVS log,                  -> `sensors`
                                 MCUboot A/B slots
```

## What's here and what's verified

| Path | What | Status |
|---|---|---|
| `core/` | Portable C99 core: FCS-16/CRC-32, MCTP serial framer/deframer, packetisation + reassembly, MCTP control responder, PLDM base/platform/OEM responder, PLDM firmware-device state machine, threshold monitor with hysteresis, crash-safe RAS log, I2C register map | ✅ **157 checks under ASan/UBSan** |
| `sim/` | `pmc-sim`: the core behind a pseudo-terminal, standing in for the board's USB serial | ✅ |
| `tools/pmctl/` | Host tool (Python, *independent* implementation of framing/MCTP/PLDM) acting as bus owner and PLDM update agent | ✅ **7 end-to-end tests vs. pmc-sim** |
| `app/` | Zephyr application: UART ISR + ring buffer, threads, task watchdog, NVS log backend, MCUboot backend, I2C target, sensors | ⚠️ **untested**: no Zephyr SDK or board here |
| `linux/` | `pmc-hwmon` kernel module + Raspberry Pi DT overlay | ⚠️ **untested** |

The protocol logic was proven on the host first, so when the firmware runs
on the board, the remaining bugs are in the Zephyr glue, not in the
protocols.

## Quick start (host, no hardware)
```sh
sudo apt install build-essential libssl-dev python3-pytest
make test
./build/pmc-sim --state /tmp/pmc &          # prints: PTY /dev/pts/N
python3 -m pmctl --dev /dev/pts/N info      # (from tools/pmctl)
python3 -m pmctl --dev /dev/pts/N sensors
echo 880 > /tmp/pmc/temp_override            # 88.0 degC -> critical event
python3 -m pmctl --dev /dev/pts/N log
head -c 20000 /dev/urandom > fw.bin
python3 -m pmctl --dev /dev/pts/N update fw.bin --sim-image
```

## Design highlights
* **Same core, three homes.** The firmware, the simulator and the unit tests
  link the same `core/` sources. Only `pmc_platform_*` hooks and storage/
  firmware backends differ.
* **Robust serial link.** Any 0x7E flag resynchronises the receiver, the
  FCS-16 check rejects corrupted frames, and link statistics are readable over
  PLDM. A test injects line noise and checks recovery.
* **The firmware device pulls the image.** As in DSP0267, the device requests
  each chunk at its own pace (`RequestFirmwareData`), then reports
  `TransferComplete`, `VerifyComplete` and `ApplyComplete`. A corrupt image is
  rejected at verify and nothing is applied.
* **Trial boot.** The new image boots once in MCUboot *test* mode and confirms
  itself only after 10 s with no fatal event. A crash or watchdog reset before
  that rolls back.
* **RAS log.** CRC-32 per record, sequence numbers, and the newest valid
  record wins after a torn write. Identical errors are coalesced with a
  power-of-two persistence schedule, so an error storm costs ≤ 8 flash writes
  per event.
* **Watchdog per thread.** `task_wdt` channels sit on top of the IWDG. A
  watchdog reset is logged as a FATAL RAS event on the next boot.

Docs: [ARCHITECTURE](docs/ARCHITECTURE.md) · [HARDWARE bring-up](docs/HARDWARE.md) ·
[LEARNING_PATH](docs/LEARNING_PATH.md) · [app build](app/README.md)

## Scope and honesty
* This is a *subset* of the DMTF specs. Message layouts follow the specs for
  the commands implemented, with simplifications marked in `core/src/pldm.c`.
  The PDR repository and PLDM event messages are not implemented yet, and
  OpenBMC's `pldmd` needs them to manage the device automatically.
* Not conformance-tested against libpldm or a real BMC.
