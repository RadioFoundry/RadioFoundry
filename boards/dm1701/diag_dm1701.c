// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 diagnostics page lines: raw readings for bench checks (no bus traffic; everything is cached elsewhere)
#include <stddef.h>
#include "dm1701.h"

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

static void put_i(out_t *o, int32_t v)
{
	if (v < 0) {
		put(o, "-");
		v = -v;
	}
	put_u(o, (uint32_t)v);
}

static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF];
	}
}

bool dm1701_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	dm1701_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "bat raw ");
		put_u(&o, dm1701_adc_raw(1));
		put(&o, " ");
		put_u(&o, board.power->battery_mv());
		put(&o, "mV");
		break;
	case 1:
		put(&o, "vol raw ");
		put_u(&o, dm1701_adc_raw(0));
		put(&o, " = ");
		put_i(&o, board.power->volume());
		put(&o, "%");
		break;
	case 2:
		put(&o, "mic raw ");
		put_u(&o, dm1701_adc_raw(2));
		put(&o, " temp ");
		put_i(&o, dm1701_temperature_c10() / 10);
		put(&o, "C");
		break;
	case 3:
		dm1701_radio_diag(&d);
		put(&o, "1846 rssi ");
		put_u(&o, d.rssi);
		put(&o, " nz ");
		put_u(&o, d.noise);
		put(&o, " 1C=");
		put_x(&o, d.flags, 4);
		break;
	case 4:
		dm1701_radio_diag(&d);
		put(&o, "C6000 4:10=");
		if (d.c6000_ok) {
			put_x(&o, d.c6000_r10, 2);
		} else {
			put(&o, "--");
		}
		put(&o, " errs ");
		put_u(&o, d.bus_errors);
		break;
	case 5:
		put(&o, "panel type ");
		put_u(&o, dm1701_display_panel_type());
		put(&o, " cal ");
		put(&o, dm1701_cal_source());
		break;
	case 6: {
		char w[28];
		uint8_t m[8];
		dm1701_cal_why(w, sizeof w, m);
		put(&o, w);
		break;
	}
	case 7: {
		char w[28];
		uint8_t m[8];
		dm1701_cal_why(w, sizeof w, m);
		put(&o, "mk ");
		for (int k = 0; k < 8; k++)
			put_x(&o, m[k], 2);
		break;
	}
	case 8:	// VHF DMR I / Q gain rows (5 points)
	case 9:	// VHF FM I / Q
	case 10: {	// UHF FM I / Q (first 5 of 9 points)
		static const uint16_t rows[3][2] = { { 0x139, 0x149 }, { 0x179, 0x189 }, { 0x170, 0x180 } };
		static const char *const names[3] = { "VdIQ ", "VfIQ ", "UfIQ " };
		put(&o, names[i - 8]);
		for (int k = 0; k < 2; k++) {
			for (int n = 0; n < 5; n++)
				put_x(&o, dm1701_cal_byte((uint16_t)(rows[i - 8][k] + n)), 2);
			put(&o, " ");
		}
		break;
	}
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}
