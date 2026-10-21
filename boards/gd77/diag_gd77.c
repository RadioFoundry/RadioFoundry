// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 diagnostics page: the raw readings the first radio session needs to settle the open pin roles
// (docs/spec/hardware-gd77.md 11): key bits, the battery channel, the chips' answers, the clock
#include <stdio.h>
#include "gd77.h"
#include "dmr_gd77.h"
#include "config.h"
#include "platform.h"
#include "ftfa_math.h"
#include "cal_blocks_gd77.h"

#if CONFIG_DIAG
bool gd77_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	gd77_radio_diag_t d;
	gd77_radio_diag(&d);
	switch (i) {
	case 0:
		snprintf(buf, len, "keys %06lx", (unsigned long)gd77_key_raw());	// column * 5 + row; direct 20-23
		break;
	case 1:
		snprintf(buf, len, "bat raw %u", gd77_battery_raw());
		break;
	case 2:
		snprintf(buf, len, "1846 %s id %04x err %lu", d.rf_ok ? "ok" : "--", d.rf_id, (unsigned long)d.rf_errors);
		break;
	case 3:
		snprintf(buf, len, "c6000 %s", d.c6k_ok ? "ok" : "--");
		break;
	case 4:
		snprintf(buf, len, "rssi %u noise %u", d.rssi, d.noise);
		break;
	case 5:
		snprintf(buf, len, "nor %06lx", (unsigned long)gd77_flash_jedec());
		break;
	case 6:
		snprintf(buf, len, "clk %lu %s%s rst %s", (unsigned long)(SystemCoreClock / 1000u),
			 k22f_clock_on_pll() ? "pll" : "fll", k22f_clock_hsrun() ? " hs" : "",
			 k22f_reset_name(k22f_reset_cause()));
		break;
	case 7:
		// Bus status per bus_cfg.h (I2C A, I2C B, NOR, LCD, C6000): 0 ok, 3 = choice not implemented
		snprintf(buf, len, "bus %d%d%d%d%d", gd77_bus_status(0), gd77_bus_status(1), gd77_bus_status(2),
			 gd77_bus_status(3), gd77_bus_status(4));
		break;
	case 8: {
		// EEPROM answer, calibration block state per band (UHF, VHF: o ok, f read failed, b blank, r rejected) and
		// NOR writes refused below 1 MB
		static const char st[] = "-ofbr";
		snprintf(buf, len, "eep %s cal %c%c nor- %lu", gd77_eeprom_present() ? "ok" : "--",
			 st[gd77_cal_state(GD77_CAL_UHF)], st[gd77_cal_state(GD77_CAL_VHF)],
			 (unsigned long)gd77_flash_refusals());
		break;
	}
	case 9: {	// the RF side: DAC0 drive, transmit built, refusals
		snprintf(buf, len, "tx %s dac %u ref %lu", d.tx_build ? "built" : "off", d.drive,
			 (unsigned long)d.tx_refused);
		break;
	}
	case 10: {	// PCM stream: 20 ms blocks, dropped, transmit underruns, I2S errors
#if CONFIG_BEEP || CONFIG_DMR
		gd77_audio_stats_t a;
		gd77_audio_stats(&a);
		snprintf(buf, len, "pcm %s %lu d%lu u%lu e%lu", a.running ? "on" : "off", (unsigned long)a.blocks,
			 (unsigned long)a.dropped, (unsigned long)a.underruns, (unsigned long)a.i2s_errors);
#else
		snprintf(buf, len, "pcm -");
#endif
		break;
	}
	case 11: {	// DMR: slot interrupts (and the other edges dropped), system, RF TX, RF RX interrupts
		gd77_dmr_stats_t m;
		gd77_dmr_stats(&m);
		snprintf(buf, len, "dmr %s ts%lu/%lu si%lu tx%lu rx%lu", m.active ? "on" : "off",
			 (unsigned long)m.slot_ints, (unsigned long)m.slot_dups, (unsigned long)m.sys_ints,
			 (unsigned long)m.rftx_ints, (unsigned long)m.rfrx_ints);
		break;
	}
	case 12: {	// DMR bursts delivered: voice, data and control, CRC errors; voice port errors
		gd77_dmr_stats_t m;
		gd77_dmr_stats(&m);
		snprintf(buf, len, "V%lu D%lu crc%lu vp%lu", (unsigned long)m.rx_voice, (unsigned long)m.rx_data,
			 (unsigned long)m.rx_crc_err, (unsigned long)m.voice_errors);
		break;
	}
	default:
		return false;
	}
	return true;
}
#endif
