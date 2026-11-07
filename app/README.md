# Building the Zephyr firmware (UNTESTED)

These files were written against Zephyr 3.7 / 4.x APIs but have **not** been
compiled. Expect a few fixes: Kconfig names, devicetree labels for your Zephyr
version, and flash sizes.

```sh
# one-time: Zephyr SDK + west workspace (https://docs.zephyrproject.org/latest/develop/getting_started/)
west init ~/zephyrproject && cd ~/zephyrproject && west update && west zephyr-export
pip install -r zephyr/scripts/requirements.txt

# without MCUboot first (simplest bring-up)
west build -b nucleo_f446re -p always /path/to/pmc/app -- -DCONFIG_BOOTLOADER_MCUBOOT=n
west flash

# with MCUboot (A/B updates)
pip install imgtool
imgtool keygen -k /path/to/pmc/app/keys/pmc-ecdsa-p256.pem -t ecdsa-p256
west build -b nucleo_f446re -p always --sysbuild /path/to/pmc/app
west flash
```

A signed update image is produced by the build as
`build/pmc/zephyr/zephyr.signed.bin`. Push it with:
```sh
python3 -m pmctl --dev /dev/ttyACM0 update build/pmc/zephyr/zephyr.signed.bin
```

## Likely first errors and where to look
| Error | Fix |
|---|---|
| `DT_N_NODELABEL_die_temp` undefined | Your Zephyr's STM32F4 dtsi names it differently; grep `st,stm32-temp-cal` in `dts/arm/st/f4/` |
| MCUboot doesn't fit in 32 KiB | Give it sectors 0-3 (64 KiB) in the overlay and move `storage` |
| `nvs_mount` fails | `sector_count` must be ≥ 2 and sectors must be equal size: keep storage on sectors 2-3 |
| UART receives nothing | The console must not be on USART2; check `zephyr,console` in `build/zephyr/zephyr.dts` |
| I2C target never addressed | Check `CONFIG_I2C_TARGET=y` and that the STM32 I2C driver supports target mode on your version |

**Never commit `keys/*.pem`.** The `.gitignore` already excludes them.
