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
#include "radio/vibe.h"
#include "radio/mic_gain.h"
#include "hal_time.h"
#include "settings/settings.h"

#define DL_SETTING_MIC_GAIN	29u	// SETTING id of the mic gain step (radio/mic_gain.h)

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
	// Dual watch: the two channels ("vfo", or zone index:position), the selected one and the one the receiver is on
	// now; "-" while it is off
	radio_dual_ref_t dual[2];
	uint8_t sel, at;
	bool on = radio_get_dual(dual, &sel, &at);
	for (uint8_t i = 0; i < 2; i++) {
		if (!on) {
			dl_text(t, "dual.%c=-\n", 'a' + i);
		} else if (dual[i].vfo) {
			dl_text(t, "dual.%c=vfo\n", 'a' + i);
		} else {
			dl_text(t, "dual.%c=%u:%u\n", 'a' + i, dual[i].zone, dual[i].pos);
		}
	}
	// receivers: what the radio has; both: the second receiver has the channel not selected now (dual.at is then the
	// main receiver's, always the selected one)
	dl_text(t, "dual.built=%u\ndual.sel=%c\ndual.at=%c\ndual.receivers=%u\ndual.both=%u\n", CONFIG_DUAL_WATCH,
		on ? 'a' + sel : '-', on ? 'a' + at : '-', board.rx2 != NULL ? 2u : 1u, on && v->rx2);
	dl_text(t, "tot_warning=%u\ntone_burst=%u\nroger=%u\nvox_keyed=%u\nrx_sleeping=%u\n", v->tot_warning,
		v->tone_burst, v->roger, v->vox_keyed, v->rx_sleeping);
	bool mic = board.power != NULL && board.power->mic_level != NULL;
	// The hook itself, not the VOX poll's last reading, so the level reads with VOX off (nothing keys). Only where VOX
	// could listen (FM, not a hotspot): a meter that starts on demand (the HR_C7000's mic PCM) then runs in the state
	// VOX would run it in, and reads 0 on its first call
	bool listen = mic && v->ch.mode == RADIO_MODE_FM && !v->hotspot;
	dl_text(t, "vox_on=%u\nvox.mic=%u\nvox.level=%u\n", v->vox_on, mic, listen ? board.power->mic_level() : 0u);
	// whether battery save can switch the receiver off on this radio (a radio driver without rx_sleep never sleeps)
	dl_text(t, "rx_save.hw=%u\n", board.radio != NULL && board.radio->rx_sleep != NULL);
	dl_text(t, "battery=%s\nbattery_pct=%u\nbattery_mv=%u\npower_off=%u\n", board_has_battery() ? word(v->battery, batt, 3) : "none",
		v->battery_pct, v->battery_mv, v->power_off);
	static const char *const vsrc[] = { "none", "knob", "soft", "override" };
	dl_text(t, "volume.soft=%u\nvolume.sent=%d\nvolume.src=%s\nvolume.max=%u\nvolume.step=%u\nvolume.override=%d\n",
		radio_volume_soft(), v->volume, word(v->volume_src, vsrc, 4), RADIO_VOLUME_MAX, v->volume_step, v->volume_override);
#if CONFIG_BEEP
	beep_stats_t b;
	beep_get_stats(&b);
	dl_text(t, "beep.built=1\nbeep.played=%u\nbeep.no_stream=%u\nbeep.blocks=%u\nbeep.stalls=%u\nbeep.busy=%u\n",
		(unsigned)b.played, (unsigned)b.no_stream, (unsigned)b.blocks, (unsigned)b.stalls, beep_busy());
#else
	dl_text(t, "beep.built=0\n");
#endif
#if CONFIG_VIBRATE
	dl_text(t, "vibe.built=1\nvibe.motor=%u\nvibe.played=%u\nvibe.busy=%u\n",
		board.power != NULL && board.power->vibrate != NULL, (unsigned)vibe_played(), vibe_busy());
#else
	dl_text(t, "vibe.built=0\n");
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
	// The step in effect (the UI's setting or the SETTING op's) and whether the board's audio path takes it
	dl_text(t, "set.mic_gain=%u\nset.mic_gain_board=%u\n", radio_mic_gain(), mic_gain_on_board());
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
	case DL_OP_VIBRATE:
		if (len < 1 || a[0] == VIBE_NONE || a[0] >= VIBE_COUNT) {
			return DL_ST_BAD_ARG;
		}
		if (!CONFIG_VIBRATE) {
			return DL_ST_UNAVAILABLE;
		}
		radio_req_vibrate(a[0]);	// played whatever the UI's Vibrate setting; nothing without a motor
		return DL_ST_OK;
	case DL_OP_VOLUME:
		// step 0..RADIO_VOLUME_MAX; RADIO_VOLUME_KNOB (0xFF) gives the knob back. A radio without a knob keeps the
		// step like a UI change (stored); one with a knob holds it in RAM over the knob. No hook: unavailable
		if (len < 1 || (a[0] > RADIO_VOLUME_MAX && a[0] != RADIO_VOLUME_KNOB)) {
			return DL_ST_BAD_ARG;
		}
		if (!radio_volume_settable()) {
			return DL_ST_UNAVAILABLE;
		}
		if (radio_volume_soft()) {
			settings_t *st = settings_begin();
			st->volume = a[0] == RADIO_VOLUME_KNOB ? 0 : radio_volume_stored(a[0]);
			settings_end(hal_millis());
			radio_req_volume(a[0] == RADIO_VOLUME_KNOB ? RADIO_VOLUME_DEFAULT : a[0]);
		} else {
			radio_req_volume_override(a[0] == RADIO_VOLUME_KNOB ? -1 : (int8_t)a[0]);
		}
		return DL_ST_OK;
	case DL_OP_DMR_FIELDS:
		// mode (0xFF = keep, else radio_mode_t), color code (0xFF = keep), timeslot (0 = keep): on a channel they are
		// temporary changes, in the VFO the VFO's own settings. The radio core refuses what it can't do
		if (len < 3 || (a[0] != 0xFF && a[0] > RADIO_MODE_DMR) || (a[1] != 0xFF && a[1] > 15) || a[2] > 2) {
			return DL_ST_BAD_ARG;
		}
#if CONFIG_DMR
		if (a[0] != 0xFF) {
			radio_req_mode(a[0]);
		}
		if (a[1] != 0xFF) {
			radio_req_dmr_color_code(a[1]);
		}
		if (a[2] != 0) {
			radio_req_dmr_timeslot(a[2]);
		}
		return DL_ST_OK;
#else
		return DL_ST_UNAVAILABLE;
#endif
	case DL_OP_VFO_COPY: {
		// Copy to VFO, as the quick menu's tile does: refused with a status when the VFO can't hold the channel
		// (the check is the radio core's), nothing sent then
		switch (radio_req_vfo_copy_checked()) {
		case VFO_COPY_OK:
			return DL_ST_OK;
		case VFO_COPY_NO_MODE:
		case VFO_COPY_OUT_OF_BAND:
			return DL_ST_REFUSED;
		default:
			return DL_ST_UNAVAILABLE;	// no VFO in this build, or already in it
		}
	}
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
		uint16_t v = (uint16_t)(a[1] | (a[2] << 8));
		if (a[0] == DL_SETTING_MIC_GAIN) {	// not a radio_settings_t field: the UI stores it (radio/mic_gain.h)
			if (v > MIC_GAIN_STEPS) {
				return DL_ST_BAD_ARG;
			}
			if (!mic_gain_available()) {
				return DL_ST_UNAVAILABLE;
			}
			radio_set_mic_gain((uint8_t)v);
			return DL_ST_OK;
		}
		radio_settings_t s;
		radio_get_settings(&s);
		if (!set_one(&s, a[0], v)) {
			return DL_ST_BAD_ARG;
		}
		radio_set_settings(&s);
		return DL_ST_OK;
	}
	default:
		return DL_ST_BAD_ARG;
	}
}
