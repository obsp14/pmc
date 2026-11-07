# Hardware bring-up

## Parts
* NUCLEO-F446RE (you already have it)
* USB-UART adapter (3.3 V) for the console on USART1, around ₹200
* Raspberry Pi 3/4/5 for the Linux hwmon part (optional at first)
* 4 jumper wires

## Wiring
| Signal | NUCLEO-F446RE | Other side |
|---|---|---|
| MCTP link | USART2 is the ST-LINK USB port | Laptop `/dev/ttyACM0` |
| Console TX | PA9 (CN10-21) | USB-UART RX |
| Console RX | PA10 (CN10-33) | USB-UART TX |
| I2C SCL | PB8 (CN10-3) | Pi GPIO3 (pin 5) |
| I2C SDA | PB9 (CN10-5) | Pi GPIO2 (pin 3) |
| GND | any GND | Pi GND (pin 6) and USB-UART GND |

The Pi has 1.8 kΩ pull-ups on GPIO2/3 already, so no extra resistors are
needed. Double-check pin numbers against the NUCLEO-64 user manual (UM1724);
morpho connector numbering is easy to get wrong.

## Bring-up order: one new thing at a time
1. **Zephyr hello world** on the board (`west build -b nucleo_f446re samples/hello_world`).
2. **pmc app without MCUboot.** Build without `--sysbuild`, flash, and check
   the console shows `pmc starting`.
3. **MCTP over the VCP.** On the laptop, run `python3 -m pmctl --dev /dev/ttyACM0 info`.
   * If there's no response, check `pmctl stats` counters through the simulator
     first, then capture raw bytes with `cat /dev/ttyACM0 | xxd`.
4. **Sensors.** Run `pmctl sensors`, then warm the chip with your finger and
   watch the value move.
5. **RAS log across resets.** Press the reset button and confirm with
   `pmctl log` that records survive.
6. **Watchdog.** Add a temporary shell command that spins forever. The IWDG
   resets the board, and a FATAL watchdog record appears after reboot.
7. **MCUboot.** Generate your key, build with `--sysbuild`, flash, then do a
   `pmctl update` with a signed image (see app/README.md).
8. **Rollback.** Update to an image whose `main()` calls `k_panic()` before
   confirming. After the watchdog reset, MCUboot reverts to the old image.
9. **Linux driver.** On the Pi, run `i2cdetect -y 1` and look for 0x42, then
   `make`, `insmod`, add the device, and run `sensors`.
10. **Optional: Linux kernel MCTP.** Attach the kernel's MCTP serial line
    discipline to the VCP (`ldattach` with line discipline N_MCTP), assign
    addresses with the `mctp` tool, and talk to the board through AF_MCTP
    sockets. This proves interoperability with the upstream Linux stack.
