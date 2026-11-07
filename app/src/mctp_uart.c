/*
 * mctp_uart.c - interrupt-driven UART for the MCTP serial binding (UNTESTED)
 *
 * RX: the ISR moves bytes from the UART FIFO into a ring buffer and gives a
 * semaphore; the MCTP thread drains it. Nothing protocol-related happens
 * in interrupt context.
 * TX: polled output - at 115200 baud a 140-byte frame takes ~12 ms, which
 * is fine for a management link. Switch to async/DMA for higher rates.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include "pmc_app.h"

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(pmc_mctp_uart));
RING_BUF_DECLARE(rx_ring, 512);
static K_SEM_DEFINE(rx_sem, 0, 1);
static uint32_t rx_overruns;

static void uart_isr(const struct device *dev, void *user)
{
	uint8_t buf[32];

	ARG_UNUSED(user);
	if (!uart_irq_update(dev))
		return;
	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0)
			break;
		if (ring_buf_put(&rx_ring, buf, (uint32_t)n) < (uint32_t)n)
			rx_overruns++;           /* thread too slow: frame will fail FCS */
		k_sem_give(&rx_sem);
	}
}

int pmc_uart_init(void)
{
	if (!device_is_ready(uart))
		return -ENODEV;
	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);
	return 0;
}

void pmc_uart_tx(void *arg, const uint8_t *frame, size_t len)
{
	ARG_UNUSED(arg);
	for (size_t i = 0; i < len; i++)
		uart_poll_out(uart, frame[i]);
}

size_t pmc_uart_rx(uint8_t *buf, size_t max, k_timeout_t timeout)
{
	uint32_t n = ring_buf_get(&rx_ring, buf, (uint32_t)max);

	if (n)
		return n;
	if (k_sem_take(&rx_sem, timeout) != 0)
		return 0;
	return ring_buf_get(&rx_ring, buf, (uint32_t)max);
}
