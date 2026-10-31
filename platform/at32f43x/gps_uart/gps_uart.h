// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A GPS module's NMEA output on an AT32F43x USART, for the radios whose module the board can power: the receive
// interrupt, the task that drains it through drivers/gps/gps_rx, run-time detection, the module's power and, where the
// board lists them, its set-up sentences.
//
// The module is powered at start-up and listened to for listen_ms, whatever Settings > Location > GPS says, so the
// radio knows whether it has one. One checksum-valid sentence makes it present, and every sentence then goes to the
// APRS position source (CONFIG_APRS). Nothing heard: the module is powered off, the USART stopped, and the radio
// reports no GPS. Once present, the module is powered only while the setting is on (at32_gps_enable): the task
// switches the supply and the receive path together, within one poll.
//
// With a set-up list (gps_setup.h), the first sentence that matches an entry sends that entry's sentences on the TX pin,
// polled (a few dozen characters, once per power-on). The TX pin is the USART's output only while the module is
// powered, a floating input otherwise, so an unpowered module isn't fed through its RX line.
//
// A board adds platform/at32f43x/gps_uart, drivers/gps/nmea_line and drivers/gps/gps_rx to BOARD_DIRS and calls
// at32_gps_irq() from its USART's handler.
#ifndef AT32_GPS_UART_H
#define AT32_GPS_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"
#include "gps/gps_rx/gps_setup.h"

typedef struct {
	at32_usart_t *uart;
	IRQn_Type irq;
	at32_gpio_t *rx_port;
	uint16_t rx_pin;
	uint32_t baud;
	uint32_t listen_ms;		// how long to wait for a first sentence after start-up
	void (*power)(bool on);		// optional, task context: the module's supply; NULL = always powered
	at32_gpio_t *tx_port;		// with setup: the USART's TX pin
	uint16_t tx_pin;
	uint8_t setup_n;		// entries in setup; 0 = receive only
	const gps_setup_t *setup;
} at32_gps_cfg_t;

// Start listening (from board_init). cfg stays valid
void at32_gps_init(const at32_gps_cfg_t *cfg);
// From the board's USART interrupt handler
void at32_gps_irq(void);
// The module has been heard (board_t.gps_present)
bool at32_gps_present(void);
// Settings > Location > GPS (board_t.gps_enable): the module's power, once it has been heard
void at32_gps_enable(bool on);
// For diagnostics: valid sentences, bad ones, bytes received; whether the module is powered and listened to
void at32_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes);
bool at32_gps_running(void);

#endif
