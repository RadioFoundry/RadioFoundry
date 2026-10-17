// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV GPS: the ATGM336H's NMEA output on UART1 RX (PTA9), 9600 8N1, receive only (docs/spec/hardware-dm32.md 12).
//
// The module is powered (GPIO 0x2C low) at start-up and listened for for LISTEN_MS. One checksum-valid sentence makes
// it present, and every sentence then goes to the APRS position source (CONFIG_APRS). Nothing heard: the module is
// powered off again, the UART stopped, and the radio reports no GPS (board.gps_present).
#include "dm32.h"
#include "pins.h"
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
static volatile bool present;
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
	(void)hrc_uart_rx_isr(UART1_BASE, rx_char);
}

static void gps_on(bool on)
{
	if (on) {
		hrc_pin_write(PIN_GPS_POWER, false);
		hrc_uart_setup(UART1_BASE, GPS_BAUD);
		hrc_irq_register(IRQ_UART1, uart_isr);
		hrc_uart_rx_irq(UART1_BASE, true);
		hrc_irq_enable(IRQ_UART1, true);
	} else {
		hrc_irq_enable(IRQ_UART1, false);
		hrc_uart_rx_irq(UART1_BASE, false);
		hrc_pin_write(PIN_GPS_POWER, true);
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
		switch (gps_rx_poll(&rx, hal_millis(), sentence)) {
		case GPS_RX_FOUND:
			present = true;
			break;
		case GPS_RX_ABSENT:
			gps_on(false);		// no module answered: don't keep its supply on
			task = NULL;
			vTaskDelete(NULL);
			break;
		default:
			break;
		}
	}
}

void dm32_gps_init(void)
{
	gps_rx_init(&rx, hal_millis(), LISTEN_MS);
	present = false;
	gps_on(true);
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

bool dm32_gps_present(void)
{
	return present;
}

void dm32_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	gps_rx_counts(&rx, good, bad, bytes);
}
