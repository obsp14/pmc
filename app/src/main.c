/*
 * main.c - pmc firmware for NUCLEO-F446RE (Zephyr). UNTESTED - see README.
 *
 * Threads:
 *   mctp_thread    (prio 5)  UART bytes -> deframer -> MCTP -> PLDM handlers
 *   monitor_thread (prio 7)  sensors every 500 ms, thresholds, RAS events,
 *                            publishes the I2C register map
 *   main           (prio 0)  boot: reset-cause logging, init, image confirm
 *
 * Every worker thread owns a task-watchdog channel. If a thread stops
 * feeding it (deadlock, infinite loop), the IWDG resets the MCU and the
 * reset cause is logged as a fatal RAS event on the next boot.
 *
 * Concurrency: the PLDM layer reads monitor state and the RAS log, which
 * the monitor thread writes, so both are guarded by pmc_lock.
 */
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "pmc_app.h"

LOG_MODULE_REGISTER(pmc, LOG_LEVEL_INF);

K_MUTEX_DEFINE(pmc_lock);

static pmc_mctp mctp;
static pmc_mctp_deframer deframer;
static pmc_raslog raslog;
static pmc_monitor monitor;
static pmc_pldm pldm;

#define MCTP_STACK 3072
#define MON_STACK  2048

static void log_reset_cause(uint32_t boot)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0)
		return;
	if (cause & RESET_WATCHDOG)
		pmc_raslog_add(&raslog, RAS_SRC_WATCHDOG, RAS_SEV_FATAL, 0x0001, cause);
	else if (cause & RESET_BROWNOUT)
		pmc_raslog_add(&raslog, RAS_SRC_BOOT, RAS_SEV_RECOVERABLE, 0x0002, cause);
	else
		pmc_raslog_add(&raslog, RAS_SRC_BOOT, RAS_SEV_INFO, 0x0001, boot);
	hwinfo_clear_reset_cause();
}

static void mctp_rx_locked(pmc_mctp *m, uint8_t src, uint8_t tag, int to,
			   const uint8_t *msg, size_t len)
{
	k_mutex_lock(&pmc_lock, K_FOREVER);
	pmc_rx_dispatch(m, src, tag, to, msg, len);
	k_mutex_unlock(&pmc_lock);
}

static void mctp_thread(void *a, void *b, void *c)
{
	int wdt = task_wdt_add(2000, NULL, NULL);
	uint8_t buf[64];

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		size_t n = pmc_uart_rx(buf, sizeof(buf), K_MSEC(500));

		for (size_t i = 0; i < n; i++) {
			int plen = pmc_mctp_deframer_feed(&deframer, buf[i]);
			if (plen > 0)
				pmc_mctp_rx_packet(&mctp, deframer.buf, (size_t)plen);
		}
		task_wdt_feed(wdt);
	}
}

static void monitor_thread(void *a, void *b, void *c)
{
	int wdt = task_wdt_add(3000, NULL, NULL);
	uint8_t regs[PMC_REGMAP_SIZE];

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_mutex_lock(&pmc_lock, K_FOREVER);
		pmc_monitor_poll(&monitor);
		pmc_regmap_snapshot(&monitor, regs);
		k_mutex_unlock(&pmc_lock);
		pmc_i2c_publish(regs);
		task_wdt_feed(wdt);
		k_sleep(K_MSEC(500));
	}
}

K_THREAD_STACK_DEFINE(mctp_stack, MCTP_STACK);
K_THREAD_STACK_DEFINE(mon_stack, MON_STACK);
static struct k_thread mctp_tid, mon_tid;

int main(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(iwdg));
	pmc_ras_backend be;
	uint32_t boot = 0;

	LOG_INF("pmc starting");
	if (task_wdt_init(hw_wdt) != 0)
		LOG_ERR("task watchdog init failed");

	if (pmc_nvs_backend(&be, &boot) != 0 || pmc_raslog_init(&raslog, &be, boot) != PMC_OK) {
		LOG_ERR("RAS log unavailable");
	}
	log_reset_cause(boot);

	pmc_monitor_init(&monitor, &raslog);
	pmc_mctp_deframer_init(&deframer);
	pmc_mctp_init(&mctp, 0, pmc_uart_tx, NULL, mctp_rx_locked, &pldm);
	pmc_pldm_init(&pldm, &mctp, &monitor, &raslog, &pmc_fw_mcuboot);
	pldm.link = &deframer;

	if (pmc_uart_init() != 0)
		LOG_ERR("MCTP UART init failed");
	if (pmc_i2c_target_init() != 0)
		LOG_WRN("I2C target unavailable");

	k_thread_create(&mon_tid, mon_stack, MON_STACK, monitor_thread, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_create(&mctp_tid, mctp_stack, MCTP_STACK, mctp_thread, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);

	/* A freshly updated image runs in MCUboot "test" mode. Confirm it only
	 * after it has proved itself; otherwise the next reset rolls back. */
	k_sleep(K_SECONDS(10));
	k_mutex_lock(&pmc_lock, K_FOREVER);
	pmc_fw_confirm_if_healthy(&raslog);
	k_mutex_unlock(&pmc_lock);
	return 0;
}
