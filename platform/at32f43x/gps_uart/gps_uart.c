// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS NMEA on an AT32F43x USART (gps_uart.h)
#include <stddef.h>
#include "gps_uart/gps_uart.h"
#include "config.h"
#include "hal_time.h"
#include "gps/gps_rx/gps_rx.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif

#define GPS_TASK_STACK		352	// words: the APRS NMEA parser runs on it (make stack-check: about 0.9 KB deep)
#define GPS_TASK_PRIORITY	1
#define GPS_POLL_MS		20
#define GPS_IRQ_PRIORITY	7	// numerically >= 4: may call FreeRTOS, doesn't need to

static const at32_gps_cfg_t *cfg;
static gps_rx_t rx;
static volatile bool present;
static volatile bool want = true;	// the setting, as at32_gps_enable last had it; on until the UI says otherwise
static volatile bool running;		// the module is powered and the USART listening (the task's own state)
static bool set_up;			// the set-up went out since the module was last powered on (task only)
static TaskHandle_t task;

void at32_gps_irq(void)
{
	at32_usart_t *u = cfg->uart;
	uint32_t st = u->STS;
	uint8_t c = (uint8_t)u->DT;	// reading STS then DT clears RDBF and the error flags
	if (st & (USART_STS_ROERR | USART_STS_FERR | USART_STS_NERR)) {
		gps_rx_error(&rx);
		return;
	}
	if (st & USART_STS_RDBF) {
		gps_rx_byte(&rx, c);
	}
}

// USART1 and USART6 are on APB2, the others on APB1
static void clock_on(bool on)
{
	at32_usart_t *u = cfg->uart;
	bool apb2 = u == USART1;
	volatile uint32_t *en = apb2 ? &CRM_APB2EN : &CRM_APB1EN;
	uint32_t bit = apb2 ? CRM_APB2EN_USART1EN : u == USART2 ? CRM_APB1EN_USART2EN :
		       u == USART3 ? CRM_APB1EN_USART3EN : u == UART4 ? CRM_APB1EN_UART4EN : CRM_APB1EN_UART5EN;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

// The TX pin's multiplexed function: 7 for USART1..3, 8 for UART4 and UART5 (the data sheet's pin table)
static uint8_t tx_af(void)
{
	return (cfg->uart == UART4 || cfg->uart == UART5) ? 8u : 7u;
}

// Polled: the set-up is a few dozen characters, sent once per power-on from the task (gps_setup.h)
static void tx_byte(uint8_t c)
{
	at32_usart_t *u = cfg->uart;
	while ((u->STS & USART_STS_TDBE) == 0) {
	}
	u->DT = c;
}

// The supply and the receive path together. The RX pin stays a floating input (the stock set-up of these radios): the
// USART takes an input pin as is, and a pull-up would feed an unpowered module through its TX line
static void gps_on(bool on)
{
	at32_usart_t *u = cfg->uart;
	if (on) {
		if (cfg->power != NULL) {
			cfg->power(true);
		}
		clock_on(true);
		uint32_t pclk = u == USART1 ? at32_apb2_hz() : at32_apb1_hz();
		u->CTRL1 = 0;
		u->BAUDR = (pclk + cfg->baud / 2u) / cfg->baud;	// the plain clock / baud divider
		u->CTRL2 = 0;
		u->CTRL3 = 0;
		(void)u->STS;
		(void)u->DT;
		u->CTRL1 = USART_CTRL1_UEN | USART_CTRL1_REN | USART_CTRL1_RDBFIEN | (cfg->setup_n != 0 ? USART_CTRL1_TEN : 0u);
		if (cfg->setup_n != 0) {
			at32_pin_af(cfg->tx_port, cfg->tx_pin, tx_af(), false);
		}
		NVIC_SetPriority(cfg->irq, GPS_IRQ_PRIORITY);
		NVIC_EnableIRQ(cfg->irq);
	} else {
		NVIC_DisableIRQ(cfg->irq);
		if (cfg->setup_n != 0) {
			// After the last character has left (TC): a set-up sent just before would otherwise be cut off
			while ((u->STS & USART_STS_TDC) == 0) {
			}
			at32_pin_in(cfg->tx_port, cfg->tx_pin, AT32_PULL_NONE);
		}
		set_up = false;		// the module forgets an unsaved set-up with its supply
		u->CTRL1 = 0;
		clock_on(false);
		if (cfg->power != NULL) {
			cfg->power(false);
		}
	}
}

static void sentence(const char *s, uint8_t len, uint32_t now_ms)
{
	if (!set_up) {
		const gps_setup_t *m = gps_setup_find(cfg->setup, cfg->setup_n, s, len);
		if (m != NULL) {
			set_up = true;
			gps_setup_send(m->send, tx_byte);
		}
	}
#if CONFIG_APRS
	aprs_nmea(s, len, true, now_ms);
#else
	(void)s;
	(void)len;
	(void)now_ms;
#endif
}

static void gps_task(void *arg)
{
	(void)arg;
	// Powered here, not in at32_gps_init: the board's power switch may need the RTOS or a part the radio sets up
	// later. The listen time starts with it
	gps_rx_init(&rx, hal_millis(), cfg->listen_ms);
	gps_on(true);
	running = true;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
		if (running) {
			switch (gps_rx_poll(&rx, hal_millis(), sentence)) {
			case GPS_RX_FOUND:
				present = true;
				break;
			case GPS_RX_ABSENT:
				gps_on(false);		// no module answered: don't keep its supply on
				running = false;
				task = NULL;
				vTaskDelete(NULL);
				break;
			default:
				break;
			}
		}
		// The setting decides once the module is known (a probe ends with the first sentence, whatever it says)
		if (present && want != running) {
			gps_on(want);
			running = want;
		}
	}
}

void at32_gps_init(const at32_gps_cfg_t *c)
{
	cfg = c;
	present = false;
	at32_pin_in(c->rx_port, c->rx_pin, AT32_PULL_NONE);
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

bool at32_gps_present(void)
{
	return present;
}

void at32_gps_enable(bool on)
{
	want = on;
}

bool at32_gps_running(void)
{
	return running;
}

void at32_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	gps_rx_counts(&rx, good, bad, bytes);
}
