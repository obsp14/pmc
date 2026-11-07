/*
 * pldm.c - PLDM responder + firmware-device state machine
 *
 * Field layouts follow DSP0240 / DSP0248 / DSP0267 for the commands
 * implemented, with simplifications marked "SIMPLIFIED". Validate against
 * the specs (or against libpldm's encoders) before claiming conformance.
 */
#include "pmc/pldm.h"
#include "pmc/crc.h"

#define RQ  0x80

/* ---------------- helpers ---------------- */
static void respond(pmc_pldm *p, uint8_t dst, uint8_t tag, const uint8_t *req,
		    uint8_t cc, const uint8_t *payload, size_t plen)
{
	uint8_t out[4 + 1 + 96];

	if (plen > sizeof(out) - 5)
		plen = 0, cc = PLDM_ERROR;
	out[0] = MCTP_TYPE_PLDM;
	out[1] = req[1] & 0x1F;            /* response, same instance id */
	out[2] = req[2];
	out[3] = req[3];
	out[4] = cc;
	if (plen)
		memcpy(out + 5, payload, plen);
	pmc_mctp_send(p->mctp, dst, 0, tag, out, 5 + plen);
}

static void send_request(pmc_pldm *p, uint8_t dst, uint8_t type, uint8_t cmd,
			 const uint8_t *payload, size_t plen)
{
	uint8_t out[4 + 32];
	uint8_t tag = pmc_mctp_alloc_tag(p->mctp);

	out[0] = MCTP_TYPE_PLDM;
	out[1] = (uint8_t)(RQ | (p->next_iid & 0x1F));
	out[2] = type & 0x3F;
	out[3] = cmd;
	memcpy(out + 4, payload, plen);
	p->fw.pending_cmd = cmd;
	p->fw.pending_iid = p->next_iid & 0x1F;
	p->fw.pending_tag = tag;
	p->next_iid = (uint8_t)((p->next_iid + 1) & 0x1F);
	pmc_mctp_send(p->mctp, dst, 1, tag, out, 4 + plen);
}

/* ---------------- type 0: base ---------------- */
static void handle_base(pmc_pldm *p, uint8_t src, uint8_t tag, const uint8_t *m, size_t len)
{
	uint8_t pl[40];

	switch (m[3]) {
	case PLDM_GET_TID:
		pl[0] = p->tid;
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 1);
		break;
	case PLDM_GET_TYPES:
		memset(pl, 0, 8);
		pl[0] = (1u << PLDM_TYPE_BASE) | (1u << PLDM_TYPE_PLATFORM) | (1u << PLDM_TYPE_FWUP);
		pl[7] = 0x80;                       /* type 63 = OEM */
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 8);
		break;
	case PLDM_GET_VERSION: {
		/* req: handle u32, op flag u8, type u8.
		 * rsp: next handle u32, transfer flag u8 (0x05 start+end),
		 *      version u32 (ver32 BCD), crc32 of the version data */
		uint8_t type;
		uint32_t ver, crc;
		uint8_t v[4];

		if (len < 4 + 6) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			break;
		}
		type = m[9];
		if (type == PLDM_TYPE_BASE) ver = 0xF1F1F000u;           /* 1.1.0 */
		else if (type == PLDM_TYPE_PLATFORM) ver = 0xF1F2F000u;  /* 1.2.0 */
		else if (type == PLDM_TYPE_FWUP) ver = 0xF1F1F000u;      /* 1.1.0 */
		else {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_TYPE, NULL, 0);
			break;
		}
		pmc_put_le32(pl, 0);
		pl[4] = 0x05;
		v[0] = (uint8_t)(ver >> 24); v[1] = (uint8_t)(ver >> 16);
		v[2] = (uint8_t)(ver >> 8);  v[3] = (uint8_t)ver;
		memcpy(pl + 5, v, 4);
		crc = pmc_crc32(v, 4);
		pmc_put_le32(pl + 9, crc);
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 13);
		break;
	}
	case PLDM_GET_COMMANDS: {
		uint8_t type;
		if (len < 4 + 5) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			break;
		}
		type = m[4];
		memset(pl, 0, 32);
		if (type == PLDM_TYPE_BASE) {
			pl[0] = (1u << PLDM_GET_TID) | (1u << PLDM_GET_VERSION) |
				(1u << PLDM_GET_TYPES) | (1u << PLDM_GET_COMMANDS);
		} else if (type == PLDM_TYPE_PLATFORM) {
			pl[PLDM_GET_SENSOR_READING / 8] |= (uint8_t)(1u << (PLDM_GET_SENSOR_READING % 8));
		} else if (type == PLDM_TYPE_FWUP) {
			static const uint8_t c[] = { FWUP_REQUEST_UPDATE, FWUP_PASS_COMPONENT_TABLE,
				FWUP_UPDATE_COMPONENT, FWUP_ACTIVATE_FIRMWARE, FWUP_GET_STATUS,
				FWUP_CANCEL_UPDATE };
			size_t i;
			for (i = 0; i < sizeof(c); i++)
				pl[c[i] / 8] |= (uint8_t)(1u << (c[i] % 8));
		} else {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_TYPE, NULL, 0);
			break;
		}
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 32);
		break;
	}
	default:
		respond(p, src, tag, m, PLDM_ERROR_UNSUPPORTED_CMD, NULL, 0);
	}
}

/* ---------------- type 2: platform monitoring ---------------- */
static void handle_platform(pmc_pldm *p, uint8_t src, uint8_t tag, const uint8_t *m, size_t len)
{
	uint8_t pl[16];
	const pmc_sensor_state *s;
	uint16_t id;

	if (m[3] != PLDM_GET_SENSOR_READING) {
		respond(p, src, tag, m, PLDM_ERROR_UNSUPPORTED_CMD, NULL, 0);
		return;
	}
	/* req: sensorID u16, rearmEventState u8 */
	if (len < 4 + 3) {
		respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
		return;
	}
	id = pmc_le16(m + 4);
	s = (id <= 0xFF) ? pmc_monitor_get(p->mon, (uint8_t)id) : NULL;
	if (!s) {
		respond(p, src, tag, m, 0x80 /* INVALID_SENSOR_ID */, NULL, 0);
		return;
	}
	/* rsp: dataSize, operationalState, eventMessageEnable, presentState,
	 *      previousState, eventState, presentReading (sint32 here) */
	pl[0] = 5;                                  /* sint32 */
	pl[1] = s->valid ? 0x00 : 0x02;             /* enabled / unavailable */
	pl[2] = 0x00;                               /* no event generation  */
	pl[3] = s->present;
	pl[4] = s->previous;
	pl[5] = s->present;
	pmc_put_le32(pl + 6, (uint32_t)s->value);
	respond(p, src, tag, m, PLDM_SUCCESS, pl, 10);
}

/* ---------------- type 63: OEM ---------------- */
static void handle_oem(pmc_pldm *p, uint8_t src, uint8_t tag, const uint8_t *m, size_t len)
{
	uint8_t pl[40];

	switch (m[3]) {
	case PLDM_OEM_ERRLOG_COUNT:
		pmc_put_le32(pl, p->log ? pmc_raslog_count(p->log) : 0);
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 4);
		break;
	case PLDM_OEM_ERRLOG_GET: {
		pmc_ras_record r;
		if (len < 8) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			break;
		}
		if (!p->log || pmc_raslog_get(p->log, pmc_le32(m + 4), &r) != PMC_OK) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_DATA, NULL, 0);
			break;
		}
		pmc_ras_encode(&r, pl);
		respond(p, src, tag, m, PLDM_SUCCESS, pl, PMC_RAS_REC_SIZE);
		break;
	}
#ifdef PMC_ALLOW_TEST_INJECT
	case PLDM_OEM_ERRLOG_INJECT:
		/* req: source u16, severity u8, code u32 */
		if (len < 11 || !p->log) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			break;
		}
		pmc_raslog_add(p->log, pmc_le16(m + 4), m[6], pmc_le32(m + 7), 0);
		respond(p, src, tag, m, PLDM_SUCCESS, NULL, 0);
		break;
#endif
	case PLDM_OEM_LINK_STATS:
		memset(pl, 0, 24);
		if (p->link) {
			pmc_put_le32(pl + 0, p->link->frames_ok);
			pmc_put_le32(pl + 4, p->link->err_fcs);
			pmc_put_le32(pl + 8, p->link->err_len);
			pmc_put_le32(pl + 12, p->link->err_framing);
		}
		pmc_put_le32(pl + 16, p->mctp->rx_msgs);
		pmc_put_le32(pl + 20, p->mctp->rx_dropped);
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 24);
		break;
	default:
		respond(p, src, tag, m, PLDM_ERROR_UNSUPPORTED_CMD, NULL, 0);
	}
}

/* ---------------- type 5: firmware update, FD side ---------------- */
static void fw_set_state(pmc_fwup *f, enum pmc_fwup_state s)
{
	f->prev_state = f->state;
	f->state = s;
}

static void fw_fail(pmc_pldm *p, uint8_t reason)
{
	p->fw.reason = reason;
	fw_set_state(&p->fw, FD_IDLE);
	if (p->log)
		pmc_raslog_add(p->log, RAS_SRC_FWUP, RAS_SEV_RECOVERABLE, reason, p->fw.offset);
}

static void fw_request_next(pmc_pldm *p)
{
	pmc_fwup *f = &p->fw;
	uint8_t pl[8];
	uint32_t n = f->image_size - f->offset;

	if (n == 0) {
		/* all data received -> TransferComplete(result = 0 success) */
		uint8_t res = 0;
		send_request(p, f->ua_eid, PLDM_TYPE_FWUP, FWUP_TRANSFER_COMPLETE, &res, 1);
		return;
	}
	if (n > f->chunk)
		n = f->chunk;
	pmc_put_le32(pl, f->offset);
	pmc_put_le32(pl + 4, n);
	send_request(p, f->ua_eid, PLDM_TYPE_FWUP, FWUP_REQUEST_FW_DATA, pl, 8);
}

/* Requests from the Update Agent (UA) */
static void handle_fwup_request(pmc_pldm *p, uint8_t src, uint8_t tag,
				const uint8_t *m, size_t len)
{
	pmc_fwup *f = &p->fw;
	uint8_t pl[16];

	switch (m[3]) {
	case FWUP_REQUEST_UPDATE:
		/* MaxTransferSize u32, NumComponents u16, MaxOutstanding u8,
		 * PackageDataLength u16, VerStrType u8, VerStrLen u8, str */
		if (len < 4 + 11) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			return;
		}
		if (f->state != FD_IDLE) {
			respond(p, src, tag, m, FWUP_CC_ALREADY_IN_UPDATE_MODE, NULL, 0);
			return;
		}
		f->max_xfer = pmc_le32(m + 4);
		if (f->max_xfer < 32) {
			respond(p, src, tag, m, FWUP_CC_INVALID_TRANSFER_LENGTH, NULL, 0);
			return;
		}
		/* one request must fit in one MCTP message with headers */
		f->chunk = f->max_xfer > 512 ? 512 : f->max_xfer;
		f->ua_eid = src;
		f->reason = 0;
		fw_set_state(f, FD_LEARN_COMPONENTS);
		pmc_put_le16(pl, 0);   /* FD metadata length            */
		pl[2] = 0;             /* FD will not send GetPackageData */
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 3);
		return;

	case FWUP_PASS_COMPONENT_TABLE:
		/* SIMPLIFIED: we accept any single component (classification /
		 * identifier are not matched against a device inventory). */
		if (f->state != FD_LEARN_COMPONENTS) {
			respond(p, src, tag, m, FWUP_CC_INVALID_STATE, NULL, 0);
			return;
		}
		if (len < 5) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			return;
		}
		if (m[4] & 0x04)           /* TransferFlag END (0x04) or START_AND_END (0x05) */
			fw_set_state(f, FD_READY_XFER);
		pl[0] = 0;                 /* ComponentResponse: can be updated */
		pl[1] = 0;
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 2);
		return;

	case FWUP_UPDATE_COMPONENT: {
		/* Classification u16, Identifier u16, ClassIndex u8,
		 * ComparisonStamp u32, ImageSize u32, UpdateOptionFlags u32,
		 * VerStrType u8, VerStrLen u8, str */
		uint32_t size;
		if (f->state != FD_READY_XFER) {
			respond(p, src, tag, m, FWUP_CC_INVALID_STATE, NULL, 0);
			return;
		}
		if (len < 4 + 19) {
			respond(p, src, tag, m, PLDM_ERROR_INVALID_LENGTH, NULL, 0);
			return;
		}
		size = pmc_le32(m + 4 + 9);
		if (size == 0 || !f->be.begin || f->be.begin(f->be.ctx, size) != PMC_OK) {
			pl[0] = 1;             /* ComponentCompatibilityResponse: cannot update */
			pl[1] = 0x09;          /* code: component size too large / invalid */
			memset(pl + 2, 0, 6);
			respond(p, src, tag, m, PLDM_SUCCESS, pl, 8);
			return;
		}
		f->image_size = size;
		f->offset = 0;
		fw_set_state(f, FD_DOWNLOAD);
		pl[0] = 0;                 /* can be updated */
		pl[1] = 0;
		pmc_put_le32(pl + 2, 0);   /* UpdateOptionFlagsEnabled */
		pmc_put_le16(pl + 6, 0);   /* TimeBeforeRequestFWData   */
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 8);
		fw_request_next(p);        /* FD now pulls the image */
		return;
	}

	case FWUP_ACTIVATE_FIRMWARE:
		if (f->state != FD_READY_XFER || f->prev_state != FD_APPLY) {
			respond(p, src, tag, m, FWUP_CC_INVALID_STATE, NULL, 0);
			return;
		}
		fw_set_state(f, FD_ACTIVATE);
		pmc_put_le16(pl, 2);       /* EstimatedTimeForActivation (s) */
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 2);
		if (f->be.activate)
			f->be.activate(f->be.ctx);   /* typically reboots */
		return;

	case FWUP_GET_STATUS:
		pl[0] = (uint8_t)f->state;
		pl[1] = (uint8_t)f->prev_state;
		pl[2] = 0;                 /* AuxState */
		pl[3] = 0;                 /* AuxStateStatus */
		pl[4] = (uint8_t)(f->image_size ? (f->offset * 100u) / f->image_size : 0);
		pl[5] = f->reason;
		pmc_put_le32(pl + 6, 0);
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 10);
		return;

	case FWUP_CANCEL_UPDATE:
		if (f->state == FD_IDLE) {
			respond(p, src, tag, m, FWUP_CC_NOT_IN_UPDATE_MODE, NULL, 0);
			return;
		}
		fw_set_state(f, FD_IDLE);
		memset(pl, 0, 9);          /* NonFunctioningComponentIndication + bitmap */
		respond(p, src, tag, m, PLDM_SUCCESS, pl, 9);
		return;

	default:
		respond(p, src, tag, m, PLDM_ERROR_UNSUPPORTED_CMD, NULL, 0);
	}
}

/* Responses from the UA to requests WE sent */
static void handle_fwup_response(pmc_pldm *p, uint8_t src, const uint8_t *m, size_t len)
{
	pmc_fwup *f = &p->fw;
	uint8_t cc;

	if (src != f->ua_eid || m[3] != f->pending_cmd || (m[1] & 0x1F) != f->pending_iid)
		return;                     /* stale or unexpected response */
	f->pending_cmd = 0;
	if (len < 5)
		return;
	cc = m[4];

	switch (m[3]) {
	case FWUP_REQUEST_FW_DATA: {
		size_t n = len - 5;
		uint32_t want = f->image_size - f->offset;
		if (f->state != FD_DOWNLOAD)
			return;
		if (want > f->chunk)
			want = f->chunk;
		if (cc != PLDM_SUCCESS || n != want) {
			fw_fail(p, 0x01);       /* transfer error */
			return;
		}
		if (f->be.write(f->be.ctx, f->offset, m + 5, n) != PMC_OK) {
			fw_fail(p, 0x02);       /* write error */
			return;
		}
		f->offset += (uint32_t)n;
		fw_request_next(p);
		return;
	}
	case FWUP_TRANSFER_COMPLETE: {
		uint8_t res;
		fw_set_state(f, FD_VERIFY);
		res = (f->be.verify && f->be.verify(f->be.ctx) == PMC_OK) ? 0 : 1;
		/* Update our state BEFORE telling the UA: its response may be
		 * processed before send_request() even returns. */
		if (res)
			fw_fail(p, 0x03);       /* verify failed (bad signature) */
		send_request(p, f->ua_eid, PLDM_TYPE_FWUP, FWUP_VERIFY_COMPLETE, &res, 1);
		return;
	}
	case FWUP_VERIFY_COMPLETE: {
		uint8_t pl[3];
		if (f->state != FD_VERIFY)
			return;
		fw_set_state(f, FD_APPLY);
		pl[0] = (f->be.apply && f->be.apply(f->be.ctx) == PMC_OK) ? 0 : 1;
		pmc_put_le16(pl + 1, 0);
		if (pl[0])
			fw_fail(p, 0x04);
		send_request(p, f->ua_eid, PLDM_TYPE_FWUP, FWUP_APPLY_COMPLETE, pl, 3);
		return;
	}
	case FWUP_APPLY_COMPLETE:
		if (f->state == FD_APPLY)
			fw_set_state(f, FD_READY_XFER);   /* await ActivateFirmware */
		return;
	default:
		return;
	}
}

/* ---------------- dispatch ---------------- */
void pmc_pldm_init(pmc_pldm *p, pmc_mctp *mctp, pmc_monitor *mon, pmc_raslog *log,
		   const pmc_fw_backend *fw)
{
	memset(p, 0, sizeof(*p));
	p->mctp = mctp;
	p->mon = mon;
	p->log = log;
	p->tid = 1;
	if (fw)
		p->fw.be = *fw;
	p->fw.state = FD_IDLE;
}

void pmc_pldm_handle(pmc_pldm *p, uint8_t src, uint8_t tag, int tag_owner,
		     const uint8_t *m, size_t len)
{
	uint8_t type;

	if (len < 4)
		return;
	type = m[2] & 0x3F;
	if (!(m[1] & RQ)) {             /* a response */
		if (type == PLDM_TYPE_FWUP && !tag_owner)
			handle_fwup_response(p, src, m, len);
		return;
	}
	switch (type) {
	case PLDM_TYPE_BASE: handle_base(p, src, tag, m, len); break;
	case PLDM_TYPE_PLATFORM: handle_platform(p, src, tag, m, len); break;
	case PLDM_TYPE_FWUP: handle_fwup_request(p, src, tag, m, len); break;
	case PLDM_TYPE_OEM: handle_oem(p, src, tag, m, len); break;
	default: respond(p, src, tag, m, PLDM_ERROR_INVALID_TYPE, NULL, 0);
	}
}

void pmc_rx_dispatch(pmc_mctp *m, uint8_t src, uint8_t tag, int tag_owner,
		     const uint8_t *msg, size_t len)
{
	pmc_pldm *p = (pmc_pldm *)m->user;

	if (len < 1)
		return;
	switch (msg[0] & 0x7F) {
	case MCTP_TYPE_CONTROL:
		pmc_mctp_control_handle(m, src, tag, msg, len);
		break;
	case MCTP_TYPE_PLDM:
		if (p)
			pmc_pldm_handle(p, src, tag, tag_owner, msg, len);
		break;
	default:
		m->rx_dropped++;
	}
}
