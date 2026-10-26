// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The family's GPS line, through the shared STM32 GPS receiver (platform/stm32f4/gps_uart): NMEA on USART1 RX (PA10),
// module power on PA9, high = on. A factory module on the MD-UV380 G models (hardware-mduv380.md 9); the DM-1701 has the
// same two pins and no module, but an owner can fit one there (hardware-dm1701.md 14). Radios with and without a module
// share an image, so the module is powered for the listen window only and switched off again when nothing is heard. A
// sentence keeps it on, and every sentence goes to the APRS position source (CONFIG_APRS)
#include "config.h"
#include "uvf.h"
#include "pins.h"
#include "gps_uart/gps_uart.h"

#if CONFIG_GPS	// off: the driver and its interrupt handler are left out, and the radio has no GPS

// 9600 is the usual default of these modules. The TYT images ask for 14400 from a clock library built for a 12 MHz
// crystal, which is 9600 on this 8 MHz one (14400 x 8 / 12, hardware-mduv380.md 9). 14400 stays in the hunt until a G
// model confirms 9600 on the wire
static const uint32_t rates[] = { 9600, 14400 };

static void absent(void)
{
	PIN_WRITE(PIN_GPS_POWER, false);
}

static const stm32_gps_cfg_t gps = {
	.uart = USART1, .irq = USART1_IRQn,
	.rx_port = PIN_PORT(PIN_GPS_RX), .rx_pin = PIN_MASK(PIN_GPS_RX), .rx_af = GPIO_AF7_USART1,
	.rates = rates, .n_rates = sizeof(rates) / sizeof(rates[0]), .listen_ms = 5000,
	.absent = absent,
};

void USART1_IRQHandler(void)
{
	stm32_gps_irq();
}

void uvf_gps_init(void)
{
	PIN_WRITE(PIN_GPS_POWER, true);	// an output, low since reset (system_uvf.c)
	stm32_gps_init(&gps);
}
#endif
