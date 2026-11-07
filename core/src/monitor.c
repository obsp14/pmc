#include "pmc/monitor.h"

void pmc_monitor_init(pmc_monitor *m, pmc_raslog *log)
{
	memset(m, 0, sizeof(*m));
	m->log = log;
	/* Defaults for an STM32F4 die sensor and a 3.3 V rail. */
	m->th[PMC_SENSOR_TEMP] = (pmc_thresholds){ .warn_hi = 700, .crit_hi = 850,
		.warn_lo = -300, .crit_lo = -400, .hyst = 30 };          /* 0.1 degC */
	m->th[PMC_SENSOR_VOLT] = (pmc_thresholds){ .warn_hi = 3450, .crit_hi = 3600,
		.warn_lo = 3150, .crit_lo = 3000, .hyst = 20 };           /* mV */
	m->s[PMC_SENSOR_TEMP].present = PMC_STATE_UNKNOWN;
	m->s[PMC_SENSOR_VOLT].present = PMC_STATE_UNKNOWN;
}

/*
 * State machine per sensor. Entering a worse state happens at the
 * threshold; returning to a better state requires crossing the threshold
 * minus hysteresis, so a value sitting on the boundary does not flap.
 */
static uint8_t classify(const pmc_thresholds *t, int32_t v, uint8_t cur)
{
	int32_t h = t->hyst;

	if (v >= t->crit_hi)
		return PMC_STATE_UPPER_CRITICAL;
	if (v <= t->crit_lo)
		return PMC_STATE_LOWER_CRITICAL;
	if (cur == PMC_STATE_UPPER_CRITICAL && v > t->crit_hi - h)
		return cur;
	if (cur == PMC_STATE_LOWER_CRITICAL && v < t->crit_lo + h)
		return cur;
	if (v >= t->warn_hi)
		return PMC_STATE_UPPER_WARNING;
	if (v <= t->warn_lo)
		return PMC_STATE_LOWER_WARNING;
	if ((cur == PMC_STATE_UPPER_WARNING || cur == PMC_STATE_UPPER_CRITICAL) &&
	    v > t->warn_hi - h)
		return PMC_STATE_UPPER_WARNING;
	if ((cur == PMC_STATE_LOWER_WARNING || cur == PMC_STATE_LOWER_CRITICAL) &&
	    v < t->warn_lo + h)
		return PMC_STATE_LOWER_WARNING;
	return PMC_STATE_NORMAL;
}

static uint8_t severity_of(uint8_t st)
{
	switch (st) {
	case PMC_STATE_UPPER_CRITICAL:
	case PMC_STATE_LOWER_CRITICAL: return RAS_SEV_RECOVERABLE;
	case PMC_STATE_UPPER_WARNING:
	case PMC_STATE_LOWER_WARNING: return RAS_SEV_CORRECTED;
	default: return RAS_SEV_INFO;
	}
}

void pmc_monitor_poll(pmc_monitor *m)
{
	uint8_t id;

	for (id = 1; id <= PMC_SENSOR_COUNT; id++) {
		pmc_sensor_state *s = &m->s[id];
		int32_t v;
		uint8_t ns;

		if (pmc_platform_sensor_read(id, &v) != PMC_OK) {
			s->read_errors++;
			s->valid = 0;
			if (m->log)
				pmc_raslog_add(m->log, RAS_SRC_SENSOR, RAS_SEV_CORRECTED,
					       0x100u | id, s->read_errors);
			continue;
		}
		s->value = v;
		s->valid = 1;
		ns = classify(&m->th[id], v, s->present);
		if (ns != s->present) {
			s->previous = s->present;
			s->present = ns;
			/* log transitions into an abnormal state (not recoveries) */
			if (ns != PMC_STATE_NORMAL && m->log)
				pmc_raslog_add(m->log, RAS_SRC_SENSOR, severity_of(ns),
					       ((uint32_t)id << 8) | ns, (uint32_t)v);
		}
	}
}

const pmc_sensor_state *pmc_monitor_get(const pmc_monitor *m, uint8_t id)
{
	if (id < 1 || id > PMC_SENSOR_COUNT)
		return NULL;
	return &m->s[id];
}

void pmc_regmap_snapshot(const pmc_monitor *m, uint8_t r[PMC_REGMAP_SIZE])
{
	const pmc_sensor_state *t = &m->s[PMC_SENSOR_TEMP], *v = &m->s[PMC_SENSOR_VOLT];
	uint8_t st = 0;
	uint32_t errs = m->log ? pmc_raslog_count(m->log) : 0;

	memset(r, 0, PMC_REGMAP_SIZE);
	r[PMC_REG_WHO_AM_I] = PMC_WHO_AM_I;
	r[PMC_REG_MAPVER] = 1;
	if (t->present == PMC_STATE_UPPER_WARNING || t->present == PMC_STATE_LOWER_WARNING)
		st |= 0x01;
	if (t->present == PMC_STATE_UPPER_CRITICAL || t->present == PMC_STATE_LOWER_CRITICAL)
		st |= 0x02;
	if (v->present != PMC_STATE_NORMAL && v->present != PMC_STATE_UNKNOWN)
		st |= 0x04;
	if (errs)
		st |= 0x08;
	r[PMC_REG_STATUS] = st;
	pmc_put_le16(r + PMC_REG_TEMP, (uint16_t)(int16_t)t->value);
	pmc_put_le16(r + PMC_REG_TEMP_CRIT, (uint16_t)(int16_t)m->th[PMC_SENSOR_TEMP].crit_hi);
	pmc_put_le16(r + PMC_REG_VOLT, (uint16_t)v->value);
	pmc_put_le16(r + PMC_REG_ERRCOUNT, (uint16_t)(errs > 0xFFFF ? 0xFFFF : errs));
	pmc_put_le32(r + PMC_REG_UPTIME, pmc_platform_uptime_ms() / 1000u);
}
