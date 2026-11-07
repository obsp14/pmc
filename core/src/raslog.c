/*
 * raslog.c - crash-safe ring of error records
 *
 * On-media record (32 bytes, little-endian):
 *   0 magic | 4 seq | 8 timestamp | 12 boot_count | 16 code | 20 data |
 *   24 source(u16) | 26 severity | 27 count | 28 crc32(bytes 0..27)
 */
#include "pmc/raslog.h"
#include "pmc/crc.h"

void pmc_ras_encode(const pmc_ras_record *r, uint8_t o[PMC_RAS_REC_SIZE])
{
	pmc_put_le32(o + 0, PMC_RAS_MAGIC);
	pmc_put_le32(o + 4, r->seq);
	pmc_put_le32(o + 8, r->timestamp_ms);
	pmc_put_le32(o + 12, r->boot_count);
	pmc_put_le32(o + 16, r->code);
	pmc_put_le32(o + 20, r->data);
	pmc_put_le16(o + 24, r->source);
	o[26] = r->severity;
	o[27] = r->count;
	pmc_put_le32(o + 28, pmc_crc32(o, 28));
}

pmc_err_t pmc_ras_decode(const uint8_t in[PMC_RAS_REC_SIZE], pmc_ras_record *r)
{
	if (pmc_le32(in) != PMC_RAS_MAGIC)
		return PMC_E_INVAL;          /* erased or never written */
	if (pmc_le32(in + 28) != pmc_crc32(in, 28))
		return PMC_E_CRC;            /* torn write / bit rot */
	r->seq = pmc_le32(in + 4);
	r->timestamp_ms = pmc_le32(in + 8);
	r->boot_count = pmc_le32(in + 12);
	r->code = pmc_le32(in + 16);
	r->data = pmc_le32(in + 20);
	r->source = pmc_le16(in + 24);
	r->severity = in[26];
	r->count = in[27];
	return PMC_OK;
}

pmc_err_t pmc_raslog_init(pmc_raslog *l, const pmc_ras_backend *be, uint32_t boot_count)
{
	uint8_t raw[PMC_RAS_REC_SIZE];
	pmc_ras_record r;
	uint32_t i, best_seq = 0;

	if (!be || !be->read || !be->write || be->slots == 0)
		return PMC_E_INVAL;
	memset(l, 0, sizeof(*l));
	l->be = *be;
	l->boot_count = boot_count;

	for (i = 0; i < be->slots; i++) {
		if (be->read(be->ctx, i * PMC_RAS_REC_SIZE, raw, sizeof(raw)) != PMC_OK)
			return PMC_E_IO;
		if (pmc_ras_decode(raw, &r) != PMC_OK)
			continue;
		l->valid++;
		if (!l->have_newest || r.seq > best_seq) {
			best_seq = r.seq;
			l->newest_slot = i;
			l->have_newest = 1;
		}
	}
	l->next_seq = l->have_newest ? best_seq + 1 : 1;
	if (l->have_newest && pmc_raslog_get(l, 0, &r) == PMC_OK)
		l->newest_count = r.count;
	return PMC_OK;
}

static pmc_err_t write_slot(pmc_raslog *l, uint32_t slot, const pmc_ras_record *r)
{
	uint8_t raw[PMC_RAS_REC_SIZE];
	pmc_ras_encode(r, raw);
	return l->be.write(l->be.ctx, slot * PMC_RAS_REC_SIZE, raw, sizeof(raw));
}

pmc_err_t pmc_raslog_add(pmc_raslog *l, uint16_t source, uint8_t severity,
			 uint32_t code, uint32_t data)
{
	pmc_ras_record r;
	uint32_t slot;

	/* Coalesce with the newest record if it is the same event this boot. */
	if (l->have_newest && pmc_raslog_get(l, 0, &r) == PMC_OK &&
	    r.source == source && r.code == code && r.severity == severity &&
	    r.boot_count == l->boot_count) {
		uint8_t c = l->newest_count;
		if (c == 0xFF)
			return PMC_OK;           /* saturated */
		l->newest_count = ++c;
		if ((c & (c - 1)) != 0 && c != 0xFF)
			return PMC_OK;           /* not a power of two: RAM only */
		/* persist under a NEW sequence number into the NEXT slot: never
		 * overwrite the only good copy in place (a power cut during the
		 * write would lose it) */
		r.count = c;
		r.data = data;
		r.seq = l->next_seq++;
		slot = (l->newest_slot + 1) % l->be.slots;
		if (write_slot(l, slot, &r) != PMC_OK)
			return PMC_E_IO;
		l->newest_slot = slot;
		return PMC_OK;
	}

	memset(&r, 0, sizeof(r));
	r.seq = l->next_seq++;
	r.timestamp_ms = pmc_platform_uptime_ms();
	r.boot_count = l->boot_count;
	r.code = code;
	r.data = data;
	r.source = source;
	r.severity = severity;
	r.count = 1;
	slot = l->have_newest ? (l->newest_slot + 1) % l->be.slots : 0;
	if (write_slot(l, slot, &r) != PMC_OK)
		return PMC_E_IO;
	l->newest_slot = slot;
	l->have_newest = 1;
	l->newest_count = 1;
	if (l->valid < l->be.slots)
		l->valid++;
	return PMC_OK;
}

/*
 * Walk backwards from the newest slot. Records are ordered by sequence
 * number, so stop as soon as a slot is invalid or not strictly older.
 * Coalesced duplicates (same seq lineage) appear once: the copy in the
 * older slot is skipped when the newer record superseded it.
 */
pmc_err_t pmc_raslog_get(pmc_raslog *l, uint32_t idx, pmc_ras_record *out)
{
	uint8_t raw[PMC_RAS_REC_SIZE];
	pmc_ras_record r, prev;
	uint32_t slot, n = 0, steps;
	int have_prev = 0;

	if (!l->have_newest)
		return PMC_E_INVAL;
	slot = l->newest_slot;
	for (steps = 0; steps < l->be.slots; steps++) {
		if (l->be.read(l->be.ctx, slot * PMC_RAS_REC_SIZE, raw, sizeof(raw)) != PMC_OK)
			return PMC_E_IO;
		if (pmc_ras_decode(raw, &r) != PMC_OK)
			return PMC_E_INVAL;
		if (have_prev && r.seq >= prev.seq)
			return PMC_E_INVAL;          /* wrapped into newer data */
		/* skip the superseded copy of a coalesced record */
		if (!(have_prev && r.source == prev.source && r.code == prev.code &&
		      r.severity == prev.severity && r.boot_count == prev.boot_count &&
		      r.timestamp_ms == prev.timestamp_ms && r.count < prev.count)) {
			if (n == idx) {
				*out = r;
				if (idx == 0 && l->newest_count > r.count)
					out->count = l->newest_count;
				return PMC_OK;
			}
			n++;
		}
		prev = r;
		have_prev = 1;
		slot = (slot + l->be.slots - 1) % l->be.slots;
	}
	return PMC_E_INVAL;
}

uint32_t pmc_raslog_count(const pmc_raslog *l)
{
	pmc_raslog tmp = *l;
	pmc_ras_record r;
	uint32_t n = 0;
	while (pmc_raslog_get(&tmp, n, &r) == PMC_OK)
		n++;
	return n;
}
