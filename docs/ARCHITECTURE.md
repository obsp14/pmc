# Architecture

## Data path
```
UART ISR --bytes--> ring buffer --sem--> mctp_thread
   mctp_thread: deframer (DSP0253) -> packet layer (reassembly, DSP0236)
                -> pmc_rx_dispatch: type 0x00 -> MCTP control responder
                                    type 0x01 -> PLDM: base | platform | fwup | OEM
monitor_thread (500 ms): sensors -> thresholds/hysteresis -> RAS log
                         -> register-map snapshot -> I2C target (ISR serves it)
main: init, reset-cause logging, trial-image confirmation
```

## MCTP
* **Serial binding.** Frames are `7E | rev 01 | len | packet | FCS-16 | 7E`
  with 0x7E/0x7D escaped. The FCS is CRC-16/CCITT (reflected 0x8408, init
  0xFFFF), the same as the Linux `mctp-serial` driver. The test vector
  `"123456789" -> 0x6F91` (CRC-16/MCRF4XX) is checked in both C and Python.
* **Packets.** The 4-byte header plus the baseline 64-byte payload. Messages up
  to 1 KiB are reassembled using SOM/EOM, a 2-bit sequence number, the tag and
  the tag-owner bit. Out-of-sequence packets drop the message.
* **Addressing.** Before a bus owner assigns an EID, the endpoint accepts EID 0
  (null) and 0xFF (broadcast). `Set Endpoint ID` assigns it, just as the Linux
  `mctpd` would.
* **Control commands:** Set/Get Endpoint ID, Get MCTP Version Support, Get
  Message Type Support.

## PLDM
| Type | Commands |
|---|---|
| 0 base | GetTID, GetPLDMVersion (with CRC-32), GetPLDMTypes, GetPLDMCommands |
| 2 platform | GetSensorReading (sint32, PLDM sensor states) |
| 5 fw update | RequestUpdate, PassComponentTable, UpdateComponent, ActivateFirmware, GetStatus, CancelUpdate; issues RequestFirmwareData, TransferComplete, VerifyComplete, ApplyComplete |
| 63 OEM | error-log count/entry, link statistics, test injection (simulator only) |

### Firmware-update state machine (device side)
```
IDLE --RequestUpdate--> LEARN_COMPONENTS --PassComponentTable(end)--> READY_XFER
READY_XFER --UpdateComponent--> DOWNLOAD --(pull all chunks)--> VERIFY
VERIFY --ok--> APPLY --ApplyComplete--> READY_XFER --ActivateFirmware--> ACTIVATE (reboot)
VERIFY --fail--> IDLE (logged as RAS fwup event; old image keeps running)
```
A bug found and fixed while writing the tests: the device used to send
`VerifyComplete(fail)` *before* switching to IDLE. Over a fast link the
agent's reply was processed in between, and the state machine moved on to
APPLY. Rule learned: **update your state before you send the message that can
trigger the next event.**

## RAS log
Records are 32 bytes (`magic, seq, timestamp, boot, code, data, source,
severity, count, crc32`), stored in a ring of slots.
* **Recovery.** Scan all slots and take the valid record with the highest seq.
  A torn write fails its CRC and is ignored.
* **Never overwrite in place.** Even a coalesced update goes to the next slot
  under a new seq, so a power cut never destroys the only copy.
* **Coalescing.** Identical source, code and severity in the same boot become
  one record. The exact count is kept in RAM, and a copy is persisted at counts
  1, 2, 4, …, 128 and 255.
* **Backends.** On Zephyr, NVS: STM32F4 flash erases in 16–128 KiB sectors, so
  NVS's log structure does the erasing and wear-levelling. In the simulator, a
  plain file.

## Sensor monitoring
Each sensor has warn/crit thresholds (upper and lower) plus hysteresis.
* Entering a worse state happens at the threshold.
* Leaving it requires crossing threshold − hysteresis.
* Only transitions *into* abnormal states are logged.

## Firmware update on the board (MCUboot)
| Step | What happens |
|---|---|
| download | `flash_img` streams the image into slot 1. Chunks always arrive in order because the device requests them. |
| verify | Checks the MCUboot header magic. The real ECDSA-P256 signature check happens in MCUboot at reboot, so an unsigned or foreign image is never swapped in. |
| apply | `boot_request_upgrade(BOOT_UPGRADE_TEST)` |
| activate | Reboot from a delayed work item, so the PLDM response leaves the UART first. |
| confirm | After 10 s with no fatal RAS event: `boot_write_img_confirmed()`. |

## Linux hwmon driver
* An I2C target at 0x42 holds a read-only register file (layout in
  `core/include/pmc/monitor.h`). The firmware double-buffers it, so the ISR
  never reads a half-updated snapshot.
* The driver maps it onto standard hwmon attributes (`temp1_input`, `in0_input`,
  alarms) plus one custom `error_count` attribute.
