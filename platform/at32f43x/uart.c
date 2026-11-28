// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A USART for a board's simple serial device (a hand microphone or a Bluetooth module): 8N1, transmit on when asked.
// It receives a byte at a time from its receive interrupt (at32_uart_start; the board's handler calls at32_uart_take),
// or by DMA into a circular buffer (at32_uart_start_dma, USART1 only), which no interrupt latency can overrun. The
// board sets its pins and owns the interrupt handlers. The GPS receiver has its own driver (gps_uart/)
#include "platform.h"

// USART1's receive DMA: DMA1 channel 1 with the flexible mapping table, request 24 (USART1_RX) (AT32F435/437
// reference manual, "DMA controller" and "DMAMUX"). The channel registers are 0x14 apart from 0x08; the flags and their
// clear bits are 4 per channel
#define RX_DMA_BASE		0x40026400u	// DMA1
#define RX_DMA_CLR		AT32_REG(RX_DMA_BASE + 0x04u)
#define RX_DMA_CCTRL		AT32_REG(RX_DMA_BASE + 0x08u)
#define RX_DMA_CDTCNT		AT32_REG(RX_DMA_BASE + 0x0Cu)
#define RX_DMA_CPADDR		AT32_REG(RX_DMA_BASE + 0x10u)
#define RX_DMA_CMADDR		AT32_REG(RX_DMA_BASE + 0x14u)
#define RX_DMA_MUXSEL		AT32_REG(RX_DMA_BASE + 0x100u)
#define RX_DMA_MUXC1CTRL	AT32_REG(RX_DMA_BASE + 0x104u)
#define DMA_MUXSEL_TBL_SEL	(1u << 0)	// the flexible mapping table
#define DMA_REQ_USART1_RX	24u
#define DMA_CCTRL_CHEN		(1u << 0)
#define DMA_CCTRL_FDTIEN	(1u << 1)
#define DMA_CCTRL_HDTIEN	(1u << 2)
#define DMA_CCTRL_LM		(1u << 5)	// circular
#define DMA_CCTRL_MINCM		(1u << 7)	// memory address increments
#define DMA_CCTRL_CHPL_HIGH	(2u << 12)
#define USART_RX_ERRORS		(USART_STS_ROERR | USART_STS_FERR | USART_STS_NERR)

// USART1 is on APB2, the others on APB1
static void clock_on(at32_usart_t *u, bool on)
{
	volatile uint32_t *en = u == USART1 ? &CRM_APB2EN : &CRM_APB1EN;
	uint32_t bit = u == USART1 ? CRM_APB2EN_USART1EN : u == USART2 ? CRM_APB1EN_USART2EN :
		       u == USART3 ? CRM_APB1EN_USART3EN : u == UART4 ? CRM_APB1EN_UART4EN : CRM_APB1EN_UART5EN;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

static void setup(at32_usart_t *u, uint32_t baud)
{
	clock_on(u, true);
	uint32_t pclk = u == USART1 ? at32_apb2_hz() : at32_apb1_hz();
	u->CTRL1 = 0;
	u->BAUDR = (pclk + baud / 2u) / baud;	// the plain clock / baud divider
	u->CTRL2 = 0;
	u->CTRL3 = 0;
	(void)u->STS;
	(void)u->DT;
}

void at32_uart_start(at32_usart_t *u, uint32_t baud, bool tx)
{
	setup(u, baud);
	u->CTRL1 = USART_CTRL1_UEN | USART_CTRL1_REN | USART_CTRL1_RDBFIEN | (tx ? USART_CTRL1_TEN : 0u);
}

void at32_uart_start_dma(at32_usart_t *u, uint32_t baud, bool tx, uint8_t *ring, uint16_t len)
{
	setup(u, baud);
	CRM_AHBEN1 |= CRM_AHBEN1_DMA1EN;
	(void)CRM_AHBEN1;
	RX_DMA_CCTRL = 0;
	RX_DMA_CLR = 0xFu;
	RX_DMA_MUXSEL = DMA_MUXSEL_TBL_SEL;
	RX_DMA_MUXC1CTRL = DMA_REQ_USART1_RX;
	RX_DMA_CPADDR = (uint32_t)(uintptr_t)&u->DT;
	RX_DMA_CMADDR = (uint32_t)(uintptr_t)ring;
	RX_DMA_CDTCNT = len;
	// Peripheral to memory, 8 bits each side
	RX_DMA_CCTRL = DMA_CCTRL_MINCM | DMA_CCTRL_LM | DMA_CCTRL_HDTIEN | DMA_CCTRL_FDTIEN | DMA_CCTRL_CHPL_HIGH |
		       DMA_CCTRL_CHEN;
	u->CTRL3 = USART_CTRL3_DMAREN | USART_CTRL3_ERRIEN;
	u->CTRL1 = USART_CTRL1_UEN | USART_CTRL1_REN | (tx ? USART_CTRL1_TEN : 0u);
}

uint16_t at32_uart_dma_pos(uint16_t len)
{
	uint32_t left = RX_DMA_CDTCNT;
	return (uint16_t)(left == 0u || left > len ? 0u : len - left);
}

void at32_uart_dma_ack(void)
{
	RX_DMA_CLR = 0xFu;
}

int32_t at32_uart_dma_error(at32_usart_t *u, uint16_t len)
{
	uint32_t st = u->STS;
	if ((st & USART_RX_ERRORS) == 0) {
		return AT32_UART_NONE;
	}
	// The flags are set with the byte they came with, which the DMA takes a few cycles later: wait for it, so the
	// count holds it. The DMA's read of DT after the read of STS above clears the flags
	for (uint32_t n = 0; (u->STS & USART_STS_RDBF) != 0 && n < 1000u; n++) {
	}
	if ((st & USART_STS_RDBF) == 0) {
		(void)u->DT;	// the DMA had it already: this read clears them. The next byte is a byte time away
	}
	uint16_t pos = at32_uart_dma_pos(len);
	return pos == 0u ? (int32_t)len - 1 : (int32_t)pos - 1;
}

void at32_uart_stop(at32_usart_t *u)
{
	u->CTRL1 = 0;
	clock_on(u, false);
}

int32_t at32_uart_take(at32_usart_t *u)
{
	uint32_t st = u->STS;
	uint8_t c = (uint8_t)u->DT;	// reading STS then DT clears RDBF and the error flags
	if (st & USART_RX_ERRORS) {
		return AT32_UART_ERROR;
	}
	return (st & USART_STS_RDBF) ? (int32_t)c : AT32_UART_NONE;
}

void at32_uart_put(at32_usart_t *u, uint8_t c)
{
	while ((u->STS & USART_STS_TDBE) == 0) {
	}
	u->DT = c;
}
