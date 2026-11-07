#include "pmc/crc.h"

uint16_t pmc_fcs16(uint16_t fcs, const uint8_t *data, size_t len)
{
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		fcs ^= data[i];
		for (b = 0; b < 8; b++)
			fcs = (uint16_t)((fcs & 1u) ? (fcs >> 1) ^ 0x8408u : fcs >> 1);
	}
	return fcs;
}

uint32_t pmc_crc32(const uint8_t *data, size_t len)
{
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		c ^= data[i];
		for (b = 0; b < 8; b++)
			c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return ~c;
}
