// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A USART for a board's simple serial device (a hand microphone or a Bluetooth module): 8N1, transmit on when asked.
// It receives a byte at a time from its receive interrupt (gd32_uart_start; the board's handler calls gd32_uart_take),
// or by DMA into a circular buffer (gd32_uart_start_dma, USART0 only), which no interrupt latency can overrun. The
// board sets its pins and owns the interrupt handlers. The GPS receiver has its own driver (gps_uart/)
#include "platform.h"

// USART0's receive DMA: DMA0 channel 4, the part's fixed request for USART0_RX (GD32F30x user manual, "DMA").
// The channel registers are 0x14 apart from 0x08; the flags and their clear bits are 4 per channel
#define RX_DMA_BASE		0x40020000u	// DMA0
#define RX_DMA_CH		4u
#define RX_DMA_INTC		GD32_REG(RX_DMA_BASE + 0x04u)
#define RX_DMA_CHCTL		GD32_REG(RX_DMA_BASE + 0x08u + 0x14u * RX_DMA_CH)
#define RX_DMA_CHCNT		GD32_REG(RX_DMA_BASE + 0x0Cu + 0x14u * RX_DMA_CH)
#define RX_DMA_CHPADDR		GD32_REG(RX_DMA_BASE + 0x10u + 0x14u * RX_DMA_CH)
#define RX_DMA_CHMADDR		GD32_REG(RX_DMA_BASE + 0x14u + 0x14u * RX_DMA_CH)
#define DMA_CHCTL_CHEN		(1u << 0)
#define DMA_CHCTL_FTFIE		(1u << 1)
#define DMA_CHCTL_HTFIE		(1u << 2)
#define DMA_CHCTL_CMEN		(1u << 5)	// circular
#define DMA_CHCTL_MNAGA		(1u << 7)	// memory address increments
#define DMA_CHCTL_PRIO_HIGH	(2u << 12)
#define USART_RX_ERRORS		(USART_STAT0_ORERR | USART_STAT0_FERR | USART_STAT0_NERR)

// USART0 is on APB2, the others on APB1
static void clock_on(gd32_usart_t *u, bool on)
{
	volatile uint32_t *en = u == USART0 ? &RCU_APB2EN : &RCU_APB1EN;
	uint32_t bit = u == USART0 ? RCU_APB2EN_USART0EN : u == USART1 ? RCU_APB1EN_USART1EN :
		       u == USART2 ? RCU_APB1EN_USART2EN : u == UART3 ? RCU_APB1EN_UART3EN : RCU_APB1EN_UART4EN;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

static void setup(gd32_usart_t *u, uint32_t baud)
{
	clock_on(u, true);
	uint32_t pclk = u == USART0 ? gd32_apb2_hz() : gd32_apb1_hz();
	u->CTL0 = 0;
	u->BAUD = (pclk + baud / 2u) / baud;	// 16x oversampling: integer and fraction at once
	u->CTL1 = 0;
	u->CTL2 = 0;
	(void)u->STAT0;
	(void)u->DATA;
}

void gd32_uart_start(gd32_usart_t *u, uint32_t baud, bool tx)
{
	setup(u, baud);
	u->CTL0 = USART_CTL0_UEN | USART_CTL0_REN | USART_CTL0_RBNEIE | (tx ? USART_CTL0_TEN : 0u);
}

void gd32_uart_start_dma(gd32_usart_t *u, uint32_t baud, bool tx, uint8_t *ring, uint16_t len)
{
	setup(u, baud);
	RCU_AHBEN |= RCU_AHBEN_DMA0EN;
	(void)RCU_AHBEN;
	RX_DMA_CHCTL = 0;
	RX_DMA_INTC = 0xFu << (4u * RX_DMA_CH);
	RX_DMA_CHPADDR = (uint32_t)(uintptr_t)&u->DATA;
	RX_DMA_CHMADDR = (uint32_t)(uintptr_t)ring;
	RX_DMA_CHCNT = len;
	// Peripheral to memory, 8 bits each side
	RX_DMA_CHCTL = DMA_CHCTL_MNAGA | DMA_CHCTL_CMEN | DMA_CHCTL_HTFIE | DMA_CHCTL_FTFIE | DMA_CHCTL_PRIO_HIGH |
		       DMA_CHCTL_CHEN;
	u->CTL2 = USART_CTL2_DENR | USART_CTL2_ERRIE;
	u->CTL0 = USART_CTL0_UEN | USART_CTL0_REN | (tx ? USART_CTL0_TEN : 0u);
}

uint16_t gd32_uart_dma_pos(uint16_t len)
{
	uint32_t left = RX_DMA_CHCNT;
	return (uint16_t)(left == 0u || left > len ? 0u : len - left);
}

void gd32_uart_dma_ack(void)
{
	RX_DMA_INTC = 0xFu << (4u * RX_DMA_CH);
}

int32_t gd32_uart_dma_error(gd32_usart_t *u, uint16_t len)
{
	uint32_t st = u->STAT0;
	if ((st & USART_RX_ERRORS) == 0) {
		return GD32_UART_NONE;
	}
	// The flags are set with the byte they came with, which the DMA takes a few cycles later: wait for it, so the
	// count holds it. The DMA's read of DATA after the read of STAT0 above clears the flags
	for (uint32_t n = 0; (u->STAT0 & USART_STAT0_RBNE) != 0 && n < 1000u; n++) {
	}
	if ((st & USART_STAT0_RBNE) == 0) {
		(void)u->DATA;	// the DMA had it already: this read clears them. The next byte is a byte time away
	}
	uint16_t pos = gd32_uart_dma_pos(len);
	return pos == 0u ? (int32_t)len - 1 : (int32_t)pos - 1;
}

void gd32_uart_stop(gd32_usart_t *u)
{
	u->CTL0 = 0;
	clock_on(u, false);
}

int32_t gd32_uart_take(gd32_usart_t *u)
{
	uint32_t st = u->STAT0;
	uint8_t c = (uint8_t)u->DATA;	// reading STAT0 then DATA clears RBNE and the error flags
	if (st & USART_RX_ERRORS) {
		return GD32_UART_ERROR;
	}
	return (st & USART_STAT0_RBNE) ? (int32_t)c : GD32_UART_NONE;
}

void gd32_uart_put(gd32_usart_t *u, uint8_t c)
{
	while ((u->STAT0 & USART_STAT0_TBE) == 0) {
	}
	u->DATA = c;
}
