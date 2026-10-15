// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro diagnostics page lines: raw readings for bench checks (cached values, except the accelerometer, which is
// read over its own bus when the line is drawn)
#include <stddef.h>
#include "dm1801pro.h"
#include "config.h"
#include "platform.h"
#include "cal/baofeng/baofeng_cal.h"

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

bool dm1801pro_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
#if CONFIG_DMR
	// The DMR modem counters first (bring-up), then the board readings
	if (i < HRC_DMR_DIAG_LINES) {
		return hrc_dmr_stats_line(i, buf, len);
	}
	i -= HRC_DMR_DIAG_LINES;
#endif
	out_t o = { buf, buf + len - 1 };
	dm1801pro_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "keys ");
		put_x(&o, dm1801pro_key_raw(), 6);	// matrix bit = strobe * 4 + input; direct pins in bits 20-23
		break;
	case 1:
		put(&o, "bat ");
		put_u(&o, dm1801pro_battery_raw());
		break;
	case 2:
		dm1801pro_radio_diag(&d);
		put(&o, "rssi ");
		put_u(&o, d.rssi_raw);
		put(&o, " noise ");
		put_u(&o, d.noise_raw);
		break;
	case 3:
		dm1801pro_radio_diag(&d);
		put(&o, "fmn ");
		put_u(&o, d.fm_noise);
		put(&o, " rf ");
		put_u(&o, d.rf_writes);
		break;
	case 4:
		put(&o, "flash ");
		put_x(&o, hrc_flash_spi0_jedec(), 6);
		break;
	case 5:
		put(&o, "cal ");
		put(&o, bf_cal_source());
		break;
	case 6:
		put(&o, "cal tx center ");
		put_u(&o, bf_cal_tx_center());
		break;
	case 7:
		put(&o, "pa 145 L/H ");
		put_u(&o, bf_cal_power_level(145000000u, 1));
		put(&o, " ");
		put_u(&o, bf_cal_power_level(145000000u, 8));
		break;
	case 8:
		put(&o, "pa 435 L/H ");
		put_u(&o, bf_cal_power_level(435000000u, 1));
		put(&o, " ");
		put_u(&o, bf_cal_power_level(435000000u, 8));
		break;
	case 9: {
		int16_t mg[3];
		if (!dm1801pro_accel_read(mg)) {
			int16_t id = dm1801pro_accel_id();
			if (id < 0) {
				put(&o, "accel no ack");
			} else {
				put(&o, "accel id ");
				put_x(&o, (uint32_t)id, 2);
			}
			break;
		}
		put(&o, "acc ");
		for (int k = 0; k < 3; k++) {
			if (mg[k] < 0) {
				put(&o, "-");
			}
			put_u(&o, (uint32_t)(mg[k] < 0 ? -mg[k] : mg[k]));
			put(&o, k < 2 ? " " : " mg");
		}
		break;
	}
	case 10:
		put(&o, "uart ring ");
		put_u(&o, hrc_host_uart_overruns());
		put(&o, " err ");
		put_u(&o, hrc_host_uart_line_errors());
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void dm1801pro_diag_reset(void)
{
#if CONFIG_DMR
	hrc_dmr_stats_reset();
	hrc_audio_pcm_reset_stats();
#endif
}

#endif
