// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV GPS (docs/spec/hardware-d578uv.md 3.6): the module on USART2 (PA2 TX, PA3 RX), 9600 8N1, through the
// platform's GPS driver (gps_uart/). The module's supply is RF chip 0's output bit 2, set through
// the radio's shadow of that register (d578uv_gps_power, radio_d578uv.c), as the stock firmware does. A CASIC module
// is known by its first $xxTXT sentence and gets the stock firmware's set-up; any other module's default output is
// parsed as is
#include "config.h"
#include "d578uv.h"
#include "pins.h"
#include "gps_uart/gps_uart.h"

#if CONFIG_GPS	// off: the driver and its interrupt handler are left out, and the radio has no GPS

// pins.h gives a pin as "port, mask"
#define PORT_OF(p)		PORT_OF_(p)
#define PORT_OF_(port, mask)	port
#define MASK_OF(p)		MASK_OF_(p)
#define MASK_OF_(port, mask)	mask

// The stock firmware's CASIC set-up, sent on the first $xxTXT: one fix a second, GGA and RMC only (what the position
// source reads), and GPS alone, the stock satellite setting's default. The stock firmware turns GSV on only while its
// satellite screen is open; the GPS status screen (CONFIG_GPS_SKY) reads GSV and GSA all the time, so they stay on with
// it. Its MTK branch (on $PMTK010,001) is left out: it sends $PMTK225 with a $PMTK314 output list (3.6), which sets
// nothing useful
#if CONFIG_GPS_SKY
#define PCAS03	"PCAS03,1,0,1,1,1,0,0,0\n"	// GGA, GSA, GSV, RMC
#else
#define PCAS03	"PCAS03,1,0,0,0,1,0,0,0\n"	// GGA, RMC
#endif
static const gps_setup_t setup[] = {
	{ "$??TXT", "PCAS02,1000\n" PCAS03 "PCAS04,1\n" },
};

static const mcu_gps_cfg_t gps = {
	.uart = MCU_USART2, .irq = MCU_USART2_IRQn,
	.rx_port = PORT_OF(PIN_GPS_RX), .rx_pin = MASK_OF(PIN_GPS_RX),
	.baud = 9600,
	// The module answers about 1 s after its supply comes on, and the supply waits for the radio's set-up of the RF
	// chips (a few hundred ms after board_init): 8 s leaves room for both
	.listen_ms = 8000,
	.power = d578uv_gps_power,
	.tx_port = PORT_OF(PIN_GPS_TX), .tx_pin = MASK_OF(PIN_GPS_TX),
	.setup_n = sizeof setup / sizeof setup[0], .setup = setup,
};

void MCU_USART2_HANDLER(void)
{
	mcu_gps_irq();
}

void d578uv_gps_init(void)
{
	mcu_gps_init(&gps);
}
#endif
