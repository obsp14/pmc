/* crc.h - checksums used by the core */
#ifndef PMC_CRC_H
#define PMC_CRC_H

#include "pmc/common.h"

/*
 * FCS-16 for the MCTP serial binding (DMTF DSP0253): CRC-16/CCITT,
 * reflected polynomial 0x8408, initial value 0xFFFF, no final XOR,
 * transmitted high byte first. This matches the Linux mctp-serial driver
 * (crc_ccitt(0xffff, ...)). Check both against your kernel version.
 */
uint16_t pmc_fcs16(uint16_t fcs, const uint8_t *data, size_t len);

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320) for persisted log records. */
uint32_t pmc_crc32(const uint8_t *data, size_t len);

#endif
