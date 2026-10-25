// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 diagnostics page lines: the board revision in use and where it came from, then raw readings for the first
// power-up (docs/spec/hardware-md9600.md 19): calibration and option blocks, band edges, the synthesizer's last LO,
// ADC channels, keys, flash. The debug link's board view (view_md9600.c) carries the same and more
#include <stddef.h>
#include "md9600.h"
#include "spi_flash/spi_flash.h"
#include "config.h"
#include "rfseq_md9600.h"

#if CONFIG_DIAG

typedef struct {
	char *p, *end;
} out_t;

static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end) {
		*o->p++ = *s++;
	}
}

static void put_u(out_t *o, uint32_t v)
{
	char t[11];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 10);
	while (n && o->p < o->end) {
		*o->p++ = t[--n];
	}
}

static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF];
	}
}

// MHz with one decimal: 136.0
static void put_mhz(out_t *o, uint32_t hz)
{
	put_u(o, hz / 1000000u);
	put(o, ".");
	put_u(o, (hz / 100000u) % 10u);
}

bool md9600_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	md9600_rev_state_t r = md9600_rev_state();
	md9600_radio_diag_t rd;
	md9600_radio_diag(&rd);
	if (i < MD9600_REV_TEXT_LINES) {
		return md9600_rev_text(i, r, rd.probe, buf, len);
	}
	switch (i) {
	// The revision lines come from md9600_rev_text (host-tested for length). The lines below fit the 128x64 panel's small font (25 characters): one reading per line where two don't fit
	case 3:
		put(&o, "cal ");
		put(&o, md9600_cal_source());
		break;
	case 4:		// the option block, and "bl on/off" when it says the backlight can't be dimmed
		put(&o, "opt ");
		put(&o, md9600_opt_source());
		put(&o, md9600_opt_backlight_switched() ? " bl on/off" : "");
		break;
	case 5:
	case 6: {	// the band edges from the option block (hardware-md9600.md 11.1)
		md9600_band_edges_t e;
		put(&o, i == 5 ? "VHF " : "UHF ");
		if (!md9600_opt_band_edges(&e)) {
			put(&o, "-");
			break;
		}
		put_mhz(&o, i == 5 ? e.vhf_lo_hz : e.uhf_lo_hz);
		put(&o, "-");
		put_mhz(&o, i == 5 ? e.vhf_hi_hz : e.uhf_hi_hz);
		break;
	}
	case 7:
		put(&o, "LO ");
		put_u(&o, rd.lo_hz / 1000u);
		put(&o, "k n");
		put_u(&o, rd.retunes);
		put(&o, " x");
		put_u(&o, rd.refused);
		break;
	case 8:		// receiver readings for the RSSI / squelch curves (hardware-md9600.md 5.7)
	case 9:
		put(&o, i == 8 ? "U rssi " : "V rssi ");
		put_u(&o, md9600_adc_raw(i == 8 ? MD9600_ADC_RSSI_U : MD9600_ADC_RSSI_V));
		put(&o, " nz ");
		put_u(&o, md9600_adc_raw(i == 8 ? MD9600_ADC_NOISE_U : MD9600_ADC_NOISE_V));
		break;
	case 10:
		put(&o, "supply ");
		put_u(&o, md9600_supply_mv());
		put(&o, "mV r");
		put_u(&o, md9600_adc_raw(MD9600_ADC_SUPPLY));
		break;
	case 11:	// the hand-mic keypad ladder and the mic level (hardware-md9600.md 9.3)
		put(&o, "mic r");
		put_u(&o, md9600_adc_raw(MD9600_ADC_MICKEY_ROW));
		put(&o, " c");
		put_u(&o, md9600_adc_raw(MD9600_ADC_MICKEY_COL));
		put(&o, " l");
		put_u(&o, md9600_adc_raw(MD9600_ADC_MIC));
		break;
	case 12: {	// the raw matrix: columns low per row K0-K3 (bring-up: which bit is which key)
		uint8_t rows[4];
		md9600_input_raw(rows);
		put(&o, "keys ");
		for (uint8_t k = 0; k < 4; k++) {
			put_x(&o, rows[k], 1);
		}
		break;
	}
	case 13:
		put(&o, "flash ");
		put_x(&o, stm32_spi_flash_device(), 4);
		put(&o, stm32_spi_flash.map != NULL ? " mapped" : " no map");
		break;
	case 14:	// the RF sequencer: state, the transmit arm, the last refusal or failure
		put(&o, rd.state == MD9600_RF_TX ? "rf tx" : rd.state == MD9600_RF_RX ? "rf rx" : "rf off");
		put(&o, rd.tx_armed ? " armed " : " safe ");
		put(&o, md9600_rf_err_name(rd.last_err));
		break;
	case 15:	// retunes, refusals, bus or lock errors
		put(&o, "rf n");
		put_u(&o, rd.retunes);
		put(&o, " x");
		put_u(&o, rd.refused);
		put(&o, " e");
		put_u(&o, rd.errors);
		break;
	case 16: {	// the calibration rows the RF code accepts per band: R front end, P PA, S squelch
		for (uint8_t b = 0; b < 2; b++) {
			uint8_t rows = md9600_cal_rf_rows(b);
			put(&o, b == MD9600_VHF ? "calrf V " : " U ");
			put(&o, (rows & MD9600_CALOK_RX) ? "R" : "-");
			put(&o, (rows & MD9600_CALOK_PA) ? "P" : "-");
			put(&o, (rows & MD9600_CALOK_SQL) ? "S" : "-");
		}
		break;
	}
	case 17:	// the sub-audio input (PA2, ADC3), for its level on a first power-up
		put(&o, "qt ");
		put_u(&o, md9600_adc3_qt());
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void md9600_diag_reset(void)
{
}

#endif
