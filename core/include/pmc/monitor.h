/*
 * monitor.h - sensor polling, threshold evaluation with hysteresis, and
 * the SMBus/I2C register map the Linux hwmon driver reads.
 */
#ifndef PMC_MONITOR_H
#define PMC_MONITOR_H

#include "pmc/raslog.h"

#define PMC_SENSOR_TEMP    1   /* die temperature, 0.1 degC, sint16 */
#define PMC_SENSOR_VOLT    2   /* supply voltage, mV, uint16        */
#define PMC_SENSOR_COUNT   2

/* PLDM numeric sensor states (DSP0248 table "sensor state") */
#define PMC_STATE_UNKNOWN        0
#define PMC_STATE_NORMAL         1
#define PMC_STATE_UPPER_WARNING  8
#define PMC_STATE_UPPER_CRITICAL 9
#define PMC_STATE_LOWER_WARNING  5
#define PMC_STATE_LOWER_CRITICAL 6

typedef struct {
	int32_t warn_hi, crit_hi;     /* upper thresholds            */
	int32_t warn_lo, crit_lo;     /* lower thresholds            */
	int32_t hyst;                 /* hysteresis to leave a state */
} pmc_thresholds;

typedef struct {
	int32_t value;
	uint8_t present, previous;    /* PLDM states */
	int valid;
	uint32_t read_errors;
} pmc_sensor_state;

typedef struct {
	pmc_sensor_state s[PMC_SENSOR_COUNT + 1];   /* index = sensor id */
	pmc_thresholds th[PMC_SENSOR_COUNT + 1];
	pmc_raslog *log;
} pmc_monitor;

/* Platform hook: read a raw sensor value in the units above. */
pmc_err_t pmc_platform_sensor_read(uint8_t id, int32_t *value);

void pmc_monitor_init(pmc_monitor *m, pmc_raslog *log);
/* Read all sensors, update states, log threshold crossings. */
void pmc_monitor_poll(pmc_monitor *m);
const pmc_sensor_state *pmc_monitor_get(const pmc_monitor *m, uint8_t id);

/* ---------------- I2C target register map (for Linux hwmon) -------------
 *   0x00 u8  WHO_AM_I  = 0x5C
 *   0x01 u8  MAP_VERSION = 1
 *   0x02 u8  STATUS    bit0 temp warning, bit1 temp critical,
 *                      bit2 volt out of range, bit3 error log not empty
 *   0x10 s16 TEMP      0.1 degC
 *   0x12 s16 TEMP_CRIT 0.1 degC
 *   0x14 u16 VOLT      mV
 *   0x20 u16 ERR_COUNT records in the RAS log
 *   0x22 u32 UPTIME    seconds
 * Multi-byte registers are little-endian (SMBus word convention).
 */
#define PMC_REG_WHO_AM_I   0x00
#define PMC_REG_MAPVER     0x01
#define PMC_REG_STATUS     0x02
#define PMC_REG_TEMP       0x10
#define PMC_REG_TEMP_CRIT  0x12
#define PMC_REG_VOLT       0x14
#define PMC_REG_ERRCOUNT   0x20
#define PMC_REG_UPTIME     0x22
#define PMC_WHO_AM_I       0x5C
#define PMC_REGMAP_SIZE    0x26

/* Snapshot the register file (the I2C ISR then serves bytes from it). */
void pmc_regmap_snapshot(const pmc_monitor *m, uint8_t regs[PMC_REGMAP_SIZE]);

#endif
