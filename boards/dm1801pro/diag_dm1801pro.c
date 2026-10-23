// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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

// "L/M/H 1616/2544/3408": the drive tx() would use at hz (hardware-dm1801pro.md 6.4), or "L/M/H none"
static void put_drive(out_t *o, uint32_t hz)
{
	put(o, "L/M/H ");
	if (!bf_cal_valid()) {
		put(o, "none");
		return;
	}
	for (uint8_t k = 0; k < 3; k++) {
		if (k > 0) {
			put(o, "/");
		}
		put_u(o, dm1801pro_pa_drive(hz, k));
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
		put(&o, " ptt ign ");
		put_u(&o, dm1801pro_input_ptt_ignored());	// PTT lows taken for host serial data (hardware-dm1801pro.md 9.1)
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
	case 8: {
		// The factory's L / M / H drive at a VHF and a UHF frequency (hardware-dm1801pro.md 6.4)
		uint32_t hz = i == 7 ? 145000000u : 435000000u;
		put(&o, i == 7 ? "pa 145 " : "pa 435 ");
		put_drive(&o, hz);
		break;
	}
	case 9:
		// The frequency of the last transmission (the RX frequency before one) and the drive it used, then the
		// L / M / H drive there: the L and M spacing on air can be compared with these (hardware-dm1801pro.md 6.4)
		dm1801pro_radio_diag(&d);
		put(&o, "pa ");
		put_u(&o, d.pa_hz / 1000u);
		put(&o, "k dac ");
		put_u(&o, d.pa_drive);
		break;
	case 10:
		dm1801pro_radio_diag(&d);
		put(&o, "pa tx ");
		put_drive(&o, d.pa_hz);
		break;
	case 11: {
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
	case 12:
		put(&o, "uart ring ");
		put_u(&o, hrc_host_uart_overruns());
		put(&o, " err ");
		put_u(&o, hrc_host_uart_line_errors());
		break;
	case 13:
		// The modem's audio routing as it is now: compares what an FM transmission sets up (APRS AFSK)
		dm1801pro_radio_diag(&d);
		put(&o, "c7 ac ");
		put_x(&o, d.c7_audio, 8);
		put(&o, " wm ");
		put_x(&o, d.c7_work_mode, 2);
		put(&o, " ptt ");
		put_u(&o, d.c7_fm_ptt);
		put(&o, " rf ");
		put_x(&o, d.c7_rf_mode, 8);
		break;
	case 14: case 15: case 16: case 17: {
		// The codec's 64 byte registers, 16 per line (read one byte at a time: a word read returns one lane)
		uint8_t base = (uint8_t)((i - 14u) * 16u);
		put(&o, "cd ");
		put_x(&o, base, 2);
		put(&o, " ");
		for (uint8_t k = 0; k < 16; k++) {
			put_x(&o, dm1801pro_codec_byte((uint8_t)(base + k)), 2);
		}
		break;
	}
#if CONFIG_DMR && CONFIG_VOX
	case 18: {
		// The VOX mic meter on the mic PCM (audio_pcm.c): level 0-100, the last 10 ms half's peak, halves measured,
		// running. Reading this line doesn't start the meter; mic_level() (VOX, the view's vox.level) does
		uint8_t lvl;
		uint16_t pk;
		uint32_t halves;
		bool on;
		hrc_audio_pcm_mic_stats(&lvl, &pk, &halves, &on);
		put(&o, "mic lvl ");
		put_u(&o, lvl);
		put(&o, " pk ");
		put_u(&o, pk);
		put(&o, " n ");
		put_u(&o, halves);
		put(&o, on ? " on" : " off");
		break;
	}
#endif
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
