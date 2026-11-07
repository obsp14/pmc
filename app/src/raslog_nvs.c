/*
 * raslog_nvs.c - RAS log backend on Zephyr NVS (UNTESTED)
 *
 * STM32F4 flash can only be erased in whole sectors (16-128 KiB), so the
 * core's "write slot N" cannot be done in place. NVS (a log-structured
 * key-value store) handles erase, wear levelling and power-cut safety for
 * us: slot N is stored under NVS id N + 1, the boot counter under id 1000.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

#include "pmc_app.h"

#define RAS_SLOTS     32
#define ID_BOOTCOUNT  1000

static struct nvs_fs fs;

static pmc_err_t nvs_read_slot(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	ssize_t r;

	ARG_UNUSED(ctx);
	r = nvs_read(&fs, (uint16_t)(off / PMC_RAS_REC_SIZE + 1), buf, len);
	if (r == -ENOENT) {
		memset(buf, 0xFF, len);          /* never written == erased */
		return PMC_OK;
	}
	return r == (ssize_t)len ? PMC_OK : PMC_E_IO;
}

static pmc_err_t nvs_write_slot(void *ctx, uint32_t off, const uint8_t *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return nvs_write(&fs, (uint16_t)(off / PMC_RAS_REC_SIZE + 1), buf, len) >= 0
	       ? PMC_OK : PMC_E_IO;
}

int pmc_nvs_backend(pmc_ras_backend *be, uint32_t *boot_count)
{
	struct flash_pages_info info;
	uint32_t boots = 0;

	fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs.flash_device))
		return -ENODEV;
	fs.offset = FIXED_PARTITION_OFFSET(storage_partition);
	if (flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info))
		return -EIO;
	fs.sector_size = (uint16_t)info.size;   /* 16 KiB on sectors 2-3 */
	fs.sector_count = FIXED_PARTITION_SIZE(storage_partition) / info.size;
	if (nvs_mount(&fs))
		return -EIO;

	if (nvs_read(&fs, ID_BOOTCOUNT, &boots, sizeof(boots)) != sizeof(boots))
		boots = 0;
	boots++;
	nvs_write(&fs, ID_BOOTCOUNT, &boots, sizeof(boots));
	*boot_count = boots;

	be->read = nvs_read_slot;
	be->write = nvs_write_slot;
	be->ctx = NULL;
	be->slots = RAS_SLOTS;
	return 0;
}
