/*
 * raslog.h - persistent RAS (Reliability, Availability, Serviceability)
 * error log.
 *
 * Fixed-size records in a ring of N slots on a storage backend (a flash
 * partition on the STM32, a file in the simulator). Each record carries a
 * sequence number and a CRC-32, so after a reset or a power cut in the
 * middle of a write we can always find the newest *valid* record and ignore
 * a torn one. Repeated identical errors are coalesced (count++) instead of
 * flooding the log - the same idea as error-record "overflow" counts in
 * Arm RAS and UEFI CPER. The exact count is kept in RAM; a new copy is
 * persisted only when the count reaches 2, 4, 8, ... 128 and 255, so an
 * error storm costs at most 8 flash writes per distinct event (flash wear)
 * while a power cut loses at most half of the count.
 */
#ifndef PMC_RASLOG_H
#define PMC_RASLOG_H

#include "pmc/common.h"

#define PMC_RAS_MAGIC     0x52415331u  /* "RAS1" */
#define PMC_RAS_REC_SIZE  32u

enum pmc_ras_sev { RAS_SEV_INFO = 0, RAS_SEV_CORRECTED = 1, RAS_SEV_RECOVERABLE = 2,
		   RAS_SEV_FATAL = 3 };

enum pmc_ras_src { RAS_SRC_BOOT = 1, RAS_SRC_WATCHDOG, RAS_SRC_SENSOR, RAS_SRC_LINK,
		   RAS_SRC_FWUP, RAS_SRC_TEST };

typedef struct {
	uint32_t seq;          /* monotonically increasing, never reused     */
	uint32_t timestamp_ms; /* uptime at first occurrence (since boot)    */
	uint32_t boot_count;   /* which boot this happened in                */
	uint32_t code;         /* source-specific error code                 */
	uint32_t data;         /* e.g. the sensor value that tripped         */
	uint16_t source;       /* enum pmc_ras_src                           */
	uint8_t severity;      /* enum pmc_ras_sev                           */
	uint8_t count;         /* occurrences coalesced into this record     */
} pmc_ras_record;

/* Storage backend: byte-addressed, record-aligned writes. */
typedef struct {
	pmc_err_t (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
	pmc_err_t (*write)(void *ctx, uint32_t off, const uint8_t *buf, size_t len);
	void *ctx;
	uint32_t slots;        /* number of 32-byte record slots */
} pmc_ras_backend;

typedef struct {
	pmc_ras_backend be;
	uint32_t next_seq;
	uint32_t newest_slot;  /* valid if have_newest */
	int have_newest;
	uint32_t boot_count;
	uint32_t valid;        /* number of valid records found / written */
	uint8_t newest_count;  /* exact count of the newest event (RAM)    */
} pmc_raslog;

/* Scan the backend and recover state. boot_count is supplied by the caller
 * (kept elsewhere, e.g. MCUboot or a settings key). */
pmc_err_t pmc_raslog_init(pmc_raslog *l, const pmc_ras_backend *be, uint32_t boot_count);

/* Record an event (coalesced with the newest record if identical source,
 * code and severity in the same boot). */
pmc_err_t pmc_raslog_add(pmc_raslog *l, uint16_t source, uint8_t severity,
			 uint32_t code, uint32_t data);

/* idx 0 = newest. Returns PMC_E_INVAL past the last valid record. */
pmc_err_t pmc_raslog_get(pmc_raslog *l, uint32_t idx, pmc_ras_record *out);

uint32_t pmc_raslog_count(const pmc_raslog *l);

/* Serialise / parse the 32-byte on-media format (exposed for tests). */
void pmc_ras_encode(const pmc_ras_record *r, uint8_t out[PMC_RAS_REC_SIZE]);
pmc_err_t pmc_ras_decode(const uint8_t in[PMC_RAS_REC_SIZE], pmc_ras_record *r);

#endif
