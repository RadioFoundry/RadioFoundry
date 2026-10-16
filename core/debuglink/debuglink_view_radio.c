// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio features for the debug link (docs/spec/debuglink.md 3.7, 3.8): the "features" view section (scan, FM
// extras, battery, beeps, settings) and the RADIO operations that drive them (scan, beep, tone burst, one setting,
// roaming)
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/beep.h"

static const char *word(uint8_t v, const char *const *names, uint8_t n)
{
	return v < n ? names[v] : "?";
}

void debuglink_view_radio(const radio_view_t *v, dl_text_t *t)
{
	static const char *const kinds[] = { "off", "zone", "vfo", "dual" };
	static const char *const phases[] = { "listen", "probe", "hold", "resume", "paused" };
	static const char *const batt[] = { "ok", "low", "critical" };
	static const char *const resumes[] = { "time", "carrier", "stop" };
	static const char *const bcls[] = { "off", "carrier", "wrong_tone" };
	static const char *const voxes[] = { "off", "channel", "all" };
	radio_settings_t s;

	dl_text(t, "scan.built=%u\n", CONFIG_SCAN);
	dl_text(t, "scan=%s\nscan_phase=%s\nscan_skipped=%u\n", word(v->scan, kinds, 4), word(v->scan_phase, phases, 5),
		v->scan_skipped);
	dl_text(t, "scan_probes=%u\nscan_synced=%u\n", v->scan_probes, v->scan_synced);
	dl_text(t, "tot_warning=%u\ntone_burst=%u\nroger=%u\nvox_keyed=%u\nrx_sleeping=%u\n", v->tot_warning,
		v->tone_burst, v->roger, v->vox_keyed, v->rx_sleeping);
	dl_text(t, "vox_on=%u\nvox.mic=%u\n", v->vox_on, board.power != NULL && board.power->mic_level != NULL);
	// whether battery save can switch the receiver off on this radio (a radio driver without rx_sleep never sleeps)
	dl_text(t, "rx_save.hw=%u\n", board.radio != NULL && board.radio->rx_sleep != NULL);
	dl_text(t, "battery=%s\nbattery_pct=%u\nbattery_mv=%u\npower_off=%u\n", word(v->battery, batt, 3),
		v->battery_pct, v->battery_mv, v->power_off);
#if CONFIG_BEEP
	beep_stats_t b;
	beep_get_stats(&b);
	dl_text(t, "beep.built=1\nbeep.played=%u\nbeep.no_stream=%u\nbeep.blocks=%u\nbeep.stalls=%u\nbeep.busy=%u\n",
		(unsigned)b.played, (unsigned)b.no_stream, (unsigned)b.blocks, (unsigned)b.stalls, beep_busy());
#else
	dl_text(t, "beep.built=0\n");
#endif
	radio_get_settings(&s);
	dl_text(t, "set.tot_s=%u\nset.tot_warn_s=%u\nset.bcl=%s\nset.roger_beep=%u\nset.tx_inhibit=%u\n", s.tot_s,
		s.tot_warn_s, word(s.bcl, bcls, 3), s.roger_beep, s.tx_inhibit);
	dl_text(t, "set.scan_resume=%s\nset.scan_hold_s=%u\nset.scan_delay_ms=%u\nset.scan_dwell_ms=%u\n",
		word(s.scan_resume, resumes, 3), s.scan_hold_s, s.scan_delay_ms, s.scan_dwell_ms);
	dl_text(t, "set.scan_priority=%u\nset.beep_level=%u\nset.key_beep=%u\n", s.scan_priority, s.beep_level,
		s.key_beep);
	dl_text(t, "set.vox=%s\nset.vox_level=%u\nset.vox_tail_ms=%u\n", word(s.vox, voxes, 3), s.vox_level,
		s.vox_tail_ms);
	dl_text(t, "set.rx_save=%u\nset.rx_save_idle_s=%u\nset.battery_off_s=%u\n", s.rx_save, s.rx_save_idle_s,
		s.battery_off_s);
	if (s.reply_s <= 30) {
		dl_text(t, "set.reply_s=%u\n", s.reply_s);
	} else {
		dl_text(t, "set.reply_s=codeplug\n");
	}
}

// One setting by id (DL radio op SETTING; docs/spec/debuglink.md 3.8)
static bool set_one(radio_settings_t *s, uint8_t id, uint16_t v)
{
	switch (id) {
	case 1: s->tot_s = v; break;
	case 2: s->tot_warn_s = (uint8_t)v; break;
	case 3: if (v > RADIO_BCL_WRONG_TONE) return false; s->bcl = (uint8_t)v; break;
	case 4: s->roger_beep = v != 0; break;
	case 5: s->tx_inhibit = v != 0; break;
	case 6: if (v > 2) return false; s->scan_resume = (uint8_t)v; break;
	case 7: s->scan_hold_s = (uint8_t)v; break;
	case 8: s->scan_delay_ms = v; break;
	case 9: s->scan_dwell_ms = v; break;
	case 10: s->scan_priority = v != 0; break;
	case 11: if (v > 100) return false; s->beep_level = (uint8_t)v; break;
	case 12: s->key_beep = v != 0; break;
	case 13: if (v > RADIO_VOX_ALL) return false; s->vox = (uint8_t)v; break;
	case 14: if (v > 10) return false; s->vox_level = (uint8_t)v; break;
	case 15: s->vox_tail_ms = v; break;
	case 16: if (v > 3) return false; s->rx_save = (uint8_t)v; break;
	case 17: s->rx_save_idle_s = v; break;
	case 18: s->battery_off_s = v; break;
	case 19: s->scan_beep = v != 0; break;
	case 20: if (v > 30 && v != RADIO_REPLY_CODEPLUG) return false; s->reply_s = (uint8_t)v; break;
	case 21: if ((int16_t)v < -125 || (int16_t)v > -60) return false; s->roam_rssi_dbm = (int8_t)(int16_t)v; break;
	case 22: if (v < 1 || v > 30) return false; s->roam_hyst_db = (uint8_t)v; break;
	case 23: if (v > 1) return false; s->roam_probe = (uint8_t)v; break;
	case 24: if (v != 0 && (v < 30 || v > 3600)) return false; s->roam_check_s = v; break;
	case 25: if (v != 0 && (v < 10 || v > 600)) return false; s->roam_beacon_s = v; break;
	case 26: if (v < 1 || v > 10) return false; s->roam_beacon_len_s = (uint8_t)v; break;
	case 27: if (v > 2) return false; s->roam_survey = (uint8_t)v; break;
	case 28: s->rpt_beacon = v != 0; break;
	default: return false;
	}
	return true;
}

uint8_t debuglink_radio_feature_op(uint8_t op, const uint8_t *a, uint16_t len)
{
	switch (op) {
	case DL_OP_SCAN:
		if (len < 1 || a[0] > RSCAN_TOGGLE) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_SCAN) {
			return DL_ST_UNAVAILABLE;
		}
		if (a[0] == RSCAN_DUAL) {
			if (len < 3) {
				return DL_ST_BAD_ARG;
			}
			radio_req_scan_dual((uint16_t)(a[1] | (a[2] << 8)));
		} else {
			radio_req_scan(a[0]);
		}
		return DL_ST_OK;
	case DL_OP_SCAN_VFO:
		if (len < 8) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_SCAN) {
			return DL_ST_UNAVAILABLE;
		}
		radio_req_scan_vfo((uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24),
				   (uint32_t)a[4] | ((uint32_t)a[5] << 8) | ((uint32_t)a[6] << 16) | ((uint32_t)a[7] << 24));
		return DL_ST_OK;
	case DL_OP_BEEP:
		if (len < 1 || a[0] == BEEP_NONE || a[0] >= BEEP_COUNT) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_BEEP) {
			return DL_ST_UNAVAILABLE;
		}
		radio_req_beep(a[0]);
		return DL_ST_OK;
	case DL_OP_TONE_BURST:
		if (len < 1) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_FM_TX_TONES) {
			return DL_ST_UNAVAILABLE;
		}
		radio_req_tone_burst(a[0] != 0);
		return DL_ST_OK;
	case DL_OP_ROAM:
		if (len < 1 || a[0] > RROAM_LOCK_TOGGLE) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_DMR_ROAMING) {
			return DL_ST_UNAVAILABLE;
		}
		radio_req_roam(a[0]);
		return DL_ST_OK;
	case DL_OP_SETTING: {
		if (len < 3) {
			return DL_ST_BAD_ARG;
		}
		radio_settings_t s;
		radio_get_settings(&s);
		if (!set_one(&s, a[0], (uint16_t)(a[1] | (a[2] << 8)))) {
			return DL_ST_BAD_ARG;
		}
		radio_set_settings(&s);
		return DL_ST_OK;
	}
	default:
		return DL_ST_BAD_ARG;
	}
}
