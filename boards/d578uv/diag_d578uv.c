// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV diagnostics page lines: the raw readings the bring-up needs (docs/testing/d578uv-bringup.md): the
// baseband's boot result, the key ladders and microphone lines (to place the keys), the supply and volume pots, the
// NAND's ID, the DMR PA gate's bursts, the GPS receiver's sentences, whether the KTMicro receiver answered, side A's
// raw RSSI and the serial hand microphone's frames. Each line keeps its number whatever is built. The gps and rssi lines are in the shape the HIL health tests
// read on every radio (tests/hil/test_health.py)
#include <stddef.h>
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#include "config.h"
#if CONFIG_GPS
#include "gps_uart/gps_uart.h"
#endif

#if CONFIG_DIAG

#define DIAG_LINES	10u

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

#if CONFIG_DMR
// sct_boot_result_t in order (drivers/modem/sct/sct_host.h)
static const char *boot_name(uint8_t r)
{
	static const char *const names[] = { "ok", "no block", "bus", "no app", "silent", "no voice" };
	return r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}
#endif

bool d578uv_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0 || i >= DIAG_LINES) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	switch (i) {
	case 0:
		put(&o, "bb ");
#if CONFIG_DMR
		put(&o, boot_name(d578uv_modem_boot_result()));
#else
		put(&o, "-");
#endif
		break;
	case 1:
		// The ladders' 12-bit samples (4095: no key), PA1 and the microphone lines PA9, PA10: 1 = pulled low
		put(&o, "keys ");
		put_u(&o, mcu_adc_read(ADC_KEY_LADDER0));
		put(&o, " ");
		put_u(&o, mcu_adc_read(ADC_KEY_LADDER1));
		put(&o, " a1 ");
		put_u(&o, !mcu_pin_read(PIN_KEY_SINGLE));
		put(&o, " mic ");
		put_u(&o, !mcu_pin_read(PIN_MIC_LINE9));
		put_u(&o, !mcu_pin_read(PIN_MIC_LINE10));
		break;
	case 2:
		put(&o, "sup ");
		put_u(&o, d578uv_power.supply_mv());
		put(&o, " ign ");
		put_u(&o, mcu_adc_read(ADC_IGNITION_SENSE));
		break;
	case 3:
		put(&o, "vol ");
		put_u(&o, mcu_adc_read(ADC_VOL_A));
		put(&o, " ");
		put_u(&o, mcu_adc_read(ADC_VOL_B));
		break;
	case 4:
		put(&o, "nand ");
		put_x(&o, d578uv_nand_id(), 4);
		break;
	case 5: {
		// Bursts the gate switched the PA on for, and key-ups refused with the burst line already high
		put(&o, "burst ");
#if CONFIG_DMR
		uint32_t bursts, refused;
		d578uv_pa_counts(&bursts, &refused);
		put_u(&o, bursts);
		put(&o, " ref ");
		put_u(&o, refused);
#else
		put(&o, "-");
#endif
		break;
	}
	case 6: {
#if CONFIG_GPS
		uint32_t good, bad, bytes;
		mcu_gps_counts(&good, &bad, &bytes);
		put(&o, mcu_gps_present() ? "gps yes " : "gps no ");
		put_u(&o, good);
		put(&o, "/");
		put_u(&o, bad);
		put(&o, " b");
		put_u(&o, bytes);
#else
		put(&o, "gps not built");
#endif
		break;
	}
	case 7:
		put(&o, "kt ");
#if CONFIG_AM_RX || CONFIG_BCAST_FM
		put(&o, d578uv_kt_present() ? "yes" : "no");
#else
		put(&o, "-");
#endif
		break;
	case 8: {
		// Chip 0's raw RSSI and noise as the radio task last read them, and its signal flag; a dead bus reads a constant
		uint16_t rssi, noise;
		bool signal;
		d578uv_rx_raw(&rssi, &noise, &signal);
		put(&o, "rssi ");
		put_u(&o, rssi);
		put(&o, " nz ");
		put_u(&o, noise);
		put(&o, signal ? " sig" : " -");
		break;
	}
	default: {
		// The serial hand microphone: present, its frames accepted, bytes dropped and the key code held (hex)
		bool present;
		uint32_t frames, errors;
		uint8_t key;
		d578uv_hand_mic(&present, &frames, &errors, &key);
		put(&o, present ? "hmic yes " : "hmic no ");
		put_u(&o, frames);
		put(&o, "/");
		put_u(&o, errors);
		put(&o, " k");
		put_x(&o, key, 2);
		break;
	}
	}
	*o.p = '\0';
	return true;
}

void d578uv_diag_reset(void)
{
#if CONFIG_DMR
	d578uv_pa_counts_reset();
#endif
}

#endif
