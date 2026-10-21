// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 diagnostics page lines: raw readings for bench checks (no bus traffic; everything is cached elsewhere)
#include <stddef.h>
#include "md380.h"
#include "config.h"
#if CONFIG_DMR
#include "dmr_md380.h"
#include "hrc/i2s_pcm.h"
#include "gps_uart/gps_uart.h"
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

bool md380_diag_line(uint8_t i, char *buf, uint8_t len)
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
	md380_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "bat raw ");
		put_u(&o, md380_adc_raw(MD380_ADC_BATTERY));
		put(&o, " ");
		put_u(&o, board.power->battery_mv());
		put(&o, "mV");
		break;
	case 1:
		put(&o, "vol ");
		put_u(&o, md380_adc_raw(MD380_ADC_VOLUME));
		put(&o, " mic ");
		put_u(&o, md380_adc_raw(MD380_ADC_MIC));
		put(&o, " ");
		put_i(&o, md380_temperature_c10() / 10);
		put(&o, "C");
		break;
	case 2:		// receiver readings for the RSSI / squelch curves (hardware-md380.md 5.5)
		put(&o, "rssi ");
		put_u(&o, md380_adc_raw(MD380_ADC_RSSI));
		put(&o, " busy ");
		put_u(&o, md380_adc_raw(MD380_ADC_BUSY));
		break;
	case 3:
		md380_radio_diag(&d);
		put(&o, "C5000 10=");
		if (d.c5000_ok) {
			put_x(&o, d.c5000_r10, 2);
		} else {
			put(&o, "--");
		}
		put(&o, " errs ");
		put_u(&o, d.bus_errors);
		break;
	case 4:
		md380_radio_diag(&d);
		put(&o, d.pll_locked ? "pll lock " : "pll UNLK ");
		put_u(&o, d.pll_vco_hz / 1000u);
		put(&o, "k f");
		put_u(&o, d.pll_fails);
		break;
	case 5:
		put(&o, "cal ");
		put(&o, md380_cal_source());
		put(&o, " ");
		put(&o, md380_cal_why());
		break;
	case 6:		// calibration bytes: reference tune, first low and high power points
		put(&o, "ref ");
		put_x(&o, md380_cal_byte(0x009), 2);
		put(&o, " lo ");
		put_x(&o, md380_cal_byte(0x020), 2);
		put(&o, " hi ");
		put_x(&o, md380_cal_byte(0x010), 2);
		put(&o, " rx ");
		put_x(&o, md380_cal_byte(0x030), 2);
		break;
	case 7: {	// raw keypad rows and the channel switch code (key positions, hardware-md380.md 9.1)
		uint8_t rows[3], code;
		md380_input_raw(rows, &code);
		put(&o, "keys ");
		put_x(&o, rows[0], 2);
		put(&o, " ");
		put_x(&o, rows[1], 2);
		put(&o, " ");
		put_x(&o, rows[2], 2);
		put(&o, " ch ");
		put_x(&o, code, 1);
		break;
	}
	case 8: {
		uint32_t good, bad, n;
		stm32_gps_counts(&good, &bad, &n);
		put(&o, stm32_gps_present() ? "gps yes " : "gps no ");
		put_u(&o, good);
		put(&o, "/");
		put_u(&o, bad);
		put(&o, " b");
		put_u(&o, n);
		break;
	}
	case 9:
		put(&o, "flash ");
		put_x(&o, md380_flash_device(), 4);
		put(&o, " ");
		put_u(&o, board.flash->size >> 10);
		put(&o, "K");
		break;
#if CONFIG_DMR
	case 10: {	// DMR PCM audio: blocks, overruns, blocks dropped
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
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void md380_diag_reset(void)
{
#if CONFIG_DMR
	stm32_hrc_dmr_stats_reset();
	stm32_i2s_pcm_reset_stats();
#endif
}

#endif
