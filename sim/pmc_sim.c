/*
 * pmc_sim.c - run the management-controller core on Linux behind a PTY.
 *
 * The PTY behaves like the Nucleo's ST-LINK virtual COM port, so the same
 * host tool (tools/pmctl) talks to the simulator and to the real board.
 *
 *   pmc-sim [--state DIR] [--eid N]
 *     prints "PTY /dev/pts/N" on stdout, then serves MCTP until killed or
 *     until a firmware image is activated (exit code 3, like a reboot).
 *
 * State files in DIR (default ./pmc-state):
 *   raslog.bin     error-log slots (survive restarts, like flash)
 *   bootcount      incremented on every start
 *   temp_override  if present, the temperature sensor reads this value
 *   fw_slot_b.bin  received firmware image ("secondary slot")
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/sha.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "pmc/pldm.h"

#define RAS_SLOTS 64

static int g_pty = -1;
static char g_dir[512] = "pmc-state";
static struct timespec g_t0;
static int g_activated;

/* ---------------- platform hooks ---------------- */
uint32_t pmc_platform_uptime_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((t.tv_sec - g_t0.tv_sec) * 1000 + (t.tv_nsec - g_t0.tv_nsec) / 1000000);
}

static void path(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", g_dir, name);
}

pmc_err_t pmc_platform_sensor_read(uint8_t id, int32_t *value)
{
	double t = pmc_platform_uptime_ms() / 1000.0;
	char p[600];
	FILE *f;

	if (id == PMC_SENSOR_TEMP) {
		path(p, sizeof(p), "temp_override");
		f = fopen(p, "r");
		if (f) {
			int v;
			int ok = fscanf(f, "%d", &v) == 1;
			fclose(f);
			if (ok) {
				*value = v;
				return PMC_OK;
			}
		}
		*value = 350 + (int32_t)(20 * sin(t / 10.0));   /* 33-37 degC */
		return PMC_OK;
	}
	if (id == PMC_SENSOR_VOLT) {
		*value = 3300 + (int32_t)(8 * sin(t / 3.0));
		return PMC_OK;
	}
	return PMC_E_INVAL;
}

/* ---------------- RAS log backend: a file ---------------- */
static pmc_err_t ras_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	int fd = *(int *)ctx;
	return pread(fd, buf, len, off) == (ssize_t)len ? PMC_OK : PMC_E_IO;
}

static pmc_err_t ras_write(void *ctx, uint32_t off, const uint8_t *buf, size_t len)
{
	int fd = *(int *)ctx;
	if (pwrite(fd, buf, len, off) != (ssize_t)len)
		return PMC_E_IO;
	return fsync(fd) == 0 ? PMC_OK : PMC_E_IO;
}

/* ---------------- firmware backend: a file + SHA-256 trailer ---------------- */
/* Simulated image format: payload || SHA-256(payload). On the real board
 * MCUboot verifies an ECDSA signature instead (see app/src/fw_mcuboot.c). */
static struct { int fd; uint32_t size; int verified; } fw = { -1, 0, 0 };

static pmc_err_t fw_begin(void *ctx, uint32_t size)
{
	char p[600];
	(void)ctx;
	if (size < 33 || size > 512 * 1024)
		return PMC_E_INVAL;
	path(p, sizeof(p), "fw_slot_b.bin");
	if (fw.fd >= 0)
		close(fw.fd);
	fw.fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
	fw.size = size;
	fw.verified = 0;
	return fw.fd >= 0 ? PMC_OK : PMC_E_IO;
}

static pmc_err_t fw_write(void *ctx, uint32_t off, const uint8_t *d, size_t n)
{
	(void)ctx;
	if (off + n > fw.size)
		return PMC_E_INVAL;
	return pwrite(fw.fd, d, n, off) == (ssize_t)n ? PMC_OK : PMC_E_IO;
}

static pmc_err_t fw_verify(void *ctx)
{
	uint8_t *buf, md[32];
	(void)ctx;
	buf = malloc(fw.size);
	if (!buf || pread(fw.fd, buf, fw.size, 0) != (ssize_t)fw.size) {
		free(buf);
		return PMC_E_IO;
	}
	SHA256(buf, fw.size - 32, md);
	fw.verified = memcmp(md, buf + fw.size - 32, 32) == 0;
	free(buf);
	fprintf(stderr, "[sim] firmware verify: %s\n", fw.verified ? "OK" : "FAILED");
	return fw.verified ? PMC_OK : PMC_E_CRC;
}

static pmc_err_t fw_apply(void *ctx)
{
	(void)ctx;
	fprintf(stderr, "[sim] image marked for test boot (MCUboot: boot_request_upgrade)\n");
	return fw.verified ? PMC_OK : PMC_E_STATE;
}

static pmc_err_t fw_activate(void *ctx)
{
	(void)ctx;
	fprintf(stderr, "[sim] activate: rebooting into new image\n");
	g_activated = 1;
	return PMC_OK;
}

/* ---------------- transport ---------------- */
static void tx(void *arg, const uint8_t *frame, size_t len)
{
	(void)arg;
	while (len) {
		ssize_t n = write(g_pty, frame, len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return;
		}
		frame += n;
		len -= (size_t)n;
	}
}

static uint32_t bump_bootcount(void)
{
	char p[600];
	FILE *f;
	unsigned v = 0;

	path(p, sizeof(p), "bootcount");
	f = fopen(p, "r");
	if (f) {
		if (fscanf(f, "%u", &v) != 1)
			v = 0;
		fclose(f);
	}
	v++;
	f = fopen(p, "w");
	if (f) {
		fprintf(f, "%u\n", v);
		fclose(f);
	}
	return v;
}

int main(int argc, char **argv)
{
	static pmc_mctp mctp;
	static pmc_mctp_deframer df;
	static pmc_raslog log;
	static pmc_monitor mon;
	static pmc_pldm pldm;
	pmc_ras_backend be;
	pmc_fw_backend fwb = { fw_begin, fw_write, fw_verify, fw_apply, fw_activate, NULL };
	struct termios tio;
	char p[600];
	int ras_fd, i, eid = 0;
	uint32_t last_poll = 0, boot;
	struct stat st;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--state") && i + 1 < argc)
			snprintf(g_dir, sizeof(g_dir), "%s", argv[++i]);
		else if (!strcmp(argv[i], "--eid") && i + 1 < argc)
			eid = atoi(argv[++i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	mkdir(g_dir, 0755);

	/* error log "flash": create erased (0xFF) on first run */
	path(p, sizeof(p), "raslog.bin");
	ras_fd = open(p, O_RDWR | O_CREAT, 0644);
	if (ras_fd < 0 || fstat(ras_fd, &st) != 0)
		return 1;
	if (st.st_size < RAS_SLOTS * PMC_RAS_REC_SIZE) {
		uint8_t ff[PMC_RAS_REC_SIZE];
		memset(ff, 0xFF, sizeof(ff));
		for (i = 0; i < RAS_SLOTS; i++)
			if (pwrite(ras_fd, ff, sizeof(ff), (off_t)i * PMC_RAS_REC_SIZE) != (ssize_t)sizeof(ff))
				return 1;
	}
	be.read = ras_read;
	be.write = ras_write;
	be.ctx = &ras_fd;
	be.slots = RAS_SLOTS;
	boot = bump_bootcount();
	if (pmc_raslog_init(&log, &be, boot) != PMC_OK)
		return 1;
	pmc_raslog_add(&log, RAS_SRC_BOOT, RAS_SEV_INFO, 0x0001, boot);

	pmc_monitor_init(&mon, &log);
	pmc_mctp_init(&mctp, (uint8_t)eid, tx, NULL, pmc_rx_dispatch, &pldm);
	pmc_mctp_deframer_init(&df);
	pmc_pldm_init(&pldm, &mctp, &mon, &log, &fwb);
	pldm.link = &df;

	g_pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (g_pty < 0 || grantpt(g_pty) || unlockpt(g_pty))
		return 1;
	tcgetattr(g_pty, &tio);
	cfmakeraw(&tio);
	tcsetattr(g_pty, TCSANOW, &tio);
	printf("PTY %s\n", ptsname(g_pty));
	fflush(stdout);

	pmc_monitor_poll(&mon);
	while (!g_activated) {
		struct pollfd pfd = { .fd = g_pty, .events = POLLIN };
		uint8_t buf[256];
		int r = poll(&pfd, 1, 100);

		if (r > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = read(g_pty, buf, sizeof(buf));
			ssize_t k;
			for (k = 0; k < n; k++) {
				int plen = pmc_mctp_deframer_feed(&df, buf[k]);
				if (plen > 0)
					pmc_mctp_rx_packet(&mctp, df.buf, (size_t)plen);
			}
		}
		if (pmc_platform_uptime_ms() - last_poll >= 200) {
			last_poll = pmc_platform_uptime_ms();
			pmc_monitor_poll(&mon);
		}
	}
	usleep(200000);      /* let the ActivateFirmware response drain */
	return 3;
}
