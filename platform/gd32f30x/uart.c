// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A USART run a byte at a time from its receive interrupt, for a board's simple serial device (a hand microphone): 8N1,
// receive and its interrupt on, transmit on when asked. The board sets its pins and owns the interrupt handler, which
// calls gd32_uart_take. The GPS receiver has its own driver (gps_uart/)
#include "platform.h"

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

void gd32_uart_start(gd32_usart_t *u, uint32_t baud, bool tx)
{
	clock_on(u, true);
	uint32_t pclk = u == USART0 ? gd32_apb2_hz() : gd32_apb1_hz();
	u->CTL0 = 0;
	u->BAUD = (pclk + baud / 2u) / baud;	// 16x oversampling: integer and fraction at once
	u->CTL1 = 0;
	u->CTL2 = 0;
	(void)u->STAT0;
	(void)u->DATA;
	u->CTL0 = USART_CTL0_UEN | USART_CTL0_REN | USART_CTL0_RBNEIE | (tx ? USART_CTL0_TEN : 0u);
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
	if (st & (USART_STAT0_ORERR | USART_STAT0_FERR | USART_STAT0_NERR)) {
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
