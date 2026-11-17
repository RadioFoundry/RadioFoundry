// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 side keys, PTT and channel knob (docs/spec/hardware-rt10.md 6). Direct inputs, low = pressed. The PTT pin is
// also the accessory jack's serial line, so a low read while host bytes arrive is serial data (shared_ptt.h). The knob
// is a 16-position switch read as a position: it picks the zone's channel by where it points (chswitch.h)
#include "rt10.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/sharedptt/shared_ptt.h"
#include "input/chswitch/chswitch.h"

// Between two positions the code can read a third for up to about 100 ms (measured)
#define KNOB_SETTLE_MS	150u

// Position by code, from the counterclockwise stop: the codes read one position at a time were
// 6 2 9 C 5 1 A E F 7 0 8 D 4 3 B (measured, hardware-rt10.md 6)
static const int8_t knob_position[16] = {
	[0x6] = 0, [0x2] = 1, [0x9] = 2, [0xC] = 3, [0x5] = 4, [0x1] = 5, [0xA] = 6, [0xE] = 7,
	[0xF] = 8, [0x7] = 9, [0x0] = 10, [0x8] = 11, [0xD] = 12, [0x4] = 13, [0x3] = 14, [0xB] = 15,
};

static uint32_t last_raw, stable;
static shared_ptt_t ptt_gate;
static chswitch_t knob;

static void init(void)
{
	hrc_pin_in(PIN_PTT);
	hrc_pin_in(PIN_SK1);
	hrc_pin_in(PIN_SK2);
	chswitch_init(&knob, knob_position, 16, KNOB_SETTLE_MS, rt10_knob_raw(), hal_millis());
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
	line.tx_seen = hrc_host_uart_last_tx(&line.tx_ms);
	if (shared_ptt_pressed(&ptt_gate, !hrc_pin_read(PIN_PTT), &line, hal_millis())) {
		raw |= 1u << KEY_PTT;
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

// Called after each scan
static int8_t position(void)
{
	return chswitch_poll(&knob, rt10_knob_raw(), hal_millis());
}

static bool ptt_line_stuck(void)
{
	return shared_ptt_stuck(&ptt_gate, hal_millis());
}

const hal_input_t rt10_input = {
	.ptt_line_stuck = ptt_line_stuck,
	.init = init,
	.scan = scan,
	.position = position,
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
