// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D diagnostics page lines: what the bring-up needs to see (docs/spec/hardware-db25d.md 10): the RF chips' IDs,
// the raw key scan (to place the keys), the ADC channels (the supply voltage), the host UART's overruns, the
// calibration page found and the PA values it gives, the NAND's ID, the storage on it and the settings it holds
#include <stddef.h>
#include "db25d.h"
#include "dmr_db25d.h"
#include "config.h"
#include "platform.h"
#include "settings/settings.h"

#if CONFIG_DIAG

// Lines 0-11 are always there; the storage lines (12-15) follow when it is built in, then the GPS line, then the DMR
// lines. The page lists lines until the first missing one, so each group starts right after the one before
#define DIAG_STORAGE_LINES	(CONFIG_NAND_STORAGE ? 4 : 0)
#define DIAG_GPS_LINE		(12 + DIAG_STORAGE_LINES)
#define DIAG_DMR_FIRST		(DIAG_GPS_LINE + 1)

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

#if CONFIG_DMR
static void put_i(out_t *o, int32_t v)
{
	if (v < 0) {
		put(o, "-");
		v = -v;
	}
	put_u(o, (uint32_t)v);
}
#endif

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
		// 0x67's 9 bits, 0x65's 7 and the sub-audio status 0x0C (b10 CTCSS, b14 DCS, b15 inverted DCS)
		put(&o, "rssi ");
		put_x(&o, d.rssi_raw & 0x1FFu, 3);
		put(&o, " n ");
		put_x(&o, d.noise_raw & 0x7Fu, 2);
		put(&o, " c ");
		put_x(&o, d.css_raw, 4);
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
	case 8:
		// The NAND's ID (98 then the device) and how many page reads needed ECC corrections
		put(&o, "nand ");
		if (db25d_nand_ready()) {
			put_x(&o, db25d_nand_id(), 4);
			put(&o, " ecc ");
			put_u(&o, db25d_nand_corrected());
		} else {
			put(&o, "none");
		}
		break;
	case 9: {
		// The mic connector's lines: level and changes seen by the key scan (PTA9 is also the PC link's RXD)
		uint16_t a9, c11;
		uint8_t lv = db25d_mic_lines(&a9, &c11);
		put(&o, "mic a9 ");
		put_u(&o, lv & 1u);
		put(&o, "/");
		put_u(&o, a9);
		put(&o, " c11 ");
		put_u(&o, (lv >> 1) & 1u);
		put(&o, "/");
		put_u(&o, c11);
		put(&o, " ign ");
		put_u(&o, db25d_input_ptt_ignored());
		break;
	}
	case 10: {
		// The hand mic's last frame on the host UART and how many distinct frames were logged
		uint8_t b[6];
		uint8_t n = db25d_miccap_last(b, sizeof(b));
		put(&o, "mrx ");
		put_u(&o, db25d_miccap_count());
		put(&o, ":");
		for (uint8_t k = 0; k < n; k++) {
			put(&o, " ");
			put_x(&o, b[k], 2);
		}
		break;
	}
	case 11:
		// The TX drive a keyup at the tuned frequency would use, low / high (0: TX refused here), and the TX state
		put(&o, "tx ");
		put_u(&o, d.hz / 1000u);
		put(&o, "k pa ");
		put_u(&o, d.pa_low);
		put(&o, "/");
		put_u(&o, d.pa_high);
		put(&o, d.transmitting ? " ON" : "");
		break;
#if CONFIG_NAND_STORAGE
	case 12:
	case 13:
	case 14: {
		// The storage layer over ODR's NAND range (hardware-db25d.md 6.6): blocks in use, free and bad; then records
		// written, block erases, blocks emptied by garbage collection and pages that read back wrong since boot;
		// then torn pages the mount set aside, blocks retired since boot and the next sequence number
		nand_sectors_stats_t st;
		db25d_flash_stats(&st);
		put(&o, "nst ");
		if (db25d_flash.size == 0) {
			if (st.status == NAND_SECTORS_FOREIGN && i == 12) {
				// Data ODR didn't write in its range: nothing touched, no storage. The physical block and how many
				put(&o, "foreign ");
				put_u(&o, DB25D_NAND_ODR_FIRST + st.foreign_block);
				put(&o, " n");
				put_u(&o, st.foreign);
			} else if (i == 12) {
				put(&o, "none");
			} else if (i == 13) {
				// The mount status (nand_sectors_status_t): 0 never mounted (no NAND), 2 bad set-up, 3 foreign
				put(&o, "st");
				put_u(&o, st.status);
			} else {
				put(&o, "-");
			}
		} else if (i == 12) {
			put(&o, "u");
			put_u(&o, st.blocks_used);
			put(&o, " f");
			put_u(&o, st.blocks_free);
			put(&o, " bad");
			put_u(&o, st.blocks_bad);
		} else if (i == 13) {
			put(&o, "w");
			put_u(&o, st.writes);
			put(&o, " e");
			put_u(&o, st.erases);
			put(&o, " gc");
			put_u(&o, st.gc_blocks);
			put(&o, " x");
			put_u(&o, st.verify_fails);
		} else {
			put(&o, "t");
			put_u(&o, st.torn);
			put(&o, " r");
			put_u(&o, st.retired);
			put(&o, " #");
			put_u(&o, st.seq);
		}
		break;
	}
	case 15: {
		// The settings: how they were loaded at boot, the newest record's number, saves and failed saves since boot
		static const char *const load[] = { "ram", "dflt", "load", "upgr", "frgn" };
		settings_stats_t st;
		settings_get_stats(&st);
		put(&o, "set ");
		put(&o, st.load < sizeof(load) / sizeof(load[0]) ? load[st.load] : "?");
		put(&o, " #");
		put_u(&o, st.seq);
		put(&o, " s");
		put_u(&o, st.saves);
		put(&o, " f");
		put_u(&o, st.failures);
		break;
	}
#endif
	default:
		if (i == DIAG_GPS_LINE) {
			// The GPS module on UART2 (platform/hrc7000/gps_hrc.c): heard, valid / bad sentences, bytes received. "b0"
			// with the module powered: the RXD pad's mux or the power pin's level (GPS_POWER_ON_LEVEL)
#if CONFIG_GPS
			uint32_t good, bad, n;
			hrc_gps_counts(&good, &bad, &n);
			put(&o, hrc_gps_present() ? "gps yes " : "gps no ");
			put_u(&o, good);
			put(&o, "/");
			put_u(&o, bad);
			put(&o, " b");
			put_u(&o, n);
			put(&o, hrc_gps_running() ? " on" : " off");
#else
			put(&o, "gps not built");
#endif
			break;
		}
#if CONFIG_DMR
		// The DMR modem counters, then the codec in the slot and the AF phase range the modem saw
		// (a value that stays within a few counts means no audio reaches the modem's ADC, modem-hrc7000.md 2.1)
		if (i >= DIAG_DMR_FIRST && i < DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES) {
			return hrc_dmr_stats_line((uint8_t)(i - DIAG_DMR_FIRST), buf, len);
		}
		if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES) {
			uint32_t ph = db25d_dmr_phase();
			put(&o, "cdc ");
			if (hrc_vocoder_tag() != 0) {
				put_u(&o, hrc_vocoder_tag());
			} else {
				put(&o, "none");
			}
			put(&o, " ph ");
			put_i(&o, (int32_t)(ph << 2) >> 18);		// b29:16, a 14-bit signed value
			put(&o, "/");
			put_i(&o, (int32_t)(ph << 18) >> 18);		// b13:0
			break;
		}
#if DB25D_DMR_TX
		if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES + 1) {
			// DMR TX bursts (4.5): TX-on and TX-off sequences run, RF RX interrupts seen, what woke the last TX-off
			// (R: RF RX interrupt, T: slot tick, A: abort), and how many ramp downs started late, the last by how much
			db25d_dmr_tx_stats_t t;
			db25d_dmr_tx_stats(&t);
			put(&o, "b");
			put_u(&o, t.bursts);
			put(&o, "/");
			put_u(&o, t.ends);
			put(&o, " rx");
			put_u(&o, t.rfrx_ints);
			put(&o, t.last_by == 'R' ? " R" : t.last_by == 'T' ? " T" : t.last_by == 'A' ? " A" : " -");
			put(&o, " L");
			put_u(&o, t.late);
			put(&o, "/");
			put_u(&o, t.last_late_us);
			break;
		}
#endif
#endif
		return false;
	}
	*o.p = '\0';
	return true;
}

void db25d_diag_reset(void)
{
	db25d_mic_lines_reset();
	db25d_miccap_arm();
#if CONFIG_DMR
	hrc_dmr_stats_reset();
#endif
}

#endif
