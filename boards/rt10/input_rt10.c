// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 side keys, PTT and channel knob (docs/spec/hardware-rt10.md 6). Direct inputs, low = pressed. The PTT pin is
// also the accessory jack's serial line, so a low read while host bytes arrive is serial data (shared_ptt.h)
#include "rt10.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/sharedptt/shared_ptt.h"

static uint32_t last_raw, stable;
static shared_ptt_t ptt_gate;

static void init(void)
{
	hrc_pin_in(PIN_PTT);
	hrc_pin_in(PIN_SK1);
	hrc_pin_in(PIN_SK2);
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = 0;
	if (!hrc_pin_read(PIN_SK1)) {
		raw |= 1u << KEY_SK1;
	}
	if (!hrc_pin_read(PIN_SK2)) {
		raw |= 1u << KEY_SK2;
	}
	shared_ptt_line_t line = { .flagged = hrc_host_uart_line_errors(), .keyed = rt10_radio_transmitting() };
	line.rx_seen = hrc_host_uart_last_rx(&line.rx_ms);
	if (shared_ptt_pressed(&ptt_gate, !hrc_pin_read(PIN_PTT), &line, hal_millis())) {
		raw |= 1u << KEY_PTT;
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

static int8_t encoder(void)
{
	return 0;
}

const hal_input_t rt10_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
};

uint8_t rt10_knob_raw(void)
{
	return (uint8_t)(hrc_pin_read(PIN_KNOB_B3) << 3 | hrc_pin_read(PIN_KNOB_B2) << 2 | hrc_pin_read(PIN_KNOB_B1) << 1 |
		hrc_pin_read(PIN_KNOB_B0));
}

uint32_t rt10_input_ptt_ignored(void)
{
	return ptt_gate.ignored;
}
