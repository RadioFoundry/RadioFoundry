// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380G / MD-390G / RT3 GPS: the module's NMEA output on USART3 RX (PD9), detected at run time
// (docs/spec/hardware-md380.md 13), through the shared STM32 GPS receiver (platform/stm32f4/gps_uart).
//
// Radios with and without the module share the board. PD8 is the module's power switch on the G models but is
// reported as a flash chip select on others, so nothing drives it until NMEA has been heard: the UART listens for
// LISTEN_MS after start-up with PD8 left as an input. One checksum-valid sentence makes the module present; PD8 is then
// held at the level it already had (the level the module runs at, whatever its polarity), and every sentence goes to
// the APRS position source (CONFIG_APRS). Nothing heard: the UART is switched off again and the radio has no GPS.
#include "md380.h"
#include "pins.h"
#include "gps_uart/gps_uart.h"

static const uint32_t rates[] = { 9600 };	// the usual default of these modules (?)

// Heard: hold the power pin where it is (hardware-md380.md 13.2)
static void found(void)
{
	bool level = PIN_READ(PIN_GPS_POWER);
	stm32_pin_out(PIN_PORT(PIN_GPS_POWER), PIN_MASK(PIN_GPS_POWER), level, false);
}

static const stm32_gps_cfg_t gps = {
	.uart = USART3, .irq = USART3_IRQn,
	.rx_port = PIN_PORT(PIN_GPS_RX), .rx_pin = PIN_MASK(PIN_GPS_RX), .rx_af = GPIO_AF7_USART3,
	.rates = rates, .n_rates = 1, .listen_ms = 5000,
	.found = found,
};

void USART3_IRQHandler(void)
{
	stm32_gps_irq();
}

void md380_gps_init(void)
{
	stm32_gps_init(&gps);
}
