// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A link to another SoC on one of the UARTs (platform.h hrc_link_uart_*): the GD-88's two SoCs talk on UART3 at
// 115200 8N1 (docs/spec/hardware-gd88.md 1, docs/spec/rx2-link.md). Receive by interrupt into a ring buffer with a
// semaphore for the reading task; transmit by polling the TX FIFO, with a spin limit so a stalled UART drops the rest
// of a write instead of hanging. Linked only into a build that calls it.
#include "platform.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define UART_RBR	0x00u
#define UART_USR	0x7Cu
#define USR_TFNF	0x02u	// TX FIFO not full

#define RX_SIZE		512u	// power of two: several link frames (at most 103 bytes each)
#define TX_SPIN		200000u	// per byte, about 2 ms at 168 MHz

static uint32_t link_base;
static uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile hrc_link_uart_stats_t stats;
static SemaphoreHandle_t rx_sem;

static void link_rx(uint8_t b, bool flagged)
{
	if (flagged) {
		stats.line_errors++;
	}
	uint32_t next = (rx_head + 1u) & (RX_SIZE - 1u);
	if (next != rx_tail) {
		rx_buf[rx_head] = b;
		rx_head = next;
	} else {
		stats.overruns++;
	}
	stats.rx_bytes++;
}

static void link_isr(void)
{
	BaseType_t woken = pdFALSE;
	if (hrc_uart_rx_isr(link_base, link_rx) != 0 && rx_sem != NULL) {
		xSemaphoreGiveFromISR(rx_sem, &woken);
	}
	portYIELD_FROM_ISR(woken);
}

void hrc_link_uart_init(uint32_t base, uint8_t irq, uint32_t baud)
{
	if (rx_sem == NULL) {
		rx_sem = xSemaphoreCreateBinary();
	}
	link_base = base;
	hrc_uart_setup(base, baud);
	hrc_uart_rx_clear(base);
	rx_head = rx_tail = 0;
	hrc_irq_register(irq, link_isr);
	hrc_uart_rx_irq(base, true);
	hrc_irq_enable(irq, true);
}

size_t hrc_link_uart_read(uint8_t *buf, size_t max)
{
	size_t n = 0;
	while (n < max && rx_tail != rx_head) {
		buf[n++] = rx_buf[rx_tail];
		rx_tail = (rx_tail + 1u) & (RX_SIZE - 1u);
	}
	return n;
}

bool hrc_link_uart_wait(uint32_t timeout_ms)
{
	if (rx_tail != rx_head) {
		return true;
	}
	if (rx_sem != NULL) {
		xSemaphoreTake(rx_sem, pdMS_TO_TICKS(timeout_ms));
	}
	return rx_tail != rx_head;
}

size_t hrc_link_uart_write(const uint8_t *buf, size_t len)
{
	if (link_base == 0) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		uint32_t spins = TX_SPIN;
		while ((REG32(link_base + UART_USR) & USR_TFNF) == 0) {
			if (--spins == 0) {
				stats.tx_stalls++;
				return i;
			}
		}
		REG32(link_base + UART_RBR) = buf[i];
		stats.tx_bytes++;
	}
	return len;
}

void hrc_link_uart_stats(hrc_link_uart_stats_t *st)
{
	st->rx_bytes = stats.rx_bytes;
	st->tx_bytes = stats.tx_bytes;
	st->overruns = stats.overruns;
	st->line_errors = stats.line_errors;
	st->tx_stalls = stats.tx_stalls;
}
