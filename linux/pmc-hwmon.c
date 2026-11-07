// SPDX-License-Identifier: GPL-2.0
/*
 * pmc-hwmon.c - Linux hwmon driver for the pmc management controller
 * (STM32 running pmc firmware, I2C target at 0x42).
 *
 * STATUS: UNTESTED - not compiled in this repository (no kernel headers in
 * CI). Build on the Raspberry Pi with the matching kernel headers:
 *     make -C /lib/modules/$(uname -r)/build M=$PWD modules
 *
 * Exposes through the standard hwmon sysfs ABI:
 *   temp1_input        die temperature, millidegrees C
 *   temp1_crit         critical threshold, millidegrees C
 *   temp1_crit_alarm   1 while the controller reports "critical"
 *   temp1_max_alarm    1 while the controller reports "warning"
 *   in0_input          supply voltage, mV
 *   in0_alarm          1 while the voltage is out of range
 *   error_count        records in the controller's persistent RAS log
 * so `sensors` (lm-sensors) shows the board with no extra tooling.
 */
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define PMC_REG_WHO_AM_I  0x00
#define PMC_REG_MAPVER    0x01
#define PMC_REG_STATUS    0x02
#define PMC_REG_TEMP      0x10
#define PMC_REG_TEMP_CRIT 0x12
#define PMC_REG_VOLT      0x14
#define PMC_REG_ERRCOUNT  0x20
#define PMC_WHO_AM_I      0x5C

#define ST_TEMP_WARN  BIT(0)
#define ST_TEMP_CRIT  BIT(1)
#define ST_VOLT_RANGE BIT(2)

struct pmc_data {
	struct i2c_client *client;
	struct mutex lock;       /* serialises multi-register reads */
};

/* SMBus words are little-endian, matching the firmware's register file. */
static int pmc_read_s16(struct pmc_data *d, u8 reg, long *val)
{
	int r = i2c_smbus_read_word_data(d->client, reg);

	if (r < 0)
		return r;
	*val = (s16)r;
	return 0;
}

static int pmc_read_status_bit(struct pmc_data *d, u8 bit, long *val)
{
	int r = i2c_smbus_read_byte_data(d->client, PMC_REG_STATUS);

	if (r < 0)
		return r;
	*val = !!(r & bit);
	return 0;
}

static int pmc_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		    int channel, long *val)
{
	struct pmc_data *d = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;

	mutex_lock(&d->lock);
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			ret = pmc_read_s16(d, PMC_REG_TEMP, val);
			*val *= 100;               /* 0.1 degC -> millidegrees */
			break;
		case hwmon_temp_crit:
			ret = pmc_read_s16(d, PMC_REG_TEMP_CRIT, val);
			*val *= 100;
			break;
		case hwmon_temp_crit_alarm:
			ret = pmc_read_status_bit(d, ST_TEMP_CRIT, val);
			break;
		case hwmon_temp_max_alarm:
			ret = pmc_read_status_bit(d, ST_TEMP_WARN, val);
			break;
		}
		break;
	case hwmon_in:
		if (attr == hwmon_in_input) {
			int r = i2c_smbus_read_word_data(d->client, PMC_REG_VOLT);
			ret = r < 0 ? r : 0;
			*val = r;                  /* already mV */
		} else if (attr == hwmon_in_alarm) {
			ret = pmc_read_status_bit(d, ST_VOLT_RANGE, val);
		}
		break;
	default:
		break;
	}
	mutex_unlock(&d->lock);
	return ret;
}

static umode_t pmc_is_visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	return 0444;
}

static const struct hwmon_channel_info *const pmc_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_CRIT |
			   HWMON_T_CRIT_ALARM | HWMON_T_MAX_ALARM),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_ALARM),
	NULL
};

static const struct hwmon_ops pmc_ops = {
	.is_visible = pmc_is_visible,
	.read = pmc_read,
};

static const struct hwmon_chip_info pmc_chip = {
	.ops = &pmc_ops,
	.info = pmc_info,
};

/* Non-standard attribute: number of records in the persistent error log */
static ssize_t error_count_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct pmc_data *d = dev_get_drvdata(dev);
	int r = i2c_smbus_read_word_data(d->client, PMC_REG_ERRCOUNT);

	if (r < 0)
		return r;
	return sysfs_emit(buf, "%d\n", r);
}
static DEVICE_ATTR_RO(error_count);

static struct attribute *pmc_attrs[] = {
	&dev_attr_error_count.attr,
	NULL
};
ATTRIBUTE_GROUPS(pmc);

static int pmc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon;
	struct pmc_data *d;
	int id, ver;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	id = i2c_smbus_read_byte_data(client, PMC_REG_WHO_AM_I);
	if (id < 0)
		return dev_err_probe(dev, id, "no response at 0x%02x\n", client->addr);
	if (id != PMC_WHO_AM_I)
		return dev_err_probe(dev, -ENODEV, "unexpected WHO_AM_I 0x%02x\n", id);
	ver = i2c_smbus_read_byte_data(client, PMC_REG_MAPVER);

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	mutex_init(&d->lock);

	hwmon = devm_hwmon_device_register_with_info(dev, "pmc", d, &pmc_chip, pmc_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	dev_info(dev, "pmc management controller, register map v%d\n", ver);
	return 0;
}

static const struct i2c_device_id pmc_id[] = {
	{ "pmc-hwmon" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pmc_id);

static const struct of_device_id pmc_of_match[] = {
	{ .compatible = "praveen,pmc-hwmon" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmc_of_match);

static struct i2c_driver pmc_driver = {
	.driver = {
		.name = "pmc-hwmon",
		.of_match_table = pmc_of_match,
	},
	.probe = pmc_probe,
	.id_table = pmc_id,
};
module_i2c_driver(pmc_driver);

MODULE_AUTHOR("Ommi Bala Srinivas Praveen");
MODULE_DESCRIPTION("hwmon driver for the pmc STM32 management controller");
MODULE_LICENSE("GPL");
