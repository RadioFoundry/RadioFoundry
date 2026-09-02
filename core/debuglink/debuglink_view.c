// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio view as key=value text for GET_VIEW (docs/spec/debuglink.md 4.9). One key per line; keys are stable
// (DL_VIEW_VERSION is bumped when one changes meaning or goes away); new keys may appear anywhere in a section.
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "hostlink/hostlink.h"
#include "input/input.h"
#include "ui/ui.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "storage/storage.h"
#if CONFIG_CPS_LEGACY
#include "cps/cps_legacy.h"
#endif
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#if CONFIG_SFR
#include "hotspot/hotspotSfr.h"
#endif
#endif
#if CONFIG_DMR_IP_GATEWAY
#include "hostlink/hostlink_net.h"
#include "hostlink/hostlink_net_air.h"
#include "dmr/ipGateway.h"
#endif

static const char *state_key(uint8_t s)
{
	static const char *const names[] = { "idle", "rx", "tx", "tx_denied" };
	return s < 4 ? names[s] : "?";
}

static const char *deny_key(uint8_t d)
{
	static const char *const names[] = { "none", "rx_only", "band", "timeout", "no_freq", "mode", "no_codec",
					     "no_dmr", "busy", "no_repeater", "rejected", "hotspot", "no_id", "inhibit",
					     "battery" };
	return d < sizeof(names) / sizeof(names[0]) ? names[d] : "?";
}

static void tone(dl_text_t *t, const char *key, const subtone_t *s)
{
	static const char *const types[] = { "none", "ctcss", "dcs", "dcs_inv" };
	dl_text(t, "%s=%s:%u\n", key, s->type < 4 ? types[s->type] : "?", s->value);
}

static void core_section(const radio_view_t *v, dl_text_t *t)
{
	const char *pw = (v->power < board.power_count && board.power_names != NULL) ? board.power_names[v->power] : "?";
	static const char *const sources[] = { "none", "builtin", "legacy" };

	dl_text(t, "view=%u\n", DL_VIEW_VERSION);
	dl_text(t, "state=%s\n", state_key((uint8_t)v->state));
	dl_text(t, "tx_deny=%s\n", deny_key(v->tx_deny));
	dl_text(t, "mode=%s\n", v->ch.mode == RADIO_MODE_DMR ? "dmr" : "fm");
	dl_text(t, "vfo=%u\n", v->vfo);
	dl_text(t, "zone_index=%u\nzone_pos=%u\nzone_size=%u\n", v->zone_index, v->zone_pos, v->zone_size);
	dl_text_kv_str(t, "zone_name", v->zone_name);
	dl_text(t, "channel_number=%u\n", v->channel_number);
	dl_text_kv_str(t, "channel_name", v->ch.name);
	dl_text(t, "rx_hz=%u\ntx_hz=%u\n", (unsigned)v->ch.rx_hz, (unsigned)v->ch.tx_hz);
	dl_text(t, "power=%u\n", v->power);
	dl_text_kv_str(t, "power_name", pw);
	dl_text(t, "power_count=%u\n", board.power_count);
	dl_text(t, "temporary=%u\ntemp=0x%02x\n", v->temporary, v->temp);
	dl_text(t, "squelch=%u\nmonitor=%u\ntone_ok=%u\n", v->squelch, v->monitor, v->tone_ok);
	dl_text(t, "rssi_dbm=%d\nnoise=%u\n", v->rssi_dbm, v->noise);
	dl_text(t, "tx_elapsed_ms=%u\ntot_s=%u\n", (unsigned)v->tx_elapsed_ms, v->tot_s);
	dl_text(t, "last_rx_ms=%u\n", (unsigned)v->last_rx_ms);
	dl_text(t, "vfo_step_hz=%u\n", (unsigned)v->vfo_step_hz);
	dl_text(t, "codeplug=%s\n", v->source < 3 ? sources[v->source] : "?");
	dl_text(t, "hotspot=%u\n", v->hotspot);
	dl_text(t, "serial=%u\n", (unsigned)radio_view_serial());
}

static void channel_section(const radio_view_t *v, dl_text_t *t)
{
	const channel_t *c = &v->ch;

	dl_text_kv_str(t, "ch.name", c->name);
	dl_text(t, "ch.rx_hz=%u\nch.tx_hz=%u\n", (unsigned)c->rx_hz, (unsigned)c->tx_hz);
	dl_text(t, "ch.mode=%s\n", c->mode == RADIO_MODE_DMR ? "dmr" : "fm");
	dl_text(t, "ch.bw=%s\n", c->bw == RADIO_BW_25K ? "25k" : "12k5");
	tone(t, "ch.rx_tone", &c->rx_tone);
	tone(t, "ch.tx_tone", &c->tx_tone);
	if (c->power == CODEPLUG_POWER_DEFAULT) {
		dl_text(t, "ch.power=default\n");
	} else {
		dl_text(t, "ch.power=%u\n", c->power);
	}
	dl_text(t, "ch.rx_only=%u\n", c->rx_only);
	dl_text(t, "ch.color_code=%u\nch.timeslot=%u\n", c->color_code, c->timeslot);
	dl_text(t, "ch.contact=%u\nch.listen_list=%u\n", c->contact, c->listen_list);
	dl_text(t, "ch.dmr_id=%u\nch.dmo=%u\n", (unsigned)c->dmr_id, c->dmo);
}

static void hotspot_section(dl_text_t *t)
{
	static const char *const modes[] = { "idle", "hotspot", "cps" };
	hostlink_mode_t m = hostlink_mode();

	static const char *const leaves[] = { "none", "user", "closed", "gone", "idle" };
	hostlink_leave_t l = hostlink_last_leave();

	dl_text(t, "hostlink=%s\n", (unsigned)m < 3 ? modes[m] : "?");
	dl_text(t, "hostlink.left=%s\n", (unsigned)l < 5 ? leaves[l] : "?");
	dl_text(t, "usb.network=%u\nusb.net_up=%u\n", hostlink_network_mode(),
		(board.netif != NULL) && board.netif->up());
#if CONFIG_DMR_IP_GATEWAY
	hostlink_net_stats_t ns;

	hostlink_net_stats(&ns);
	dl_text(t, "net.host_ip=%08x\nnet.radio_ip=%08x\n", (unsigned)ipGatewayHostIP(), (unsigned)ipGatewayRadioIP());
	dl_text(t, "net.frames_in=%u\nnet.to_air=%u\nnet.to_air_refused=%u\n", (unsigned)ns.frames_in,
		(unsigned)ns.to_air, (unsigned)ns.to_air_refused);
	dl_text(t, "net.packets_air=%u\nnet.packets_up=%u\nnet.packets_dropped=%u\n", (unsigned)ns.packets_air,
		(unsigned)ns.packets_up, (unsigned)ns.packets_dropped);
	hostlink_net_air_stats_t as;

	hostlink_net_air_stats(&as);
	dl_text(t, "net.air_sent=%u\nnet.air_busy=%u\nnet.air_dropped=%u\n", (unsigned)as.sent,
		(unsigned)as.refused_busy, (unsigned)(as.dropped + as.too_long));
#endif
#if CONFIG_HOTSPOT
	hotspot_view_t h;
	hotspot_stats_t s;

	hotspot_get_view(&h);
	hotspot_get_stats(&s);
	dl_text(t, "hs.running=%u\nhs.transmitting=%u\nhs.receiving=%u\n", h.running, h.transmitting, h.receiving);
	dl_text(t, "hs.rx_hz=%u\nhs.tx_hz=%u\nhs.cc=%u\n", (unsigned)h.rx_hz, (unsigned)h.tx_hz, h.cc);
	dl_text(t, "hs.src=%u\nhs.dst=%u\nhs.group=%u\nhs.from_net=%u\n", (unsigned)h.src, (unsigned)h.dst, h.group,
		h.from_net);
	dl_text(t, "hs.net_frames=%u\nhs.net_dropped=%u\nhs.net_calls=%u\n", (unsigned)s.net_frames,
		(unsigned)s.net_dropped, (unsigned)s.net_calls);
	dl_text(t, "hs.rf_bursts=%u\nhs.rf_frames_up=%u\nhs.rf_lost=%u\n", (unsigned)s.rf_bursts,
		(unsigned)s.rf_frames_up, (unsigned)s.rf_lost);
	dl_text(t, "hs.events_dropped=%u\nhs.tx_rejected=%u\n", (unsigned)s.events_dropped, (unsigned)s.tx_rejected);
	dl_text(t, "hs.cwid_requests=%u\nhs.cwid_unsent=%u\nhs.enter_attempts=%u\n", (unsigned)s.cwid_requests,
		(unsigned)s.cwid_unsent, (unsigned)s.enter_attempts);
	dl_text(t, "hs.starts=%u\nhs.stops=%u\n", (unsigned)s.starts, (unsigned)s.stops);
#if CONFIG_SFR
	hotspot_sfr_view_t f;

	hotspot_sfr_get_view(&f);
	dl_text(t, "sfr.on=%u\nsfr.keyed=%u\nsfr.busy=%u\nsfr.slots=%u>%u\n", f.on, f.keyed, f.inbound_busy,
		f.inbound_slot + 1u, 2u - f.inbound_slot);
	dl_text(t, "sfr.rx=%u\nsfr.repeated=%u\nsfr.ignored=%u\nsfr.echo=%u\nsfr.bad_crc=%u\n",
		(unsigned)f.stats.rx_bursts, (unsigned)f.stats.rx_repeated, (unsigned)f.stats.rx_ignored,
		(unsigned)f.stats.rx_echo, (unsigned)f.stats.rx_bad_crc);
	dl_text(t, "sfr.tx_bursts=%u\nsfr.tx_idle=%u\nsfr.tx_starts=%u\nsfr.up=%u\nsfr.up_lost=%u\n",
		(unsigned)f.stats.tx_bursts, (unsigned)f.stats.tx_idle, (unsigned)f.stats.tx_starts,
		(unsigned)f.stats.up_bursts, (unsigned)f.stats.up_lost);
#endif
#else
	dl_text(t, "hs.built=0\n");
#endif
}

static void link_section(dl_text_t *t)
{
	uint32_t c[DL_LINK_COUNTERS];

	debuglink_link_counters(c);
	dl_text(t, "uptime_ms=%u\n", (unsigned)hal_millis());
	dl_text(t, "ui.screen=%s\n", ui_screen_name());
	ui_app_status_t u;
	ui_get_status(&u);
	dl_text(t, "ui.depth=%u\nui.focus=%u\nui.count=%u\nui.locked=%u\n", (unsigned)u.depth, (unsigned)u.focus,
		(unsigned)u.count, (unsigned)u.locked);
	dl_text(t, "ui.banner=%s\nui.toast=%s\nui.query=%s\nui.draws=%u\n", u.banner, u.toast, u.query,
		(unsigned)u.draws);
	dl_text(t, "input.idle_ms=%u\n", (unsigned)input_idle_ms());
	dl_text(t, "input.source=%s\n", input_last_source() == INPUT_SRC_HOST ? "host" : "keys");
	dl_text(t, "input.raw_keys=0x%08x\ninput.injected=0x%08x\n", (unsigned)input_raw_keys(),
		(unsigned)input_injected_keys());
	dl_text(t, "input.encoder_total=%d\n", (int)input_encoder_total());
	dl_text(t, "input.long_ms=%u\n", (unsigned)input_long_ms());
	dl_text(t, "dl.frames=%u\ndl.bad_crc=%u\ndl.too_long=%u\ndl.timeouts=%u\n", (unsigned)c[0], (unsigned)c[1],
		(unsigned)c[2], (unsigned)c[3]);
}

static void storage_section(dl_text_t *t)
{
	static const char *const loads[] = { "ram", "defaults", "loaded", "upgraded", "foreign" };
	static const char *const regions[] = { "none", "ours", "foreign" };
	static const char *const sources[] = { "none", "builtin", "legacy" };
	settings_stats_t ss;
	storage_stats_t st;
	codeplug_source_t src = codeplug_source();

	settings_get_stats(&ss);
	storage_get_stats(&st);
	dl_text(t, "settings.load=%s\nsettings.dirty=%u\nsettings.seq=%u\n", ss.load < 5 ? loads[ss.load] : "?", ss.dirty,
		(unsigned)ss.seq);
	dl_text(t, "settings.saves=%u\nsettings.erases=%u\nsettings.failures=%u\n", (unsigned)ss.saves,
		(unsigned)ss.erases, (unsigned)ss.failures);
	dl_text(t, "settings.sector=%u\nsettings.slot=%u\nsettings.skipped=%u\n", ss.sector, ss.slot, ss.skipped);
	settings_t sv;
	settings_read(&sv);
	dl_text(t, "settings.auto_lock_s=%u\nsettings.long_press_ms=%u\nsettings.repeat_ms=%u\nsettings.mic_gain=%u\n",
		sv.auto_lock_s, sv.long_press_ms, sv.repeat_ms, sv.mic_gain);
	dl_text(t, "settings.theme=%u\nsettings.ui_flags=%u\nsettings.updown_home=%u\nsettings.low_batt_pct=%u\n", sv.theme,
		sv.ui_flags, sv.updown_home, sv.low_batt_pct);
	dl_text(t, "settings.auto_off_min=%u\nsettings.privacy_type=%u\nsettings.privacy_active=%u\n", sv.auto_off_min,
		sv.privacy_type[0], sv.privacy_active);
	dl_text(t, "storage.job_busy=%u\n", storage_job_busy());
	dl_text(t, "storage.sector_writes=%u\nstorage.sector_skips=%u\n", (unsigned)st.sector_writes,
		(unsigned)st.sector_skips);
	dl_text(t, "storage.replays=%u\nstorage.errors=%u\n", (unsigned)st.replays, (unsigned)st.errors);
	storage_region_t rg = storage_region();
	dl_text(t, "storage.region=%s\nstorage.journal=%s\n", (unsigned)rg < 3 ? regions[rg] : "?",
		rg == STORAGE_REGION_FOREIGN ? "foreign" : (rg == STORAGE_REGION_OURS ? "ok" : "none"));
	dl_text(t, "storage.foreign_slots=%u\nstorage.foreign_refusals=%u\nstorage.claim_pending=%u\n",
		st.foreign_slots, (unsigned)st.foreign_refusals, st.claim_pending);
	dl_text(t, "codeplug.source=%s\ncodeplug.serial=%u\n", (unsigned)src < 3 ? sources[src] : "?",
		(unsigned)codeplug_serial());
#if CONFIG_CPS_LEGACY
	cps_stats_t c;

	cps_get_stats(&c);
	dl_text(t, "cps.built=1\ncps.requests=%u\ncps.reads=%u\ncps.commits=%u\n", (unsigned)c.requests,
		(unsigned)c.reads, (unsigned)c.commits);
	dl_text(t, "cps.refused=%u\ncps.overflows=%u\ncps.last=0x%02x\n", (unsigned)c.refused, (unsigned)c.overflows,
		c.last);
	dl_text(t, "cps.session=%u\ncps.pending=%u\n", c.session, c.pending);
#else
	dl_text(t, "cps.built=0\n");
#endif
}

void debuglink_view_section(uint8_t section, const radio_view_t *v, dl_text_t *t)
{
	static const char *const names[DL_VIEW_COUNT] = { "core", "channel", "dmr", "dmr_call", "dmr_stats", "hotspot",
							  "link", "features", "dmr_data", "storage", "dmr_ext",
							  "dmr_data_stats", "roam" };

	if (section >= DL_VIEW_COUNT) {
		return;
	}
	dl_text(t, "section=%s\n", names[section]);
	switch (section) {
	case DL_VIEW_CORE:
		core_section(v, t);
		break;
	case DL_VIEW_CHANNEL:
		channel_section(v, t);
		break;
	case DL_VIEW_DMR:
		debuglink_view_dmr(0, v, t);
		break;
	case DL_VIEW_DMR_CALL:
		debuglink_view_dmr(1, v, t);
		break;
	case DL_VIEW_DMR_STATS:
		debuglink_view_dmr(2, v, t);
		break;
	case DL_VIEW_HOTSPOT:
		hotspot_section(t);
		break;
	case DL_VIEW_LINK:
		link_section(t);
		break;
	case DL_VIEW_FEATURES:
		debuglink_view_radio(v, t);
		break;
	case DL_VIEW_DMR_DATA:
		debuglink_view_dmr_data(0, t);
		break;
	case DL_VIEW_STORAGE:
		storage_section(t);
		break;
	case DL_VIEW_DMR_EXT:
		debuglink_view_dmr_ext(t);
		break;
	case DL_VIEW_DMR_DATA_STATS:
		debuglink_view_dmr_data(1, t);
		break;
	case DL_VIEW_ROAM:
		debuglink_view_roam(v, t);
		break;
	default:
		break;
	}
}
