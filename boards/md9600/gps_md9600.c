// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 GPS: an optional module's NMEA on USART1 RX (PA10), detected at run time (docs/spec/hardware-md9600.md 13),
// through the shared STM32 GPS receiver (platform/stm32f4/gps_uart). The module's supply is PA9, which also powers the
// hand mic, so it stays on whatever the GPS setting says; the setting only stops the sentences reaching the position
// source. 9600 baud first (8N1, receive only), then the other usual rates: a line that stays silent for the first
// window ends the search, so a radio without a module listens once and turns the UART off again.
#include "config.h"
#if CONFIG_GPS	// off: the driver and its interrupt handler are left out, and the radio has no GPS
#include "md9600.h"
#include "pins.h"
#include "gps_uart/gps_uart.h"

#define LISTEN_MS	3000u	// per rate: modules send at least once a second

static const uint32_t rates[] = { 9600, 4800, 19200, 38400, 57600, 115200 };

static const stm32_gps_cfg_t gps = {
	.uart = USART1, .irq = USART1_IRQn,
	.rx_port = PIN_PORT(PIN_GPS_RX), .rx_pin = PIN_MASK(PIN_GPS_RX), .rx_af = GPIO_AF7_USART1,
	.rates = rates, .n_rates = sizeof rates / sizeof rates[0], .listen_ms = LISTEN_MS,
};

void USART1_IRQHandler(void)
{
	stm32_gps_irq();
}

void md9600_gps_init(void)
{
	stm32_gps_init(&gps);
}

#endif	// CONFIG_GPS
