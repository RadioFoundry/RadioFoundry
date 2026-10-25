// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A diagnostics page lines: what the bring-up needs to see: the RF chips' IDs, the raw key scan (to name
// the keys), the ADC channels, the serial chain's outputs, the host UART's overruns, the calibration page and the PA
// values it gives, the NAND, the link to SoC B, the storage and the settings, the GPS and the DMR modem
#include <stddef.h>
#include "gd88.h"
#include "config.h"
#include "platform.h"
#include "settings/settings.h"

#if CONFIG_DIAG

// Lines 0-14 are always there; the storage lines (15-18) follow when it is built in, then the GPS line, then the DMR
// lines. The page lists lines until the first missing one, so each group starts right after the one before
#define DIAG_FIXED_LINES	15
#define DIAG_STORAGE_LINES	(CONFIG_NAND_STORAGE ? 4 : 0)
#define DIAG_GPS_LINE		(DIAG_FIXED_LINES + DIAG_STORAGE_LINES)
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

bool gd88_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	gd88_radio_diag_t d;
	gd88_radio_diag(&d);
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
		put_x(&o, gd88_input_raw(), 5);
		break;
	case 3:
		put(&o, "adc0 ");
		put_u(&o, hrc_adc_read(GD88_ADC_MIC));
		put(&o, " 1 ");
		put_u(&o, hrc_adc_read(GD88_ADC_VOLUME));
		put(&o, " 2 ");
		put_u(&o, hrc_adc_read(GD88_ADC_BATTERY));
		break;
	case 4:
		put(&o, "bat ");
		put_u(&o, gd88_power.battery_mv());
		put(&o, "mV vol ");
		put_u(&o, (uint32_t)gd88_power.volume());
		break;
	case 5:
		put(&o, "rf wr ");
		put_u(&o, d.rf_writes);
		put(&o, " ovr ");
		put_u(&o, hrc_host_uart_overruns());
		break;
	case 6:
		put(&o, "cal ");
		put(&o, gd88_cal_source() == GD88_CAL_FLASH ? "flash #" : gd88_cal_source() == GD88_CAL_NAND ? "nand #" : "none");
		if (gd88_cal_valid()) {
			put_u(&o, gd88_cal_counter());
			put(&o, " c ");
			put_u(&o, gd88_cal_tx_center());
		}
		break;
	case 7:
		// The PA values the page gives at 446 and 146 MHz, high / low
		put(&o, "pa ");
		put_u(&o, gd88_cal_pa(446000000, true));
		put(&o, "/");
		put_u(&o, gd88_cal_pa(446000000, false));
		put(&o, " ");
		put_u(&o, gd88_cal_pa(146000000, true));
		put(&o, "/");
		put_u(&o, gd88_cal_pa(146000000, false));
		break;
	case 8:
		// The NAND's ID (98 then the device) and how many page reads needed ECC corrections
		put(&o, "nand ");
		if (gd88_nand_ready()) {
			put_x(&o, gd88_nand_id(), 4);
			put(&o, " ecc ");
			put_u(&o, gd88_nand_corrected());
		} else {
			put(&o, "none");
		}
		break;
	case 9: {
		// The serial chain's 64 outputs (chain_gd88.c), high word then low
		uint64_t c = gd88_chain_shadow();
		put(&o, "chain ");
		put_x(&o, (uint32_t)(c >> 32), 8);
		put(&o, " ");
		put_x(&o, (uint32_t)c, 8);
		break;
	}
	case 10:
		// Key scans with PTA9 low that the shared-line gate took for serial data
		put(&o, "ptt ign ");
		put_u(&o, gd88_input_ptt_ignored());
		break;
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
	case 12: {
		// SoC B over UART3: up, its protocol version and flags, its firmware; then frames, bad frames, tunes sent and
		// resent, and the UART's overruns and line errors
		gd88_rx2_diag_t r;
		gd88_rx2_diag(&r);
		put(&o, "b ");
		put(&o, r.up ? "up v" : "down v");
		put_u(&o, r.version);
		put(&o, " f");
		put_x(&o, r.flags, 2);
		put(&o, " ");
		put(&o, r.fw);
		break;
	}
	case 13: {
		gd88_rx2_diag_t r;
		gd88_rx2_diag(&r);
		put(&o, "bl ");
		put_u(&o, r.frames);
		put(&o, " x");
		put_u(&o, r.bad);
		put(&o, " t");
		put_u(&o, r.tunes);
		put(&o, "/");
		put_u(&o, r.resent);
		put(&o, " o");
		put_u(&o, r.uart.overruns);
		put(&o, " e");
		put_u(&o, r.uart.line_errors);
		break;
	}
	case 14:
		// The display: the panel variant in use (ili9341.h) and where it came from (the panel record or the default)
		put(&o, "lcd v");
		put_u(&o, gd88_panel_variant());
		put(&o, gd88_panel_from_record() ? " rec" : " dflt");
		break;
#if CONFIG_NAND_STORAGE
	case 15:
	case 16:
	case 17: {
		// The storage layer over RadioFoundry's NAND range : blocks in use, free and bad; then records
		// written, block erases, blocks emptied by garbage collection and pages that read back wrong since boot;
		// then torn pages the mount set aside, blocks retired since boot and the next sequence number
		nand_sectors_stats_t st;
		gd88_flash_stats(&st);
		put(&o, "nst ");
		if (gd88_flash.size == 0) {
			if (st.status == NAND_SECTORS_FOREIGN && i == 15) {
				// Data RadioFoundry didn't write in its range: nothing touched, no storage. The physical block and how many
				put(&o, "foreign ");
				put_u(&o, GD88_NAND_RFOUNDRY_FIRST + st.foreign_block);
				put(&o, " n");
				put_u(&o, st.foreign);
			} else if (i == 15) {
				put(&o, "none");
			} else if (i == 16) {
				// The mount status (nand_sectors_status_t): 0 never mounted (no NAND), 2 bad set-up, 3 foreign
				put(&o, "st");
				put_u(&o, st.status);
			} else {
				put(&o, "-");
			}
		} else if (i == 15) {
			put(&o, "u");
			put_u(&o, st.blocks_used);
			put(&o, " f");
			put_u(&o, st.blocks_free);
			put(&o, " bad");
			put_u(&o, st.blocks_bad);
		} else if (i == 16) {
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
	case 18: {
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
			uint32_t ph = gd88_dmr_phase();
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
#if GD88_TX
		if (i == DIAG_DMR_FIRST + HRC_DMR_DIAG_LINES + 1) {
			// DMR TX bursts: TX-on and TX-off sequences run, RF RX interrupts seen, what woke the last TX-off
			// (R: RF RX interrupt, T: slot tick, A: abort), and how many ramp downs started late, the last by how much
			gd88_dmr_tx_stats_t t;
			gd88_dmr_tx_stats(&t);
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

void gd88_diag_reset(void)
{
#if CONFIG_DMR
	hrc_dmr_stats_reset();
#endif
}

#endif
