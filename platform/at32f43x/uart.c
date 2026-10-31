// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A USART run a byte at a time from its receive interrupt, for a board's simple serial device (a hand microphone): 8N1,
// receive and its interrupt on, transmit on when asked. The board sets its pins and owns the interrupt handler, which
// calls at32_uart_take. The GPS receiver has its own driver (gps_uart/)
#include "platform.h"

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

void at32_uart_start(at32_usart_t *u, uint32_t baud, bool tx)
{
	clock_on(u, true);
	uint32_t pclk = u == USART1 ? at32_apb2_hz() : at32_apb1_hz();
	u->CTRL1 = 0;
	u->BAUDR = (pclk + baud / 2u) / baud;	// the plain clock / baud divider
	u->CTRL2 = 0;
	u->CTRL3 = 0;
	(void)u->STS;
	(void)u->DT;
	u->CTRL1 = USART_CTRL1_UEN | USART_CTRL1_REN | USART_CTRL1_RDBFIEN | (tx ? USART_CTRL1_TEN : 0u);
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
	if (st & (USART_STS_ROERR | USART_STS_FERR | USART_STS_NERR)) {
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
