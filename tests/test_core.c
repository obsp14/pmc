/*
 * test_core.c - host unit tests for the management-controller core.
 * A loopback "wire" connects the device-side pmc_mctp to a test-side
 * pmc_mctp acting as bus owner / update agent.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pmc/crc.h"
#include "pmc/pldm.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; \
	printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- platform hooks for tests ---------------- */
static uint32_t fake_ms;
static int32_t fake_temp = 350, fake_volt = 3300;
static int sensor_fail;
uint32_t pmc_platform_uptime_ms(void) { return fake_ms; }
pmc_err_t pmc_platform_sensor_read(uint8_t id, int32_t *v)
{
	if (sensor_fail)
		return PMC_E_IO;
	*v = id == PMC_SENSOR_TEMP ? fake_temp : fake_volt;
	return id <= 2 ? PMC_OK : PMC_E_INVAL;
}

/* ---------------- RAM flash for the RAS log ---------------- */
#define SLOTS 8
static uint8_t flash[SLOTS * PMC_RAS_REC_SIZE];
static int flash_writes;
static pmc_err_t f_read(void *c, uint32_t o, uint8_t *b, size_t n) { (void)c; memcpy(b, flash + o, n); return PMC_OK; }
static pmc_err_t f_write(void *c, uint32_t o, const uint8_t *b, size_t n)
{ (void)c; memcpy(flash + o, b, n); flash_writes++; return PMC_OK; }
static const pmc_ras_backend BE = { f_read, f_write, NULL, SLOTS };

/* ---------------- loopback wire ---------------- */
static pmc_mctp dev, host;
static pmc_mctp_deframer df_dev, df_host;
static uint8_t host_msg[2048];
static size_t host_msg_len;
static int host_msgs;

static void wire(pmc_mctp_deframer *df, pmc_mctp *dst, const uint8_t *f, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		int l = pmc_mctp_deframer_feed(df, f[i]);
		if (l > 0)
			pmc_mctp_rx_packet(dst, df->buf, (size_t)l);
	}
}
static void dev_tx(void *a, const uint8_t *f, size_t n) { (void)a; wire(&df_host, &host, f, n); }
static void host_tx(void *a, const uint8_t *f, size_t n) { (void)a; wire(&df_dev, &dev, f, n); }

/* Update-agent behaviour: answer the FD's requests from this image. */
static uint8_t ua_image[3000];
static size_t ua_image_len;
static int ua_seen_transfer, ua_seen_verify, ua_seen_apply;
static uint8_t ua_verify_result;

static void host_rx(pmc_mctp *m, uint8_t src, uint8_t tag, int to,
		    const uint8_t *msg, size_t len)
{
	(void)to;
	if (len >= 4 && msg[0] == MCTP_TYPE_PLDM && (msg[1] & 0x80) &&
	    (msg[2] & 0x3F) == PLDM_TYPE_FWUP) {
		uint8_t rsp[5 + 600];
		size_t rl = 5;
		rsp[0] = MCTP_TYPE_PLDM;
		rsp[1] = msg[1] & 0x1F;
		rsp[2] = msg[2];
		rsp[3] = msg[3];
		rsp[4] = 0;
		if (msg[3] == FWUP_REQUEST_FW_DATA) {
			uint32_t off = pmc_le32(msg + 4), n = pmc_le32(msg + 8);
			memcpy(rsp + 5, ua_image + off, n);
			rl += n;
		} else if (msg[3] == FWUP_TRANSFER_COMPLETE) {
			ua_seen_transfer = 1;
		} else if (msg[3] == FWUP_VERIFY_COMPLETE) {
			ua_seen_verify = 1;
			ua_verify_result = msg[4];
		} else if (msg[3] == FWUP_APPLY_COMPLETE) {
			ua_seen_apply = 1;
		}
		pmc_mctp_send(m, src, 0, tag, rsp, rl);
		return;
	}
	memcpy(host_msg, msg, len);
	host_msg_len = len;
	host_msgs++;
}

static void host_req(const uint8_t *msg, size_t len)
{
	host_msg_len = 0;
	pmc_mctp_send(&host, dev.eid ? dev.eid : 0, 1, pmc_mctp_alloc_tag(&host), msg, len);
}

/* ---------------- fw backend (RAM) ---------------- */
static uint8_t slot_b[4096];
static uint32_t slot_size;
static int fw_verify_ok = 1, fw_activated;
static pmc_err_t fb_begin(void *c, uint32_t s) { (void)c; if (s > sizeof(slot_b)) return PMC_E_INVAL; slot_size = s; return PMC_OK; }
static pmc_err_t fb_write(void *c, uint32_t o, const uint8_t *d, size_t n) { (void)c; memcpy(slot_b + o, d, n); return PMC_OK; }
static pmc_err_t fb_verify(void *c) { (void)c; return fw_verify_ok ? PMC_OK : PMC_E_CRC; }
static pmc_err_t fb_apply(void *c) { (void)c; return PMC_OK; }
static pmc_err_t fb_activate(void *c) { (void)c; fw_activated = 1; return PMC_OK; }

/* ================================================================== */
static void test_crc(void)
{
	const uint8_t s[] = "123456789";
	printf("crc\n");
	CHECK(pmc_fcs16(0xFFFF, s, 9) == 0x6F91, "FCS16 (CRC-16/MCRF4XX) check value");
	CHECK(pmc_crc32(s, 9) == 0xCBF43926u, "CRC-32 check value");
}

static void test_serial(void)
{
	uint8_t pkt[68], frame[200];
	pmc_mctp_deframer d;
	int fl, i, got = 0;

	printf("serial framing\n");
	for (i = 0; i < 68; i++)
		pkt[i] = (uint8_t)(i * 37);
	pkt[5] = 0x7E;
	pkt[6] = 0x7D;                          /* force escapes */
	fl = pmc_mctp_frame(pkt, 68, frame, sizeof(frame));
	CHECK(fl > 68 + 6, "framed length %d", fl);
	CHECK(frame[0] == 0x7E && frame[fl - 1] == 0x7E, "flags");
	for (i = 1; i < fl - 1; i++)
		CHECK(frame[i] != 0x7E, "unescaped flag inside frame at %d", i);

	pmc_mctp_deframer_init(&d);
	pmc_mctp_deframer_feed(&d, 0x55);       /* noise before the frame */
	pmc_mctp_deframer_feed(&d, 0x13);
	for (i = 0; i < fl; i++) {
		int r = pmc_mctp_deframer_feed(&d, frame[i]);
		if (r > 0) {
			got = r;
			CHECK(memcmp(d.buf, pkt, 68) == 0, "payload roundtrip");
		}
	}
	CHECK(got == 68, "deframed length %d", got);

	/* corrupt one data byte -> FCS error, no packet */
	frame[10] ^= 0x01;
	got = 0;
	for (i = 0; i < fl; i++)
		got |= pmc_mctp_deframer_feed(&d, frame[i]);
	CHECK(got == 0 && d.err_fcs == 1, "fcs error detected");
	frame[10] ^= 0x01;

	/* truncated frame followed by a good one: receiver resynchronises */
	got = 0;
	for (i = 0; i < 20; i++)
		pmc_mctp_deframer_feed(&d, frame[i]);
	for (i = 0; i < fl; i++)
		got |= pmc_mctp_deframer_feed(&d, frame[i]);
	CHECK(got == 68, "resync after truncated frame");
	CHECK(d.err_framing >= 1, "framing error counted");

	/* bad length byte */
	{
		uint8_t bad[] = { 0x7E, 0x01, 0x02, 0, 0 };
		for (i = 0; i < 5; i++)
			pmc_mctp_deframer_feed(&d, bad[i]);
		CHECK(d.err_len == 1, "length < 4 rejected");
	}
	CHECK(pmc_mctp_frame(pkt, 3, frame, sizeof(frame)) == PMC_E_INVAL, "tiny packet");
	CHECK(pmc_mctp_frame(pkt, 68, frame, 20) == PMC_E_NOSPC, "small output buffer");
}

static pmc_raslog rlog;
static pmc_monitor mon;
static pmc_pldm pl;

static void setup_endpoints(void)
{
	pmc_fw_backend fb = { fb_begin, fb_write, fb_verify, fb_apply, fb_activate, NULL };
	memset(flash, 0xFF, sizeof(flash));
	pmc_raslog_init(&rlog, &BE, 1);
	pmc_monitor_init(&mon, &rlog);
	pmc_mctp_init(&dev, 0, dev_tx, NULL, pmc_rx_dispatch, &pl);
	pmc_mctp_init(&host, 8, host_tx, NULL, host_rx, NULL);
	pmc_mctp_deframer_init(&df_dev);
	pmc_mctp_deframer_init(&df_host);
	pmc_pldm_init(&pl, &dev, &mon, &rlog, &fb);
	pl.link = &df_dev;
}

static void test_mctp_control(void)
{
	uint8_t setid[] = { 0x00, 0x80, 0x01, 0x00, 0x1D };
	uint8_t getid[] = { 0x00, 0x81, 0x02 };
	uint8_t types[] = { 0x00, 0x82, 0x05 };
	uint8_t ver[] = { 0x00, 0x83, 0x04, 0xFF };

	printf("MCTP control\n");
	setup_endpoints();
	host_req(setid, sizeof(setid));         /* sent to EID 0 (unassigned) */
	CHECK(host_msg_len == 7 && host_msg[3] == 0 && host_msg[5] == 0x1D, "Set EID");
	CHECK(dev.eid == 0x1D, "EID assigned");
	host_req(getid, sizeof(getid));
	CHECK(host_msg_len == 7 && host_msg[4] == 0x1D, "Get EID");
	host_req(types, sizeof(types));
	CHECK(host_msg_len == 7 && host_msg[4] == 2 && host_msg[6] == MCTP_TYPE_PLDM, "message types");
	host_req(ver, sizeof(ver));
	CHECK(host_msg_len == 9 && host_msg[5] == 0xF1 && host_msg[6] == 0xF3, "version 1.3.x");
	CHECK(host_msg[1] == 0x03, "response keeps instance id, Rq cleared");

	/* packet for another EID is dropped */
	{
		uint8_t before = (uint8_t)dev.rx_dropped;
		pmc_mctp_send(&host, 0x55, 1, 0, getid, sizeof(getid));
		CHECK(dev.rx_dropped == (uint32_t)before + 1, "foreign EID dropped");
	}
}

static void test_fragmentation(void)
{
	uint8_t big[300];
	uint8_t pkt[8];
	size_t i;

	printf("MCTP fragmentation\n");
	/* 300-byte OEM request -> 5 packets; device answers "unsupported" */
	big[0] = MCTP_TYPE_PLDM;
	big[1] = 0x80;
	big[2] = PLDM_TYPE_OEM;
	big[3] = 0x77;
	for (i = 4; i < sizeof(big); i++)
		big[i] = (uint8_t)i;
	host_req(big, sizeof(big));
	CHECK(host_msg_len == 5 && host_msg[4] == PLDM_ERROR_UNSUPPORTED_CMD, "reassembled 300 B");

	/* EOM packet without SOM is dropped */
	pkt[0] = 1; pkt[1] = dev.eid; pkt[2] = 8; pkt[3] = MCTP_FLAG_EOM | 0x10; pkt[4] = 0;
	{
		uint32_t before = dev.rx_dropped;
		pmc_mctp_rx_packet(&dev, pkt, 5);
		CHECK(dev.rx_dropped == before + 1, "orphan EOM dropped");
	}
}

static void test_pldm_base_platform(void)
{
	uint8_t tid[] = { 0x01, 0x81, 0x00, PLDM_GET_TID };
	uint8_t types[] = { 0x01, 0x82, 0x00, PLDM_GET_TYPES };
	uint8_t gver[] = { 0x01, 0x83, 0x00, PLDM_GET_VERSION, 0, 0, 0, 0, 1, PLDM_TYPE_PLATFORM };
	uint8_t sens[] = { 0x01, 0x84, 0x02, PLDM_GET_SENSOR_READING, 1, 0, 0 };
	uint8_t bad[] = { 0x01, 0x85, 0x02, PLDM_GET_SENSOR_READING, 9, 0, 0 };

	printf("PLDM base + platform\n");
	host_req(tid, sizeof(tid));
	CHECK(host_msg_len == 6 && host_msg[4] == 0 && host_msg[5] == 1, "GetTID");
	host_req(types, sizeof(types));
	CHECK(host_msg_len == 13 && host_msg[5] == 0x25 && host_msg[12] == 0x80, "GetPLDMTypes 0,2,5,63");
	host_req(gver, sizeof(gver));
	CHECK(host_msg_len == 18 && host_msg[10] == 0xF1 && host_msg[11] == 0xF2, "platform version 1.2");
	{
		uint8_t v[4] = { host_msg[10], host_msg[11], host_msg[12], host_msg[13] };
		CHECK(pmc_le32(host_msg + 14) == pmc_crc32(v, 4), "version CRC-32");
	}
	fake_temp = 412;
	pmc_monitor_poll(&mon);
	host_req(sens, sizeof(sens));
	CHECK(host_msg_len == 15 && host_msg[4] == 0, "GetSensorReading ok");
	CHECK((int32_t)pmc_le32(host_msg + 11) == 412, "reading 41.2 degC");
	CHECK(host_msg[8] == PMC_STATE_NORMAL, "state normal");
	host_req(bad, sizeof(bad));
	CHECK(host_msg_len == 5 && host_msg[4] == 0x80, "invalid sensor id");
}

static void test_monitor_and_log(void)
{
	pmc_ras_record r;
	uint8_t regs[PMC_REGMAP_SIZE];
	uint32_t n0;

	printf("monitor thresholds + RAS log\n");
	n0 = pmc_raslog_count(&rlog);
	fake_temp = 720;                         /* >= warn 70.0 */
	pmc_monitor_poll(&mon);
	CHECK(mon.s[1].present == PMC_STATE_UPPER_WARNING, "warning state");
	fake_temp = 690;                         /* below warn but inside hysteresis */
	pmc_monitor_poll(&mon);
	CHECK(mon.s[1].present == PMC_STATE_UPPER_WARNING, "hysteresis holds");
	fake_temp = 660;
	pmc_monitor_poll(&mon);
	CHECK(mon.s[1].present == PMC_STATE_NORMAL, "back to normal");
	fake_temp = 900;
	pmc_monitor_poll(&mon);
	CHECK(mon.s[1].present == PMC_STATE_UPPER_CRITICAL, "critical");
	CHECK(pmc_raslog_count(&rlog) == n0 + 2, "two threshold events logged");
	CHECK(pmc_raslog_get(&rlog, 0, &r) == PMC_OK && r.severity == RAS_SEV_RECOVERABLE &&
	      r.data == 900, "newest = critical, value 900");

	pmc_regmap_snapshot(&mon, regs);
	CHECK(regs[PMC_REG_WHO_AM_I] == 0x5C, "WHO_AM_I");
	CHECK((int16_t)pmc_le16(regs + PMC_REG_TEMP) == 900, "TEMP register");
	CHECK(regs[PMC_REG_STATUS] & 0x02, "STATUS critical bit");
	CHECK(regs[PMC_REG_STATUS] & 0x08, "STATUS log-not-empty bit");

	sensor_fail = 1;
	pmc_monitor_poll(&mon);
	sensor_fail = 0;
	CHECK(!mon.s[1].valid && mon.s[1].read_errors == 1, "sensor read failure tracked");
	fake_temp = 350;
	pmc_monitor_poll(&mon);
}

static void test_raslog(void)
{
	pmc_raslog l, l2;
	pmc_ras_record r;
	int i, w0;

	printf("RAS log persistence\n");
	memset(flash, 0xFF, sizeof(flash));
	CHECK(pmc_raslog_init(&l, &BE, 7) == PMC_OK && pmc_raslog_count(&l) == 0, "empty");
	for (i = 0; i < 3; i++)
		pmc_raslog_add(&l, RAS_SRC_TEST, RAS_SEV_CORRECTED, 100u + (uint32_t)i, 0);
	CHECK(pmc_raslog_count(&l) == 3, "3 records");
	CHECK(pmc_raslog_get(&l, 0, &r) == PMC_OK && r.code == 102 && r.boot_count == 7, "newest first");
	CHECK(pmc_raslog_get(&l, 2, &r) == PMC_OK && r.code == 100, "oldest last");

	/* coalescing: 20 identical events -> one record, count 20, few writes */
	w0 = flash_writes;
	for (i = 0; i < 20; i++)
		pmc_raslog_add(&l, RAS_SRC_LINK, RAS_SEV_CORRECTED, 0x55, (uint32_t)i);
	CHECK(pmc_raslog_count(&l) == 4, "coalesced into one record");
	CHECK(pmc_raslog_get(&l, 0, &r) == PMC_OK && r.count == 20, "count 20 (RAM)");
	CHECK(flash_writes - w0 == 5, "persisted at 1,2,4,8,16 only (%d writes)", flash_writes - w0);

	/* reboot: recover from flash; count is the last persisted power of two */
	CHECK(pmc_raslog_init(&l2, &BE, 8) == PMC_OK, "re-init");
	CHECK(pmc_raslog_count(&l2) == 4, "records survive reset");
	CHECK(pmc_raslog_get(&l2, 0, &r) == PMC_OK && r.count == 16 && r.code == 0x55, "count 16 after reset");
	/* new boot: same error is a new record, not coalesced across boots */
	pmc_raslog_add(&l2, RAS_SRC_LINK, RAS_SEV_CORRECTED, 0x55, 0);
	/* 8 slots: 3 + 5 coalesced copies filled the ring, so this new record
	 * evicted the oldest (code 100): 101, 102, 0x55/boot7, 0x55/boot8 */
	CHECK(pmc_raslog_count(&l2) == 4, "not coalesced across boots, oldest evicted");
	CHECK(pmc_raslog_get(&l2, 0, &r) == PMC_OK && r.boot_count == 8 && r.count == 1,
	      "new boot starts a new record");
	CHECK(pmc_raslog_get(&l2, 3, &r) == PMC_OK && r.code == 101, "oldest is now 101");

	/* torn write: corrupt the newest record's CRC -> previous newest wins */
	flash[l2.newest_slot * PMC_RAS_REC_SIZE + 10] ^= 0xFF;
	CHECK(pmc_raslog_init(&l, &BE, 9) == PMC_OK, "re-init after torn write");
	CHECK(pmc_raslog_get(&l, 0, &r) == PMC_OK && r.boot_count == 7 && r.code == 0x55,
	      "torn record ignored");

	/* wrap-around: ring of 8 slots keeps the 8 newest seq numbers */
	memset(flash, 0xFF, sizeof(flash));
	pmc_raslog_init(&l, &BE, 1);
	for (i = 0; i < 13; i++)
		pmc_raslog_add(&l, RAS_SRC_TEST, RAS_SEV_INFO, (uint32_t)i, 0);
	CHECK(pmc_raslog_count(&l) == SLOTS, "ring holds %d", SLOTS);
	CHECK(pmc_raslog_get(&l, 0, &r) == PMC_OK && r.code == 12, "newest after wrap");
	CHECK(pmc_raslog_get(&l, SLOTS - 1, &r) == PMC_OK && r.code == 5, "oldest after wrap");
	pmc_raslog_init(&l2, &BE, 2);
	CHECK(pmc_raslog_get(&l2, 0, &r) == PMC_OK && r.code == 12, "wrap survives reset");
}

static void test_oem(void)
{
	uint8_t cnt[] = { 0x01, 0x86, 0x3F, PLDM_OEM_ERRLOG_COUNT };
	uint8_t get[] = { 0x01, 0x87, 0x3F, PLDM_OEM_ERRLOG_GET, 0, 0, 0, 0 };
	uint8_t stats[] = { 0x01, 0x88, 0x3F, PLDM_OEM_LINK_STATS };
	pmc_ras_record r;

	printf("PLDM OEM\n");
	setup_endpoints();
	dev.eid = 0x1D;
	pmc_raslog_add(&rlog, RAS_SRC_WATCHDOG, RAS_SEV_FATAL, 0xDEAD, 1);
	host_req(cnt, sizeof(cnt));
	CHECK(host_msg_len == 9 && pmc_le32(host_msg + 5) == 1, "error log count");
	host_req(get, sizeof(get));
	CHECK(host_msg_len == 5 + 32 && pmc_ras_decode(host_msg + 5, &r) == PMC_OK &&
	      r.code == 0xDEAD, "error log entry");
	host_req(stats, sizeof(stats));
	CHECK(host_msg_len == 5 + 24 && pmc_le32(host_msg + 5) >= 3, "link stats");
}

static void fw_req(uint8_t cmd, const uint8_t *data, size_t n)
{
	uint8_t m[64];
	m[0] = MCTP_TYPE_PLDM;
	m[1] = 0x80 | 0x10;
	m[2] = PLDM_TYPE_FWUP;
	m[3] = cmd;
	memcpy(m + 4, data, n);
	host_req(m, 4 + n);
}

static void test_fwup(int good)
{
	uint8_t ru[11] = { 0 }, pct[16] = { 0 }, uc[19] = { 0 }, act[1] = { 1 }, gs[1];
	size_t i;

	printf("PLDM firmware update (%s image)\n", good ? "valid" : "corrupt");
	setup_endpoints();
	dev.eid = 0x1D;
	fw_verify_ok = good;
	fw_activated = 0;
	ua_seen_transfer = ua_seen_verify = ua_seen_apply = 0;
	ua_image_len = 2500;
	for (i = 0; i < ua_image_len; i++)
		ua_image[i] = (uint8_t)(i ^ 0xA5);

	pmc_put_le32(ru, 256);                   /* max transfer size */
	ru[4] = 1;                               /* one component */
	fw_req(FWUP_REQUEST_UPDATE, ru, sizeof(ru));
	CHECK(host_msg[4] == 0 && pl.fw.state == FD_LEARN_COMPONENTS, "RequestUpdate");
	fw_req(FWUP_REQUEST_UPDATE, ru, sizeof(ru));
	CHECK(host_msg[4] == FWUP_CC_ALREADY_IN_UPDATE_MODE, "second RequestUpdate refused");

	pct[0] = 0x05;                           /* start and end */
	fw_req(FWUP_PASS_COMPONENT_TABLE, pct, sizeof(pct));
	CHECK(host_msg[4] == 0 && pl.fw.state == FD_READY_XFER, "PassComponentTable");

	fw_req(FWUP_ACTIVATE_FIRMWARE, act, 1);
	CHECK(host_msg[4] == FWUP_CC_INVALID_STATE, "activate before download refused");

	pmc_put_le32(uc + 9, (uint32_t)ua_image_len);
	fw_req(FWUP_UPDATE_COMPONENT, uc, sizeof(uc));
	/* the whole pull-transfer runs synchronously over the loopback */
	CHECK(ua_seen_transfer, "TransferComplete sent");
	CHECK(slot_size == ua_image_len && memcmp(slot_b, ua_image, ua_image_len) == 0,
	      "image written byte-exact");
	if (good) {
		CHECK(ua_seen_verify && ua_verify_result == 0, "VerifyComplete success");
		CHECK(ua_seen_apply && pl.fw.state == FD_READY_XFER, "ApplyComplete -> READY_XFER");
		fw_req(FWUP_GET_STATUS, gs, 0);
		CHECK(host_msg[5] == FD_READY_XFER && host_msg[9] == 100, "GetStatus 100%%");
		fw_req(FWUP_ACTIVATE_FIRMWARE, act, 1);
		CHECK(host_msg[4] == 0 && fw_activated && pl.fw.state == FD_ACTIVATE, "ActivateFirmware");
	} else {
		CHECK(ua_seen_verify && ua_verify_result == 1, "VerifyComplete failure reported");
		CHECK(!ua_seen_apply && pl.fw.state == FD_IDLE, "back to IDLE, nothing applied");
		CHECK(pmc_raslog_count(&rlog) >= 1, "failed update logged");
		fw_req(FWUP_ACTIVATE_FIRMWARE, act, 1);
		CHECK(!fw_activated, "cannot activate a rejected image");
	}
}

int main(void)
{
	test_crc();
	test_serial();
	test_mctp_control();
	test_fragmentation();
	test_pldm_base_platform();
	test_monitor_and_log();
	test_raslog();
	test_oem();
	test_fwup(1);
	test_fwup(0);
	printf("test_core: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
