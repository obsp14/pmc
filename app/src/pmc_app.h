/* pmc_app.h - shared declarations between the Zephyr app modules */
#ifndef PMC_APP_H
#define PMC_APP_H

#include <zephyr/kernel.h>

#include "pmc/pldm.h"

/* mctp_uart.c */
int pmc_uart_init(void);
void pmc_uart_tx(void *arg, const uint8_t *frame, size_t len);
/* blocks until bytes arrive; returns count copied into buf */
size_t pmc_uart_rx(uint8_t *buf, size_t max, k_timeout_t timeout);

/* raslog_nvs.c */
int pmc_nvs_backend(pmc_ras_backend *be, uint32_t *boot_count);

/* fw_mcuboot.c */
extern const pmc_fw_backend pmc_fw_mcuboot;
void pmc_fw_confirm_if_healthy(pmc_raslog *log);

/* i2c_target.c */
int pmc_i2c_target_init(void);
void pmc_i2c_publish(const uint8_t regs[PMC_REGMAP_SIZE]);

#endif
