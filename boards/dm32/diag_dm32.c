// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV diagnostics page lines: raw readings for bench checks (cached values only, no bus traffic)
#include <stddef.h>
#include "dm32.h"
#include "config.h"
#include "platform.h"

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

bool dm32_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
#if CONFIG_DMR
	// The DMR modem counters first (bring-up), then the board readings
	if (i < DM32_DMR_DIAG_LINES) {
		return dm32_dmr_stats_line(i, buf, len);
	}
	i -= DM32_DMR_DIAG_LINES;
#endif
	out_t o = { buf, buf + len - 1 };
	dm32_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "bat ");
		put_u(&o, dm32_adc_raw(2));
		put(&o, " ");
		put_u(&o, board.power->battery_mv());
		put(&o, "mV");
		break;
	case 1:
		dm32_radio_diag(&d);
		put(&o, "rssi ");
		put_u(&o, d.rssi_raw);
		put(&o, " nz ");
		put_u(&o, d.noise_raw);
		put(&o, " fm ");
		put_u(&o, d.fm_noise);
		break;
	case 2:
		dm32_radio_diag(&d);
		put(&o, "fd wr ");
		put_u(&o, d.fd_writes);
		put(&o, " uo ");
		put_u(&o, hrc_uart0_overruns());
		break;
	case 3:
		put(&o, "flash ");
		put_x(&o, dm32_flash_jedec(), 6);
		put(&o, " cal ");
		put(&o, dm32_cal_source());
		break;
	case 4:
		put(&o, "txc ");
		put_u(&o, dm32_cal_tx_center());
		put(&o, " mod ");
		put_x(&o, dm32_cal_modcal(145000000u), 4);
		put(&o, " ");
		put_x(&o, dm32_cal_modcal(435000000u), 4);
		break;
	case 5:	// PA drive, power level 1 (low) and 8 (high) at 145 / 435 MHz
		put(&o, "pa ");
		put_u(&o, dm32_cal_power_level(145000000u, 1));
		put(&o, "/");
		put_u(&o, dm32_cal_power_level(145000000u, 8));
		put(&o, " ");
		put_u(&o, dm32_cal_power_level(435000000u, 1));
		put(&o, "/");
		put_u(&o, dm32_cal_power_level(435000000u, 8));
		break;
#if CONFIG_DMR
	case 6: {
		uint32_t b, ov, un;
		dm32_audio_stats(&b, &ov, &un);
		put(&o, "aud ");
		put_u(&o, b);
		put(&o, " ov ");
		put_u(&o, ov);
		put(&o, " un ");
		put_u(&o, un);
		break;
	}
	case 7:
		put(&o, "codec ");
		if (dm32_vocoder_tag() != 0) {
			put(&o, "0");
			put_u(&o, dm32_vocoder_tag());
		} else {
			put(&o, "none");
		}
		break;
#endif
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void dm32_diag_reset(void)
{
#if CONFIG_DMR
	dm32_dmr_stats_reset();
	dm32_audio_reset_stats();
#endif
}

#endif
