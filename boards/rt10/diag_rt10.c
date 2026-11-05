// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 diagnostics page: raw input and ADC readings (docs/spec/hardware-rt10.md 3)
#include <stdio.h>
#include "rt10.h"
#include "pins.h"
#include "platform.h"

bool rt10_diag_line(uint8_t i, char *buf, uint8_t len)
{
	switch (i) {
	case 0:
		snprintf(buf, len, "knob raw %u", rt10_knob_raw());
		return true;
	case 1:
		snprintf(buf, len, "pwr sw %u ptt %u", hrc_pin_read(PIN_POWER_SWITCH), hrc_pin_read(PIN_PTT));
		return true;
	case 2:
		snprintf(buf, len, "sk1 %u sk2 %u", hrc_pin_read(PIN_SK1), hrc_pin_read(PIN_SK2));
		return true;
	case 3:
		snprintf(buf, len, "bat raw %u", hrc_adc_last(2));
		return true;
	case 4:
		snprintf(buf, len, "ptt ign %lu", (unsigned long)rt10_input_ptt_ignored());
		return true;
	case 5: {
		uint16_t id;
		uint32_t nacks;
		rt10_rf_info(&id, &nacks);
		snprintf(buf, len, "rf id %04X nack %lu", id, (unsigned long)nacks);
		return true;
	}
	default:
		return false;
	}
}
