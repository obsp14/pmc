/*
 * i2c_target.c - SMBus-style register file at address 0x42 (UNTESTED)
 *
 * Protocol (what the Linux driver's i2c_smbus_read_*_data() produces):
 *   START addr+W  reg  RESTART addr+R  data0 data1 ...  STOP
 * The first written byte sets the register pointer; reads auto-increment.
 *
 * The monitor thread publishes a fresh snapshot every 500 ms. The ISR only
 * reads from the "front" buffer, so it never sees a half-updated file.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "pmc_app.h"

#define PMC_I2C_ADDR 0x42

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static uint8_t bufs[2][PMC_REGMAP_SIZE];
static atomic_t front;           /* index of the buffer the ISR reads */
static uint8_t ptr;
static bool ptr_set;

void pmc_i2c_publish(const uint8_t regs[PMC_REGMAP_SIZE])
{
	int back = !atomic_get(&front);
	memcpy(bufs[back], regs, PMC_REGMAP_SIZE);
	atomic_set(&front, back);
}

static uint8_t reg_byte(uint8_t r)
{
	return r < PMC_REGMAP_SIZE ? bufs[atomic_get(&front)][r] : 0xFF;
}

static int t_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	ptr_set = false;
	return 0;
}

static int t_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);
	if (!ptr_set) {
		ptr = val;
		ptr_set = true;
	}
	/* register file is read-only: further written bytes are ignored */
	return 0;
}

static int t_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	*val = reg_byte(ptr);
	return 0;
}

static int t_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	ptr++;
	*val = reg_byte(ptr);
	return 0;
}

static int t_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	return 0;
}

static const struct i2c_target_callbacks cbs = {
	.write_requested = t_write_requested,
	.write_received = t_write_received,
	.read_requested = t_read_requested,
	.read_processed = t_read_processed,
	.stop = t_stop,
};

static struct i2c_target_config tcfg = {
	.address = PMC_I2C_ADDR,
	.callbacks = &cbs,
};

int pmc_i2c_target_init(void)
{
	if (!device_is_ready(i2c))
		return -ENODEV;
	return i2c_target_register(i2c, &tcfg);
}
