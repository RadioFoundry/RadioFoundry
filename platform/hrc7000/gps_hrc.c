// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An HR_C7000 radio's GPS: the module's NMEA output on a receive-only UART, 9600 8N1 (docs/spec/hardware-dm32.md 12,
// hardware-db25d.md 3.1). The board gives the UART, its interrupt, the power pin with its polarity and, if its io init
// hasn't, the RXD pad's mux (hrc_gps_cfg_t).
//
// The module is powered at start-up and listened for for LISTEN_MS, whatever Settings > Location > GPS says, so the
// radio knows whether it has one. One checksum-valid sentence makes it present, and every sentence then goes to the
// APRS position source (CONFIG_APRS). Nothing heard: the module is powered off again, the UART stopped, and the radio
// reports no GPS (board.gps_present). Once present, the module is powered only while the setting is on
// (hrc_gps_enable, board_t.gps_enable): the task switches it within one poll, the UART and the supply together.
#include "platform.h"
#include "config.h"
#include "hal_time.h"
#include "gps/gps_rx/gps_rx.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif

#define GPS_BAUD		9600u
#define LISTEN_MS		5000u	// the module sends a sentence a second from about 1 s after power-up, fix or not
#define GPS_TASK_STACK		384	// words: the APRS NMEA parser runs on it
#define GPS_TASK_PRIORITY	1
#define GPS_POLL_MS		20

static gps_rx_t rx;
static const hrc_gps_cfg_t *cfg;
static volatile bool present;
static volatile bool want = true;	// the setting, as hrc_gps_enable last had it; on until the UI says otherwise
static bool running;			// the module is powered and the UART listening (the task's own state)
static TaskHandle_t task;

static void rx_char(uint8_t c, bool flagged)
{
	if (flagged) {
		gps_rx_error(&rx);
	} else {
		gps_rx_byte(&rx, c);
	}
}

static void uart_isr(void)
{
	(void)hrc_uart_rx_isr(cfg->uart_base, rx_char);
}

// The UART is set up once (hrc_gps_init). Switching the module on again only clears the UART and enables its
// interrupt: while the module is off its line is held low, a break the UART is still receiving, and this UART ignores
// a line control write while it is busy, so setting the baud rate again then would fail
static void gps_on(bool on)
{
	if (on) {
		hrc_pin_write(cfg->power_pin, cfg->power_on_level);
		hrc_uart_rx_clear(cfg->uart_base);
		hrc_uart_rx_irq(cfg->uart_base, true);
		hrc_irq_enable(cfg->irq, true);
	} else {
		hrc_irq_enable(cfg->irq, false);
		hrc_uart_rx_irq(cfg->uart_base, false);
		hrc_pin_write(cfg->power_pin, !cfg->power_on_level);
	}
}

static void sentence(const char *s, uint8_t len, uint32_t now_ms)
{
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

void hrc_gps_init(const hrc_gps_cfg_t *c)
{
	cfg = c;
	gps_rx_init(&rx, hal_millis(), LISTEN_MS);
	present = false;
	IO_DIPLEX0 &= ~cfg->rxd_mux_clear;
	hrc_uart_setup(cfg->uart_base, GPS_BAUD);	// before the module is powered: its line is idle, the UART not busy
	hrc_irq_register(cfg->irq, uart_isr);
	gps_on(true);
	running = true;
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

void hrc_gps_enable(bool on)
{
	want = on;
}

bool hrc_gps_running(void)
{
	return running;
}

bool hrc_gps_present(void)
{
	return present;
}

void hrc_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	gps_rx_counts(&rx, good, bad, bytes);
}
