// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family diagnostics page lines: raw readings for bench checks (no bus traffic; everything is cached elsewhere)
#include <stddef.h>
#include "uvf.h"
#include "config.h"
#if CONFIG_DMR
#include "dmr_uvf.h"
#include "hrc/i2s_pcm.h"
#endif

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

bool uvf_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
#if CONFIG_DMR
	// The DMR modem counters first (bring-up), then the board readings
	if (i < STM32_HRC_DMR_DIAG_LINES) {
		return stm32_hrc_dmr_stats_line(i, buf, len);
	}
	i -= STM32_HRC_DMR_DIAG_LINES;
#endif
	out_t o = { buf, buf + len - 1 };
	uvf_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "bat raw ");
		put_u(&o, uvf_adc_raw(1));
		put(&o, " ");
		put_u(&o, board.power->battery_mv());
		put(&o, "mV");
		break;
	case 1:
		put(&o, "vol raw ");
		put_u(&o, uvf_adc_raw(0));
		put(&o, " = ");
		put_i(&o, board.power->volume());
		put(&o, "%");
		break;
	case 2:
		put(&o, "mic raw ");
		put_u(&o, uvf_adc_raw(2));
		put(&o, " temp ");
		put_i(&o, uvf_temperature_c10() / 10);
		put(&o, "C");
		break;
	case 3:
		uvf_radio_diag(&d);
		put(&o, "1846 rssi ");
		put_u(&o, d.rssi);
		put(&o, " nz ");
		put_u(&o, d.noise);
		put(&o, " 1C=");
		put_x(&o, d.flags, 4);
		break;
	case 4:
		uvf_radio_diag(&d);
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
		put_u(&o, uvf_display_panel_type());
		put(&o, " cal ");
		put(&o, uvf_cal_source());
		break;
	case 6: {
		char w[28];
		uint8_t m[8];
		uvf_cal_why(w, sizeof w, m);
		put(&o, w);
		break;
	}
	case 7: {
		char w[28];
		uint8_t m[8];
		uvf_cal_why(w, sizeof w, m);
		put(&o, "mk ");
		for (int k = 0; k < 8; k++)
			put_x(&o, m[k], 2);
		break;
	}
#if CONFIG_DMR
	case 11: {	// DMR PCM audio: blocks, overruns, blocks dropped
		stm32_i2s_pcm_stats_t a;
		stm32_i2s_pcm_stats(&a);
		put(&o, "aud blk ");
		put_u(&o, a.blocks);
		put(&o, " ov ");
		put_u(&o, a.overruns);
		put(&o, " dr ");
		put_u(&o, a.dropped);
		break;
	}
#endif
	case 8:	// VHF DMR I / Q gain rows (5 points)
	case 9:	// VHF FM I / Q
	case 10: {	// UHF FM I / Q (first 5 of 9 points)
		static const uint16_t rows[3][2] = { { 0x139, 0x149 }, { 0x179, 0x189 }, { 0x170, 0x180 } };
		static const char *const names[3] = { "VdIQ ", "VfIQ ", "UfIQ " };
		put(&o, names[i - 8]);
		for (int k = 0; k < 2; k++) {
			for (int n = 0; n < 5; n++)
				put_x(&o, uvf_cal_byte((uint16_t)(rows[i - 8][k] + n)), 2);
			put(&o, " ");
		}
		break;
	}
#if !CONFIG_DMR
	case 11:
		put(&o, "no DMR");
		break;
#endif
	case 12: {	// how the clock started, and whether this image may transmit on this radio (uvf_variant.tx_block)
		static const char *const hse[] = { "HSI", "ext", "xtal" };
		const char *block = uvf_variant.tx_block != NULL ? uvf_variant.tx_block() : NULL;
		put(&o, "hse ");
		put(&o, hse[uvf_hse_mode() % 3u]);
		put(&o, block != NULL ? " TX off: " : " TX ok");
		if (block != NULL) {
			put(&o, block);
		}
		break;
	}
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void uvf_diag_reset(void)
{
#if CONFIG_DMR
	stm32_hrc_dmr_stats_reset();
	stm32_i2s_pcm_reset_stats();
#endif
}

#endif
