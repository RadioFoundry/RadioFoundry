// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The programming cable's UART as the host port (hal_stream.h): 115200 8N1 (docs/spec/hardware-dm32.md 3.1). UART0 on
// the DM-32UV, UART1 on the DB25-D (the board sets HRC_HOST_UART). Receive by interrupt into a ring buffer; transmit by
// polling the TX FIFO (the host link task is the only writer). The set-up and the receive interrupt's loop are shared
// with the boards' other UARTs (hrc_uart_*, the DM-32UV's GPS).
#include "platform.h"
#include "hal_stream.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define UART_RBR	0x00u	// RBR / THR / DLL
#define UART_IER	0x04u	// IER / DLH
#define UART_FCR	0x08u	// IIR (read) / FCR (write)
#define UART_LCR	0x0Cu
#define UART_LSR	0x14u
#define UART_USR	0x7Cu

#define LCR_DLAB	0x80u
#define LCR_8N1		0x03u
#define LSR_DR		0x01u
#define LSR_ERRORS	0x1Eu	// overrun, parity, framing, break
#define USR_TFNF	0x02u	// TX FIFO not full
#define IER_RX		0x05u	// received data + line status

#define BAUD		115200u
#define RX_SIZE		1024u	// power of two; MMDVM frames are at most 255 bytes, CPS writes 4 KB in 32-byte chunks
#define TX_SPIN		200000u	// per byte, about 2 ms at 168 MHz: a stalled UART gives up rather than hangs

#ifndef HRC_HOST_UART
#define HRC_HOST_UART	0
#endif
#if HRC_HOST_UART == 0
#define HOST_BASE	UART0_BASE
#define HOST_IRQ	IRQ_UART0
#elif HRC_HOST_UART == 1
#define HOST_BASE	UART1_BASE
#define HOST_IRQ	IRQ_UART1
#else
#error "HRC_HOST_UART must be 0 or 1"
#endif

#define U(off)		REG32(HOST_BASE + (off))
#define UB(base, off)	REG32((base) + (off))

static uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile uint32_t rx_overruns;	// the ring was full
static volatile uint32_t rx_line_errors;	// the UART flagged a character (FIFO overrun, parity, framing, break)
static volatile uint32_t rx_bytes;
static uint32_t tx_bytes, tx_stalls;	// a stall: the TX FIFO stayed full for TX_SPIN, the rest of the write dropped
static uint32_t waits;			// wait_rx() calls: the host link task's heartbeat (one per pass, 5 ms at most)
static SemaphoreHandle_t rx_sem;
static bool started;

void hrc_uart_setup(uint32_t base, uint32_t baud)
{
	uint32_t div = (HRC_APB_HZ + 8u * baud) / (16u * baud);	// 115200: 46 (45.6), 0.9 % slow; 9600: 547, 0.02 %
	UB(base, UART_IER) = 0;
	UB(base, UART_LCR) = LCR_DLAB;
	UB(base, UART_RBR) = div & 0xFFu;
	UB(base, UART_IER) = (div >> 8) & 0xFFu;
	UB(base, UART_LCR) = LCR_8N1;
	UB(base, UART_FCR) = 0x67;	// FIFOs on and reset, RX trigger 1 character
}

void hrc_uart_rx_irq(uint32_t base, bool on)
{
	UB(base, UART_IER) = on ? IER_RX : 0;
}

uint32_t hrc_uart_rx_isr(uint32_t base, void (*rx)(uint8_t c, bool flagged))
{
	uint32_t n = 0;
	while (n < 64u) {
		uint32_t lsr = UB(base, UART_LSR);	// reading it clears the error bits: one read per character
		if ((lsr & LSR_DR) == 0) {
			break;
		}
		rx((uint8_t)UB(base, UART_RBR), (lsr & LSR_ERRORS) != 0);
		n++;
	}
	(void)UB(base, UART_LSR);	// clears a line status interrupt
	(void)UB(base, UART_USR);	// clears a busy-detect interrupt
	return n;
}

static void host_rx(uint8_t b, bool flagged)
{
	if (flagged) {
		rx_line_errors++;
	}
	uint32_t next = (rx_head + 1u) & (RX_SIZE - 1u);
	if (next != rx_tail) {
		rx_buf[rx_head] = b;
		rx_head = next;
	} else {
		rx_overruns++;
	}
	rx_bytes++;
}

static void uart_isr(void)
{
	BaseType_t woken = pdFALSE;
	if (hrc_uart_rx_isr(HOST_BASE, host_rx) != 0 && rx_sem != NULL) {
		xSemaphoreGiveFromISR(rx_sem, &woken);
	}
	portYIELD_FROM_ISR(woken);
}

void hrc_host_uart_init(void)
{
	hrc_uart_setup(HOST_BASE, BAUD);
}

static bool start(void)
{
	if (started) {
		return true;
	}
	if (rx_sem == NULL) {
		rx_sem = xSemaphoreCreateBinary();
	}
	hrc_host_uart_init();
	rx_head = rx_tail = 0;
	hrc_irq_register(HOST_IRQ, uart_isr);
	hrc_uart_rx_irq(HOST_BASE, true);
	hrc_irq_enable(HOST_IRQ, true);
	started = true;
	return true;
}

static bool connected(void)
{
	return started;	// a UART can't tell whether a cable is there
}

static size_t rd(uint8_t *buf, size_t max)
{
	size_t n = 0;
	while (n < max && rx_tail != rx_head) {
		buf[n++] = rx_buf[rx_tail];
		rx_tail = (rx_tail + 1u) & (RX_SIZE - 1u);
	}
	return n;
}

static bool wait_rx(uint32_t timeout_ms)
{
	waits++;
	if (rx_tail != rx_head) {
		return true;
	}
	if (rx_sem != NULL) {
		xSemaphoreTake(rx_sem, pdMS_TO_TICKS(timeout_ms));
	}
	return rx_tail != rx_head;
}

static size_t wr(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint32_t spins = TX_SPIN;
		while ((U(UART_USR) & USR_TFNF) == 0) {
			if (--spins == 0) {
				tx_stalls++;
				return i;
			}
		}
		U(UART_RBR) = buf[i];
		tx_bytes++;
	}
	return len;
}

uint32_t hrc_host_uart_overruns(void)
{
	return rx_overruns;
}

uint32_t hrc_host_uart_line_errors(void)
{
	return rx_line_errors;
}

void hrc_host_uart_traffic(hrc_uart_traffic_t *t)
{
	t->rx_bytes = rx_bytes;
	t->tx_bytes = tx_bytes;
	t->tx_stalls = tx_stalls;
	t->waits = waits;
	t->ier = (uint8_t)U(UART_IER);
}

const hal_stream_t hrc_host_uart_stream = {
	.start = start,
	.connected = connected,
	.read = rd,
	.wait_rx = wait_rx,
	.write = wr,
};
