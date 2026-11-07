/* platform_zephyr.c - core platform hooks on Zephyr (UNTESTED) */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include "pmc/monitor.h"

static const struct device *const die_temp = DEVICE_DT_GET(DT_NODELABEL(die_temp));
static const struct device *const vref = DEVICE_DT_GET(DT_NODELABEL(vref));

uint32_t pmc_platform_uptime_ms(void)
{
	return k_uptime_get_32();
}

pmc_err_t pmc_platform_sensor_read(uint8_t id, int32_t *value)
{
	struct sensor_value v;

	if (id == PMC_SENSOR_TEMP) {
		if (!device_is_ready(die_temp) || sensor_sample_fetch(die_temp) ||
		    sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v))
			return PMC_E_IO;
		/* degC -> 0.1 degC */
		*value = v.val1 * 10 + v.val2 / 100000;
		return PMC_OK;
	}
	if (id == PMC_SENSOR_VOLT) {
		/* VREFINT gives the actual VDDA in mV */
		if (!device_is_ready(vref) || sensor_sample_fetch(vref) ||
		    sensor_channel_get(vref, SENSOR_CHAN_VOLTAGE, &v))
			return PMC_E_IO;
		*value = v.val1 * 1000 + v.val2 / 1000;
		return PMC_OK;
	}
	return PMC_E_INVAL;
}
