/*
 * fw_mcuboot.c - PLDM firmware update backend on MCUboot (UNTESTED)
 *
 *  begin/write : stream the image into the secondary slot (flash_img)
 *  verify      : sanity-check the MCUboot image header here; the real
 *                ECDSA-P256 signature check is done by MCUboot at reboot,
 *                which refuses to swap in an image it cannot verify
 *  apply       : boot_request_upgrade(BOOT_UPGRADE_TEST) - one trial boot
 *  activate    : reboot (after the PLDM response has been sent)
 *
 * After the trial boot the new image must call boot_write_img_confirmed();
 * if it crashes or the watchdog fires first, MCUboot swaps back.
 */
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "pmc_app.h"

LOG_MODULE_REGISTER(pmc_fw, LOG_LEVEL_INF);

#define MCUBOOT_IMAGE_MAGIC 0x96f3b83dU

static struct flash_img_context img;
static uint32_t expected;

static pmc_err_t fw_begin(void *ctx, uint32_t size)
{
	ARG_UNUSED(ctx);
	if (size > FIXED_PARTITION_SIZE(slot1_partition))
		return PMC_E_INVAL;
	if (flash_img_init(&img) != 0)
		return PMC_E_IO;
	expected = size;
	return PMC_OK;
}

static pmc_err_t fw_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
	ARG_UNUSED(ctx);
	/* flash_img writes sequentially; the FD always requests in order */
	if (off != flash_img_bytes_written(&img))
		return PMC_E_INVAL;
	return flash_img_buffered_write(&img, data, len, off + len == expected) == 0
	       ? PMC_OK : PMC_E_IO;
}

static void reboot_work(struct k_work *w)
{
	ARG_UNUSED(w);
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(reboot_dwork, reboot_work);

static pmc_err_t fw_verify(void *ctx)
{
	const struct flash_area *fa;
	uint32_t magic = 0;

	ARG_UNUSED(ctx);
	if (flash_img_bytes_written(&img) != expected)
		return PMC_E_STATE;
	if (flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa))
		return PMC_E_IO;
	flash_area_read(fa, 0, &magic, sizeof(magic));
	flash_area_close(fa);
	if (magic != MCUBOOT_IMAGE_MAGIC) {
		LOG_ERR("not an MCUboot image (magic %08x)", magic);
		return PMC_E_CRC;
	}
	return PMC_OK;
}

static pmc_err_t fw_apply(void *ctx)
{
	ARG_UNUSED(ctx);
	return boot_request_upgrade(BOOT_UPGRADE_TEST) == 0 ? PMC_OK : PMC_E_IO;
}

static pmc_err_t fw_activate(void *ctx)
{
	ARG_UNUSED(ctx);
	/* give the ActivateFirmware response time to leave the UART */
	k_work_schedule(&reboot_dwork, K_MSEC(500));
	return PMC_OK;
}

const pmc_fw_backend pmc_fw_mcuboot = {
	fw_begin, fw_write, fw_verify, fw_apply, fw_activate, NULL
};

void pmc_fw_confirm_if_healthy(pmc_raslog *log)
{
	pmc_ras_record r;

	if (boot_is_img_confirmed())
		return;
	/* "healthy": we got this far (threads alive, no fatal event this boot) */
	if (log && pmc_raslog_get(log, 0, &r) == PMC_OK &&
	    r.severity == RAS_SEV_FATAL && r.boot_count == log->boot_count) {
		LOG_WRN("fatal event in trial image - not confirming, will roll back");
		return;
	}
	if (boot_write_img_confirmed() == 0) {
		LOG_INF("new image confirmed");
		pmc_raslog_add(log, RAS_SRC_FWUP, RAS_SEV_INFO, 0x0100, 0);
	}
}
