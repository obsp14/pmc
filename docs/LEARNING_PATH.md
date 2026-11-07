# Learning path: making this project yours

The host-side core is complete and tested. The parts that make it *your*
project are the ones that need your hands and your board. Do them, and
you'll be able to defend every line of the resume bullet.

## Week 1: protocols on the host
* Read DSP0236 (MCTP base) sections 8–12 and DSP0253 (serial binding).
  Follow one request byte by byte with `pmc-sim` and `pmctl`. Add a debug print
  of raw frames in `tools/pmctl/pmctl/link.py`.
* Read `core/src/mctp.c`, then break the deframer on purpose (for example,
  remove resync on 0x7E) and watch `test_link_noise_resync` fail.
* **Do it yourself:** implement MCTP `Get Endpoint UUID` (control command
  0x03) in C and Python, with tests.

## Week 2: Zephyr fundamentals + your driver port (the resume bullet)
* Do the Zephyr "basic" tutorials: threads, semaphores, work queues,
  devicetree, Kconfig.
* **Port your bare-metal STM32F446RE drivers into Zephyr as out-of-tree
  drivers** (a `drivers/` folder, `zephyr/module.yml`, a devicetree binding
  YAML, and the `DEVICE_DT_INST_DEFINE` pattern). Start with GPIO and UART.
  Compare your register-level code with Zephyr's `drivers/serial/uart_stm32.c`
  and write down what Zephyr does that you didn't: power management, pinctrl,
  async API. This is the "ported my bare-metal drivers to Zephyr" claim, and
  only you can do it.

## Week 3: firmware on the board
Follow docs/HARDWARE.md steps 1–6. Expect to debug `app/`; it was never
compiled. Keep a log of every bug you hit, because those are interview stories.

## Week 4: MCUboot and A/B update
* Steps 7–8: signed update, then a deliberately broken update that must roll
  back.
* Read MCUboot's design doc on swap-using-scratch and image trailers.
* **Do it yourself:** add an anti-rollback security counter
  (`imgtool --security-counter`) and test that a downgrade is refused.

## Week 5: Linux driver
* Build and load `linux/pmc-hwmon.c` on the Pi, and get `sensors` working.
* Read `Documentation/hwmon/hwmon-kernel-api.rst`.
* **Do it yourself:**
  * Add `temp1_max` (the warning threshold) as a new register end to end:
    firmware register map, then core, then driver.
  * Add an `update_interval` attribute.

## Stretch goals (each one is a strong interview topic)
* **PLDM PDRs.** Implement `GetPDR` with Numeric Sensor PDRs so OpenBMC's
  `pldmd` can discover the sensors without hard-coding.
* **Upstream Linux MCTP.** Talk to the board through the kernel's
  `mctp-serial` and AF_MCTP sockets instead of pmctl (HARDWARE.md step 10).
* **Use libpldm.** Replace the hand-written encoders with OpenBMC's
  `libpldm` on the host side and check that everything still interoperates.
* **Tie it to pqboot.** Have MCUboot verify with LMS or ML-DSA (MCUboot has
  experimental PQ support work), or attest the firmware version via PLDM.

## Interview questions this prepares you for
* When would you choose an RTOS over bare metal? (Use your PLM experience:
  PLM runs its own task scheduler, so where's the line?)
* How does your UART path avoid doing work in the ISR?
* What happens if power fails in the middle of writing a log record, or in
  the middle of a firmware update?
* How does MCUboot decide to roll back?
* Walk me through MCTP packet reassembly. What happens with an out-of-order
  packet?
* Why does the firmware device pull the image instead of the host pushing it?
* How does your hwmon driver avoid reading a half-updated value?
