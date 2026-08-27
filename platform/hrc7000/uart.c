// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// UART0, the programming cable, as the host port (hal_stream.h): 115200 8N1 (docs/spec/hardware-dm32.md 3.1). Receive
// by interrupt into a ring buffer; transmit by polling the TX FIFO (the host link task is the only writer).
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

#define U(off)		REG32(UART0_BASE + (off))

static uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile uint32_t rx_overruns;
static SemaphoreHandle_t rx_sem;
static bool started;

static void uart_isr(void)
{
	BaseType_t woken = pdFALSE;
	bool any = false;
	for (uint32_t n = 0; n < 64 && (U(UART_LSR) & LSR_DR); n++) {
		uint8_t b = (uint8_t)U(UART_RBR);
		uint32_t next = (rx_head + 1u) & (RX_SIZE - 1u);
		if (next != rx_tail) {
			rx_buf[rx_head] = b;
			rx_head = next;
		} else {
			rx_overruns++;
		}
		any = true;
	}
	(void)U(UART_LSR);	// clears a line status interrupt
	(void)U(UART_USR);	// clears a busy-detect interrupt
	if (any && rx_sem != NULL) {
		xSemaphoreGiveFromISR(rx_sem, &woken);
	}
	portYIELD_FROM_ISR(woken);
}

void hrc_uart0_init(void)
{
	uint32_t div = (HRC_APB_HZ + 8u * BAUD) / (16u * BAUD);	// 46 (45.6): 0.9 % slow
	U(UART_IER) = 0;
	U(UART_LCR) = LCR_DLAB;
	U(UART_RBR) = div & 0xFFu;
	U(UART_IER) = (div >> 8) & 0xFFu;
	U(UART_LCR) = LCR_8N1;
	U(UART_FCR) = 0x67;	// FIFOs on and reset, RX trigger 1 character
}

static bool start(void)
{
	if (started) {
		return true;
	}
	if (rx_sem == NULL) {
		rx_sem = xSemaphoreCreateBinary();
	}
	hrc_uart0_init();
	rx_head = rx_tail = 0;
	hrc_irq_register(IRQ_UART0, uart_isr);
	U(UART_IER) = IER_RX;
	hrc_irq_enable(IRQ_UART0, true);
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
				return i;
			}
		}
		U(UART_RBR) = buf[i];
	}
	return len;
}

uint32_t hrc_uart0_overruns(void)
{
	return rx_overruns;
}

const hal_stream_t hrc_uart0_stream = {
	.start = start,
	.connected = connected,
	.read = rd,
	.wait_rx = wait_rx,
	.write = wr,
};
