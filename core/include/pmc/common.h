/*
 * pmc - platform management controller core
 * common.h - status codes, byte helpers, platform hooks
 *
 * The core is portable C99 with no dynamic memory and no OS calls. It is
 * compiled into the Zephyr application (app/) and into the host simulator
 * (sim/) used by the tests, so the protocol logic is tested on a laptop
 * before it ever runs on the STM32.
 */
#ifndef PMC_COMMON_H
#define PMC_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int pmc_err_t;
#define PMC_OK          0
#define PMC_E_INVAL    -1   /* malformed input           */
#define PMC_E_NOSPC    -2   /* buffer too small           */
#define PMC_E_STATE    -3   /* not allowed in this state  */
#define PMC_E_IO       -4   /* backend / transport failed */
#define PMC_E_CRC      -5   /* integrity check failed     */
#define PMC_E_NOTSUP   -6   /* unsupported command/type   */
#define PMC_E_AGAIN    -7   /* need more data             */

static inline uint16_t pmc_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t pmc_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void pmc_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void pmc_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Platform hook: monotonic milliseconds (k_uptime_get_32 on Zephyr). */
uint32_t pmc_platform_uptime_ms(void);

#endif
