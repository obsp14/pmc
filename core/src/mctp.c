/*
 * mctp.c - serial binding, packetisation/reassembly and control responder
 */
#include "pmc/mctp.h"
#include "pmc/crc.h"

/* ================= serial binding (DSP0253) ================= */
void pmc_mctp_deframer_init(pmc_mctp_deframer *d)
{
	memset(d, 0, sizeof(*d));
	d->st = DF_IDLE;
}

int pmc_mctp_deframer_feed(pmc_mctp_deframer *d, uint8_t c)
{
	/* A flag always (re)starts a frame, whatever state we are in. This is
	 * what lets the receiver resynchronise after line noise. */
	if (c == MCTP_SERIAL_FLAG) {
		if (d->st != DF_IDLE && d->st != DF_END && d->st != DF_REV)
			d->err_framing++;
		d->st = DF_REV;
		d->esc = 0;
		return 0;
	}
	if (d->st == DF_IDLE || d->st == DF_END)
		return 0;                  /* garbage between frames */
	if (c == MCTP_SERIAL_ESC) {
		d->esc = 1;
		return 0;
	}
	if (d->esc) {
		c ^= 0x20;
		d->esc = 0;
	}

	switch (d->st) {
	case DF_REV:
		if (c != MCTP_SERIAL_REV) {
			d->err_rev++;
			d->st = DF_IDLE;
			return 0;
		}
		d->st = DF_LEN;
		break;
	case DF_LEN:
		if (c < 4 || c > MCTP_PKT_MAX) {
			d->err_len++;
			d->st = DF_IDLE;
			return 0;
		}
		d->len = c;
		d->pos = 0;
		d->st = DF_DATA;
		break;
	case DF_DATA:
		d->buf[d->pos++] = c;
		if (d->pos == d->len)
			d->st = DF_FCS1;
		break;
	case DF_FCS1:
		d->fcs_rx = (uint16_t)(c << 8);
		d->st = DF_FCS2;
		break;
	case DF_FCS2: {
		uint8_t hdr[2] = { MCTP_SERIAL_REV, d->len };
		uint16_t fcs = pmc_fcs16(0xFFFF, hdr, 2);

		fcs = pmc_fcs16(fcs, d->buf, d->len);
		d->fcs_rx |= c;
		d->st = DF_END;            /* closing flag is optional for us */
		if (fcs != d->fcs_rx) {
			d->err_fcs++;
			return 0;
		}
		d->frames_ok++;
		return d->len;
	}
	default:
		d->st = DF_IDLE;
	}
	return 0;
}

static int put_escaped(uint8_t *out, size_t outmax, size_t *o, uint8_t c)
{
	if (c == MCTP_SERIAL_FLAG || c == MCTP_SERIAL_ESC) {
		if (*o + 2 > outmax)
			return -1;
		out[(*o)++] = MCTP_SERIAL_ESC;
		out[(*o)++] = (uint8_t)(c ^ 0x20);
	} else {
		if (*o + 1 > outmax)
			return -1;
		out[(*o)++] = c;
	}
	return 0;
}

int pmc_mctp_frame(const uint8_t *pkt, size_t len, uint8_t *out, size_t outmax)
{
	uint8_t hdr[2];
	uint16_t fcs;
	size_t o = 0, i;

	if (len < 4 || len > MCTP_PKT_MAX)
		return PMC_E_INVAL;
	hdr[0] = MCTP_SERIAL_REV;
	hdr[1] = (uint8_t)len;
	fcs = pmc_fcs16(0xFFFF, hdr, 2);
	fcs = pmc_fcs16(fcs, pkt, len);

	if (outmax < 1)
		return PMC_E_NOSPC;
	out[o++] = MCTP_SERIAL_FLAG;
	if (put_escaped(out, outmax, &o, hdr[0]) || put_escaped(out, outmax, &o, hdr[1]))
		return PMC_E_NOSPC;
	for (i = 0; i < len; i++)
		if (put_escaped(out, outmax, &o, pkt[i]))
			return PMC_E_NOSPC;
	if (put_escaped(out, outmax, &o, (uint8_t)(fcs >> 8)) ||
	    put_escaped(out, outmax, &o, (uint8_t)fcs) || o + 1 > outmax)
		return PMC_E_NOSPC;
	out[o++] = MCTP_SERIAL_FLAG;
	return (int)o;
}

/* ================= packet layer (DSP0236) ================= */
void pmc_mctp_init(pmc_mctp *m, uint8_t eid, pmc_mctp_tx_fn tx, void *tx_arg,
		   pmc_mctp_rx_fn rx, void *user)
{
	memset(m, 0, sizeof(*m));
	m->eid = eid;
	m->tx = tx;
	m->tx_arg = tx_arg;
	m->rx = rx;
	m->user = user;
}

uint8_t pmc_mctp_alloc_tag(pmc_mctp *m)
{
	uint8_t t = m->next_tag;
	m->next_tag = (uint8_t)((m->next_tag + 1) & 7);
	return t;
}

void pmc_mctp_rx_packet(pmc_mctp *m, const uint8_t *pkt, size_t len)
{
	uint8_t dest, src, flags, seq, tag, to;
	size_t body = len - 4;

	if (len < 5 || (pkt[0] & 0x0F) != MCTP_HDR_VERSION) {
		m->rx_dropped++;
		return;
	}
	dest = pkt[1];
	src = pkt[2];
	flags = pkt[3];
	seq = (uint8_t)((flags >> 4) & 3);
	to = (flags & MCTP_FLAG_TO) ? 1 : 0;
	tag = flags & 7;

	/* Accept our EID, broadcast, and the null EID (needed before the bus
	 * owner has assigned us one with Set Endpoint ID). */
	if (dest != m->eid && dest != MCTP_EID_BROADCAST && dest != MCTP_EID_NULL) {
		m->rx_dropped++;
		return;
	}

	if (flags & MCTP_FLAG_SOM) {
		m->rasm.active = 1;
		m->rasm.src = src;
		m->rasm.tag = tag;
		m->rasm.to = to;
		m->rasm.len = 0;
		m->rasm.next_seq = seq;
	} else if (!m->rasm.active || src != m->rasm.src || tag != m->rasm.tag ||
		   to != m->rasm.to || seq != m->rasm.next_seq) {
		/* middle/end packet that does not continue the current message */
		m->rasm.active = 0;
		m->rx_dropped++;
		return;
	}
	if (m->rasm.len + body > sizeof(m->rasm.buf)) {
		m->rasm.active = 0;
		m->rx_dropped++;
		return;
	}
	memcpy(m->rasm.buf + m->rasm.len, pkt + 4, body);
	m->rasm.len += body;
	m->rasm.next_seq = (uint8_t)((seq + 1) & 3);

	if (flags & MCTP_FLAG_EOM) {
		m->rasm.active = 0;
		m->rx_msgs++;
		if (m->rx)
			m->rx(m, src, tag, to, m->rasm.buf, m->rasm.len);
	}
}

pmc_err_t pmc_mctp_send(pmc_mctp *m, uint8_t dest, int tag_owner, uint8_t tag,
			const uint8_t *msg, size_t len)
{
	uint8_t pkt[MCTP_PKT_MAX];
	uint8_t frame[2 * (MCTP_PKT_MAX + 4) + 2];
	size_t off = 0;
	uint8_t seq = 0;

	if (len == 0 || len > MCTP_MSG_MAX || !m->tx)
		return PMC_E_INVAL;
	while (off < len) {
		size_t chunk = len - off > MCTP_BTU ? MCTP_BTU : len - off;
		int fl;

		pkt[0] = MCTP_HDR_VERSION;
		pkt[1] = dest;
		pkt[2] = m->eid;
		pkt[3] = (uint8_t)(((off == 0) ? MCTP_FLAG_SOM : 0) |
				   ((off + chunk == len) ? MCTP_FLAG_EOM : 0) |
				   ((seq & 3) << 4) | (tag_owner ? MCTP_FLAG_TO : 0) | (tag & 7));
		memcpy(pkt + 4, msg + off, chunk);
		fl = pmc_mctp_frame(pkt, 4 + chunk, frame, sizeof(frame));
		if (fl < 0)
			return fl;
		m->tx(m->tx_arg, frame, (size_t)fl);
		off += chunk;
		seq++;
	}
	return PMC_OK;
}

/* ================= control protocol (DSP0236 section 12) ================= */
#define CTRL_SET_EID        0x01
#define CTRL_GET_EID        0x02
#define CTRL_GET_VERSION    0x04
#define CTRL_GET_MSG_TYPES  0x05

#define CC_SUCCESS          0x00
#define CC_ERROR_INVALID    0x02
#define CC_UNSUPPORTED      0x05

void pmc_mctp_control_handle(pmc_mctp *m, uint8_t src, uint8_t tag,
			     const uint8_t *msg, size_t len)
{
	/* msg: [0] type 0x00, [1] Rq|D|rsvd|instance, [2] command, data... */
	uint8_t rsp[16];
	size_t rl = 4;

	if (len < 3 || !(msg[1] & 0x80))
		return;                    /* not a request: ignore */
	rsp[0] = MCTP_TYPE_CONTROL;
	rsp[1] = msg[1] & 0x1F;            /* Rq=0, same instance id */
	rsp[2] = msg[2];
	rsp[3] = CC_SUCCESS;

	switch (msg[2]) {
	case CTRL_SET_EID:
		/* data: operation(1) eid(1). We accept "set" (0) and "force" (1). */
		if (len < 5 || (msg[3] & 3) > 1 || msg[4] == MCTP_EID_NULL ||
		    msg[4] == MCTP_EID_BROADCAST) {
			rsp[3] = CC_ERROR_INVALID;
			break;
		}
		m->eid = msg[4];
		rsp[4] = 0x00;             /* EID assignment accepted, no pool */
		rsp[5] = m->eid;
		rsp[6] = 0x00;             /* EID pool size */
		rl = 7;
		break;
	case CTRL_GET_EID:
		rsp[4] = m->eid;
		rsp[5] = 0x00;             /* simple endpoint, dynamic EID */
		rsp[6] = 0x00;             /* medium-specific */
		rl = 7;
		break;
	case CTRL_GET_VERSION:
		/* data: message type number (0xFF = MCTP base spec) */
		if (len < 4) {
			rsp[3] = CC_ERROR_INVALID;
			break;
		}
		if (msg[3] == 0xFF || msg[3] == MCTP_TYPE_CONTROL) {
			rsp[4] = 1;                    /* one version entry */
			rsp[5] = 0xF1; rsp[6] = 0xF3;  /* 1.3.1, BCD with 0xF prefix */
			rsp[7] = 0xF1; rsp[8] = 0x00;
			rl = 9;
		} else if (msg[3] == MCTP_TYPE_PLDM) {
			rsp[4] = 1;
			rsp[5] = 0xF1; rsp[6] = 0xF0;  /* PLDM over MCTP 1.0.0 */
			rsp[7] = 0xF0; rsp[8] = 0x00;
			rl = 9;
		} else {
			rsp[3] = 0x80;                 /* message type not supported */
		}
		break;
	case CTRL_GET_MSG_TYPES:
		rsp[4] = 2;
		rsp[5] = MCTP_TYPE_CONTROL;
		rsp[6] = MCTP_TYPE_PLDM;
		rl = 7;
		break;
	default:
		rsp[3] = CC_UNSUPPORTED;
	}
	pmc_mctp_send(m, src, 0, tag, rsp, rl);
}
