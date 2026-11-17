// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 diagnostics page: raw input and ADC readings (docs/spec/hardware-rt10.md 3)
#include <stdio.h>
#include "rt10.h"
#include "pins.h"
#include "platform.h"
#include "config.h"
#if CONFIG_DMR
#include "dmr_glue.h"
#include "dmr_rfburst.h"
#include "vocoder.h"
#include "dmr_rt10.h"
#endif

#define DIAG_DMR_FIRST	6u	// the DMR modem counters (dmr_glue.c), then the codec and the burst line

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
		break;
	}
#if CONFIG_DMR
	if (i >= DIAG_DMR_FIRST && i < DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES) {
		return hrc_dmr_stats_line((uint8_t)(i - DIAG_DMR_FIRST), buf, len);
	}
	if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES) {
		// DMR TX bursts (hardware-rt10.md 7.1): TX-on and TX-off sequences run, RF RX interrupts seen, what woke the
		// last TX-off (R: RF RX interrupt, T: slot tick, A: abort), the ramp downs that started late and the last by
		// how much; then the codec in the slot
		hrc_rfburst_stats_t t;
		hrc_rfburst_stats(&t);
		snprintf(buf, len, "b%lu/%lu rx%lu %c L%lu/%lu c%u", (unsigned long)t.bursts, (unsigned long)t.ends,
			 (unsigned long)t.rfrx_ints, t.last_by != 0 ? t.last_by : '-', (unsigned long)t.late,
			 (unsigned long)t.last_late_us, (unsigned)hrc_vocoder_tag());
		return true;
	}
	if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES + 1) {
		// The AF phase range the modem saw, largest / smallest: within a few counts, no audio reaches its ADC
		// (modem-hrc7000.md 2.1)
		uint32_t ph = rt10_dmr_phase();
		snprintf(buf, len, "ph %ld/%ld", (long)((int32_t)(ph << 2) >> 18), (long)((int32_t)(ph << 18) >> 18));
		return true;
	}
	if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES + 2) {
		// The TX-on timing since the key-up (hrc_rfburst_stats_t), in us: the RF task's wake (largest), the PA up after
		// the RF TX interrupt (smallest..largest; the first symbol is at lead x 100 + 850), an interrupt off its grid
		hrc_rfburst_stats_t t;
		hrc_rfburst_stats(&t);
		snprintf(buf, len, "up w%lu u%lu..%lu g%lu/%lu", (unsigned long)t.wake_max_us, (unsigned long)t.up_min_us,
			 (unsigned long)t.up_max_us, (unsigned long)t.grid_max_us, (unsigned long)t.regridded);
		return true;
	}
#endif
	return false;
}
