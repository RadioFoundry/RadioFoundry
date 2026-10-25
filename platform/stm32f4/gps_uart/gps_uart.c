// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS NMEA on a receive-only STM32F4 USART (gps_uart.h; docs/spec/hardware-md380.md 13, hardware-md9600.md 13)
#include <stddef.h>
#include "gps_uart/gps_uart.h"
#include "config.h"
#include "hal_time.h"
#include "gps/gps_rx/gps_rx.h"
#include "gps/gps_rx/gps_baud.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif

#define GPS_TASK_STACK		384	// words: the APRS NMEA parser runs on it
#define GPS_TASK_PRIORITY	1
#define GPS_POLL_MS		20
#define GPS_IRQ_PRIORITY	7	// numerically >= 5: may not call FreeRTOS, doesn't need to
#define NO_TIMEOUT		0xFFFFFFFFu	// gps_rx never gives up on its own: the rate hunt decides

static const stm32_gps_cfg_t *cfg;
static gps_rx_t rx;
static gps_baud_t hunt;
static volatile bool present;
static volatile bool enabled = true;
static volatile uint32_t baud;
static TaskHandle_t task;

void stm32_gps_irq(void)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t sr = u->SR;
	uint8_t c = (uint8_t)u->DR;	// reading SR then DR clears RXNE and the error flags
	if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
		gps_rx_error(&rx);
		return;
	}
	if (sr & USART_SR_RXNE) {
		gps_rx_byte(&rx, c);
	}
}

static bool on_apb2(const USART_TypeDef *u)
{
	return u == USART1 || u == USART6;
}

static void clock_on(bool on)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t bit = u == USART1 ? RCC_APB2ENR_USART1EN : u == USART6 ? RCC_APB2ENR_USART6EN :
		       u == USART2 ? RCC_APB1ENR_USART2EN : u == USART3 ? RCC_APB1ENR_USART3EN :
		       u == UART4 ? RCC_APB1ENR_UART4EN : RCC_APB1ENR_UART5EN;
	volatile uint32_t *en = on_apb2(u) ? &RCC->APB2ENR : &RCC->APB1ENR;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

static void uart_rate(uint32_t rate)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t pclk = on_apb2(u) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
	u->CR1 = 0;
	u->BRR = (pclk + rate / 2u) / rate;	// oversampling by 16
	u->CR2 = 0;
	u->CR3 = 0;
	(void)u->SR;
	(void)u->DR;
	u->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_RXNEIE;
	baud = rate;
}

static void uart_on(bool on)
{
	if (on) {
		clock_on(true);
		stm32_pin_af(cfg->rx_port, cfg->rx_pin, cfg->rx_af, false, GPIO_PULLUP);
		uart_rate(gps_baud_rate(&hunt));
		HAL_NVIC_SetPriority(cfg->irq, GPS_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(cfg->irq);
	} else {
		HAL_NVIC_DisableIRQ(cfg->irq);
		cfg->uart->CR1 = 0;
		clock_on(false);
		stm32_pin_in(cfg->rx_port, cfg->rx_pin, GPIO_NOPULL);
		baud = 0;
	}
}

// Characters and flagged characters received so far: any of them means something drives the line
static uint32_t activity(void)
{
	return rx.bytes + rx.uart_errors;
}

static void sentence(const char *s, uint8_t len, uint32_t now_ms)
{
#if CONFIG_APRS
	if (enabled) {
		aprs_nmea(s, len, true, now_ms);
	}
#else
	(void)s;
	(void)len;
	(void)now_ms;
#endif
}

static void gps_task(void *arg)
{
	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
		uint32_t now = hal_millis();
		gps_rx_state_t st = gps_rx_poll(&rx, now, sentence);
		switch (gps_baud_step(&hunt, st, now, activity())) {
		case GPS_BAUD_FOUND:
			if (!present) {
				present = true;
				if (cfg->found != NULL) {
					cfg->found();
				}
			}
			break;
		case GPS_BAUD_SWITCH:
			uart_rate(gps_baud_rate(&hunt));
			break;
		case GPS_BAUD_ABSENT:
			uart_on(false);		// no module: nothing more to do
			task = NULL;
			vTaskDelete(NULL);
			break;
		default:
			break;
		}
	}
}

void stm32_gps_init(const stm32_gps_cfg_t *c)
{
	cfg = c;
	uint32_t now = hal_millis();
	gps_rx_init(&rx, now, NO_TIMEOUT);
	gps_baud_init(&hunt, c->rates, c->n_rates, c->listen_ms, now, 0);
	present = false;
	uart_on(true);
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

bool stm32_gps_present(void)
{
	return present;
}

void stm32_gps_enable(bool on)
{
	enabled = on;
}

void stm32_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	gps_rx_counts(&rx, good, bad, bytes);
}

uint32_t stm32_gps_baud(void)
{
	return baud;
}
