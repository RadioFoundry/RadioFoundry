// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D diagnostics page lines: what the bring-up needs to see (docs/spec/hardware-db25d.md 10): the RF chips' IDs,
// the raw key scan (to place the keys), the ADC channels (the supply voltage), the host UART's overruns, the
// calibration page found and the PA values it gives
#include <stddef.h>
#include "db25d.h"
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

bool db25d_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	db25d_radio_diag_t d;
	db25d_radio_diag(&d);
	switch (i) {
	case 0:
		put(&o, "rf ");
		put_x(&o, d.main_id, 4);
		put(&o, " lo ");
		put_x(&o, d.lo_id, 4);
		break;
	case 1:
		put(&o, "rssi ");
		put_x(&o, d.rssi_raw, 4);
		put(&o, " n ");
		put_x(&o, d.noise_raw, 4);
		break;
	case 2:
		put(&o, "keys ");
		put_x(&o, db25d_input_raw(), 5);
		break;
	case 3:
		put(&o, "adc0 ");
		put_u(&o, db25d_adc_raw(0));
		put(&o, " 2 ");
		put_u(&o, db25d_adc_raw(2));
		break;
	case 4:
		put(&o, "adc6 ");
		put_u(&o, db25d_adc_raw(6));
		put(&o, " 7 ");
		put_u(&o, db25d_adc_raw(7));
		break;
	case 5:
		put(&o, "rf wr ");
		put_u(&o, d.rf_writes);
		put(&o, " ovr ");
		put_u(&o, hrc_host_uart_overruns());
		break;
	case 6:
		put(&o, "cal ");
		put(&o, db25d_cal_source() == DB25D_CAL_FLASH ? "flash #" : db25d_cal_source() == DB25D_CAL_NAND ? "nand #" : "none");
		if (db25d_cal_valid()) {
			put_u(&o, db25d_cal_counter());
			put(&o, " c ");
			put_u(&o, db25d_cal_tx_center());
		}
		break;
	case 7:
		// The PA values the radio would use at 446 and 146 MHz, high / low: what to check against the stock radio
		put(&o, "pa ");
		put_u(&o, db25d_cal_pa(446000000, true));
		put(&o, "/");
		put_u(&o, db25d_cal_pa(446000000, false));
		put(&o, " ");
		put_u(&o, db25d_cal_pa(146000000, true));
		put(&o, "/");
		put_u(&o, db25d_cal_pa(146000000, false));
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void db25d_diag_reset(void)
{
}

#endif
