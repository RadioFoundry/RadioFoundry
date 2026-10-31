// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio view as key=value text for GET_VIEW (docs/spec/debuglink.md 3.7). One key per line; keys are stable
// (DL_VIEW_VERSION is bumped when one changes meaning or goes away); new keys may appear anywhere in a section.
#include <string.h>
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "hostlink/hostlink.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/i18n.h"
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "settings/settings.h"
#include "radio/ref_trim.h"
#include "storage/storage.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif
#if CONFIG_NET_USRP
#include "usbaudio/usrp_net.h"
#endif
#if CONFIG_CPS_OPENFW
#include "cps/cps_openfw.h"
#endif
#if CONFIG_CPS_FAST
#include "cps/cps_fast.h"
#endif
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#if CONFIG_SFR
#include "hotspot/hotspotSfr.h"
#endif
#endif
#if CONFIG_SFR
#include "radio/radio_sfr.h"
#endif
#if CONFIG_DMR_IP_GATEWAY
#include "hostlink/hostlink_net.h"
#include "hostlink/hostlink_net_air.h"
#include "dmr/ipGateway.h"
#if CONFIG_NET_PPP
#include "net/ppp.h"
#endif
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
					     "battery", "am", "bcast" };
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
	static const char *const sources[] = { "none", "builtin", "open-firmware" };

	dl_text(t, "view=%u\n", DL_VIEW_VERSION);
	dl_text(t, "state=%s\n", state_key((uint8_t)v->state));
	dl_text(t, "tx_deny=%s\n", deny_key(v->tx_deny));
	dl_text(t, "can_tx=%u\n", board_can_tx() ? 1u : 0u);
	dl_text(t, "mode=%s\n", v->ch.mode == RADIO_MODE_DMR ? "dmr" : "fm");
	dl_text(t, "am=%u\nbcast=%u\nbcast_fault=%u\nbcast_khz=%u\n", v->am, v->bcast, v->bcast_fault, (unsigned)v->bcast_khz);
	dl_text(t, "vfo=%u\nvfo_index=%u\n", v->vfo, v->vfo_index);
	dl_text(t, "zone_index=%u\nzone_pos=%u\nzone_size=%u\nzone_count=%u\n", v->zone_index, v->zone_pos, v->zone_size,
		(unsigned)codeplug_zone_count());
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
	dl_text(t, "last_rx_ms=%u\nlast_tx_ms=%u\ntx_auto=%u\nwakes=%u\n", (unsigned)v->last_rx_ms,
		(unsigned)v->last_tx_ms, v->tx_auto, radio_view_wakes(v));
	dl_text(t, "vfo_step_hz=%u\n", (unsigned)v->vfo_step_hz);
	dl_text(t, "codeplug=%s\n", v->source < 3 ? sources[v->source] : "?");
	dl_text(t, "hotspot=%u\n", v->hotspot);
	uint32_t handled, dropped;
	radio_req_stats(&handled, &dropped);
	dl_text(t, "req_handled=%u\nreq_dropped=%u\nch_read_errors=%u\n", (unsigned)handled, (unsigned)dropped,
		(unsigned)codeplug_channel_errors());
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
	// The second receiver while it has dual watch's other channel (v->rx2): a signal or a call for us there, and the
	// DMR call's source. A HIL test sees a call on that channel here: the main receiver never goes there
	rx2_status_t r2;
	memset(&r2, 0, sizeof(r2));
	if (v->rx2 && board.rx2 != NULL) {
		board.rx2->status(&r2);
	}
	dl_text(t, "rx2.active=%u\nrx2.src=%u\n", r2.tuned && r2.active, (unsigned)r2.src);
}

// mtu_lines and ppp_lines are not inlined: net_link_section is, into debuglink_view_section, which is on the host
// task's deepest path (docs/dev/stack-check.md), and their locals would add to it
#if CONFIG_DMR_IP_GATEWAY
// MTUs (usb-network.md 3.2): the link's (DHCP option 26), the air's (one transmission; the ICMP next-hop MTU)
__attribute__((noinline)) static void mtu_lines(dl_text_t *t)
{
	ipGatewayStats_t gs;

	ipGatewayGetStats(&gs);
	dl_text(t, "net.mtu=%u\nnet.air_mtu=%u\nnet.air_too_big=%u\nnet.frag_needed=%u\nnet.up_too_big=%u\n",
		(unsigned)IPGW_LINK_MTU, (unsigned)IPGW_AIR_MTU, (unsigned)gs.airTooBig, (unsigned)gs.fragNeeded,
		(unsigned)gs.upTooBig);
}
#endif

#if CONFIG_NET_PPP
// The PPP link on the serial host port (docs/spec/ppp.md): LCP / IPCP states as RFC 1661 numbers them (9 open), the
// MRU each way (ppp.md 2)
__attribute__((noinline)) static void ppp_lines(dl_text_t *t)
{
	ppp_stats_t ps;

	ppp_stats(&ps);
	dl_text(t, "ppp.running=%u\nppp.ip_up=%u\nppp.lcp=%u\nppp.ipcp=%u\n", ppp_running(), ppp_ip_up(), ps.lcp, ps.ipcp);
	dl_text(t, "ppp.rx_frames=%u\nppp.tx_frames=%u\nppp.rx_errors=%u\n", (unsigned)ps.rx_frames,
		(unsigned)ps.tx_frames, (unsigned)ps.rx_errors);
	dl_text(t, "ppp.mru=%u\nppp.peer_mru=%u\n", ps.mru, ps.peer_mru);
}
#endif

// Its own section: with them the hotspot section outgrew a reply on radios with PPP (truncated=1 before hs.starts)
static void net_link_section(dl_text_t *t)
{
#if CONFIG_DMR_IP_GATEWAY
	mtu_lines(t);
#endif
#if CONFIG_NET_PPP
	ppp_lines(t);
#endif
#if !CONFIG_DMR_IP_GATEWAY && !CONFIG_NET_PPP
	dl_text(t, "net_link.built=0\n");
#endif
}

static void hotspot_section(dl_text_t *t)
{
	static const char *const modes[] = { "idle", "hotspot", "cps", "tnc" };
	hostlink_mode_t m = hostlink_mode();

	static const char *const leaves[] = { "none", "user", "closed", "gone", "idle" };
	hostlink_leave_t l = hostlink_last_leave();

	dl_text(t, "hostlink=%s\n", (unsigned)m < 4 ? modes[m] : "?");
	dl_text(t, "hostlink.left=%s\n", (unsigned)l < 5 ? leaves[l] : "?");
	dl_text(t, "usb.network=%u\nusb.net_up=%u\n", hostlink_network_mode(),
		(board.netif != NULL) && board.netif->up());
#if CONFIG_DMR_IP_GATEWAY
	hostlink_net_stats_t ns;
	// The three settings fields read in place: no copy of the whole settings on the host task's stack (its deepest
	// path comes through here, stack-check.md) nor a static one (RAM is tight on some radios)
	const settings_t *cfg = settings_begin();
	uint8_t usb_mode = cfg->usb_mode;
	uint8_t net_flags = cfg->net_flags;
	uint32_t link_ip = cfg->net_link_ip;

	settings_end(hal_millis());	// nothing changed: nothing to save
	// Control station mode (docs/spec/usb-network.md 5): the stored settings and what is in force
	dl_text(t, "usb.boot_network=%u\nnet.passthrough=%u\nnet.passthrough_active=%u\nnet.fixed_link=%u\n",
		usb_mode == 1, (net_flags & SETTINGS_NET_PASSTHROUGH) != 0, hostlink_net_passthrough_active(),
		(net_flags & SETTINGS_NET_FIXED_LINK) != 0);
	dl_text(t, "net.link_ip=%08x\nnet.link_in_force=%08x\n", (unsigned)link_ip, (unsigned)ipGatewayFixedLink());

	hostlink_net_stats(&ns);
	dl_text(t, "net.host_ip=%08x\nnet.radio_ip=%08x\n", (unsigned)ipGatewayHostIP(), (unsigned)ipGatewayRadioIP());
	// The board's host netif: without one the gateway's answers have nowhere to go (a link can still negotiate)
	dl_text(t, "net.netif=%u\n", board.netif != NULL);
	dl_text(t, "net.frames_in=%u\nnet.to_air=%u\nnet.to_air_refused=%u\n", (unsigned)ns.frames_in,
		(unsigned)ns.to_air, (unsigned)ns.to_air_refused);
	dl_text(t, "net.packets_air=%u\nnet.packets_up=%u\nnet.packets_dropped=%u\n", (unsigned)ns.packets_air,
		(unsigned)ns.packets_up, (unsigned)ns.packets_dropped);
	hostlink_net_air_stats_t as;

	hostlink_net_air_stats(&as);
	// air_busy: dropped after waiting for a busy service or for too long in the queue (view 3; view 2 counted
	// packets refused because the service was busy); air_full: refused because the air queue was full
	dl_text(t, "net.air_sent=%u\nnet.air_busy=%u\nnet.air_dropped=%u\nnet.air_failed=%u\nnet.air_full=%u\n",
		(unsigned)as.sent, (unsigned)as.dropped, (unsigned)(as.dropped + as.too_long),
		(unsigned)as.failed, (unsigned)as.refused_full);
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
#else
	dl_text(t, "hs.built=0\n");
#endif
}

// The SFR (docs/spec/sfr.md), its own section: in the hotspot section the replies outgrew DL_REPLY_MAX and the last
// keys were cut off. The runner of whichever mode runs: a channel's repeater role, else the MMDVM hotspot's SFR mode
static void sfr_section(dl_text_t *t)
{
#if CONFIG_SFR
	radio_sfr_view_t c;
	bool on, keyed, busy;
	uint8_t slot;
	const sfrRunnerStats_t *st;
	const char *mode;

	radio_sfr_get_view(&c);
	on = c.on;
	keyed = c.keyed;
	busy = c.inbound_busy;
	slot = c.inbound_slot;
	st = &c.stats;
	mode = c.on ? "channel" : "off";
#if CONFIG_HOTSPOT
	hotspot_sfr_view_t f;

	if (!c.on) {
		hotspot_sfr_get_view(&f);
		on = f.on;
		keyed = f.keyed;
		busy = f.inbound_busy;
		slot = f.inbound_slot;
		st = &f.stats;
		mode = f.on ? "hotspot" : "off";
	}
#endif
	dl_text(t, "sfr.on=%u\nsfr.mode=%s\nsfr.keyed=%u\nsfr.busy=%u\nsfr.slots=%u>%u\n", on, mode, keyed, busy,
		slot + 1u, 2u - slot);
	dl_text(t, "sfr.rx=%u\nsfr.repeated=%u\nsfr.ignored=%u\nsfr.echo=%u\nsfr.bad_crc=%u\n",
		(unsigned)st->rx_bursts, (unsigned)st->rx_repeated, (unsigned)st->rx_ignored,
		(unsigned)st->rx_echo, (unsigned)st->rx_bad_crc);
	dl_text(t, "sfr.tx_bursts=%u\nsfr.tx_idle=%u\nsfr.tx_starts=%u\nsfr.up=%u\nsfr.up_lost=%u\n",
		(unsigned)st->tx_bursts, (unsigned)st->tx_idle, (unsigned)st->tx_starts,
		(unsigned)st->up_bursts, (unsigned)st->up_lost);
	dl_text(t, "sfr.tx_beacons=%u\nsfr.tx_gaps=%u\nsfr.evq_drops=%u\nsfr.split=%u\n", (unsigned)st->tx_beacons,
		(unsigned)st->tx_gaps, (unsigned)c.evq_drops, st->tx_split);
	// The second transmitter (hal_dmr_modem_t.tx_split_stats): ready, the last transmission's place, its counts
	dmr_split_stats_t sp;
	memset(&sp, 0, sizeof(sp));
	if (board.dmr != NULL && board.dmr->tx_split_stats != NULL) {
		board.dmr->tx_split_stats(&sp);
	}
	dl_text(t, "sfr.split_ready=%u\nsfr.split_remote=%u\nsfr.split_events=%u\nsfr.split_bursts=%u\n", sp.ready,
		sp.remote, (unsigned)sp.events, (unsigned)sp.bursts);
	dl_text(t, "sfr.split_ends_forced=%u\nsfr.split_fallbacks=%u\n", (unsigned)sp.ends_forced,
		(unsigned)sp.fallbacks);
	// Why bursts were ignored: on the outbound slot, another color code; the last one's CC and type (sfrRunner.h)
	dl_text(t, "sfr.ignored_slot=%u\nsfr.ignored_cc=%u\nsfr.last_ignored=%02x\n", (unsigned)st->rx_ignored_slot,
		(unsigned)st->rx_ignored_cc, st->last_ignored);
	// The repeated voice's AMBE FEC as heard (frames, Golay bits corrected, frames lost)
	dl_text(t, "sfr.rx_fec=%u/%u/%u\n", (unsigned)st->rx_fec_frames, (unsigned)st->rx_fec_bits,
		(unsigned)st->rx_fec_lost);
	dl_text(t, "sfr.rx_fec_pos=%u/%u/%u\n", (unsigned)st->rx_fec_pos[0], (unsigned)st->rx_fec_pos[1],
		(unsigned)st->rx_fec_pos[2]);
#else
	dl_text(t, "sfr.built=0\n");
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
	dl_text(t, "input.idle_ms=%u\nui.backlight=%u\n", (unsigned)input_idle_ms(), ui_backlight());
	if (board.display != NULL && board.display->panel != NULL) {
		hal_display_panel_t pi;
		board.display->panel(&pi);
		if (pi.factory < 0) {
			dl_text(t, "display.panel_raw=none\n");
		} else {
			dl_text(t, "display.panel_raw=0x%02x\n", (unsigned)pi.factory);
		}
		dl_text(t, "display.panel_type=%u\n", pi.type);
		if (pi.controller != NULL) {
			dl_text(t, "display.panel_controller=%s\n", pi.controller);
		}
		if (pi.source != NULL) {
			dl_text(t, "display.panel_source=%s\n", pi.source);
		}
		if (pi.id_read) {
			dl_text(t, "display.panel_id=%02x%02x%02x\ndisplay.panel_id_reg=%02x%02x%02x\n", pi.id[0], pi.id[1],
				pi.id[2], pi.id_reg[0], pi.id_reg[1], pi.id_reg[2]);
		}
	}
	dl_text(t, "input.source=%s\n", input_last_source() == INPUT_SRC_HOST ? "host" : "keys");
	dl_text(t, "input.raw_keys=0x%08x\ninput.injected=0x%08x\n", (unsigned)input_raw_keys(),
		(unsigned)input_injected_keys());
	dl_text(t, "input.encoder_total=%d\n", (int)input_encoder_total());
	hal_encoder_stats_t es;
	uint32_t deferred;
	bool stats = input_encoder_stats(&es, &deferred);
	if (stats) {
		// The decoder's view, before the UI: transitions / detents should be enc_steps; illegal stays near 0
		dl_text(t, "input.enc_detents=%d\ninput.enc_transitions=%u\ninput.enc_illegal=%u\n", (int)es.detents,
			(unsigned)es.transitions, (unsigned)es.illegal);
		dl_text(t, "input.enc_reversals=%u\ninput.enc_steps=%u\n", (unsigned)es.reversals,
			(unsigned)es.steps_per_detent);
		if (es.irq_watch) {
			// Lost encoder interrupts the scan found and served; the state is the first stall's (0 until one)
			dl_text(t, "input.enc_irqs=%u\ninput.enc_stalls=%u\n", (unsigned)es.irqs, (unsigned)es.stalls);
		}
	}
	dl_text(t, "input.enc_deferred=%u\n", (unsigned)deferred);
	dl_text(t, "input.long_ms=%u\ninput.ptt_downs=%u\n", (unsigned)input_long_ms(), (unsigned)input_ptt_downs());
	if (board.system != NULL && board.system->irq_unhandled != NULL) {
		// Interrupts with no handler, each masked for good by the platform's fallback: any count here is a bug
		uint32_t n, src;
		board.system->irq_unhandled(&n, &src);
		dl_text(t, "irq.unhandled=%u\n", (unsigned)n);
		if (n != 0) {
			dl_text(t, "irq.unhandled_src=%u\n", (unsigned)src);
		}
	}
	dl_text(t, "dl.frames=%u\ndl.bad_crc=%u\ndl.too_long=%u\ndl.timeouts=%u\n", (unsigned)c[0], (unsigned)c[1],
		(unsigned)c[2], (unsigned)c[3]);
	if (stats && es.stalls != 0) {
		// Last, as the section is near the reply size: a full reply drops this detail, never the counters above.
		// Three words each, hex: the interrupt controller's two mask words and request record, the GPIO bank's
		// pending, enabled and masked bits
		dl_text(t, "input.enc_stall_ms=%u\ninput.enc_stall_handler=%u\n", (unsigned)es.stall_ms,
			(unsigned)es.stall_handler_ok);
		dl_text(t, "input.enc_stall_pic=%08x,%08x,%08x\n", (unsigned)es.stall_irq_mask[0],
			(unsigned)es.stall_irq_mask[1], (unsigned)es.stall_irq_pending);
		dl_text(t, "input.enc_stall_gpio=%08x,%08x,%08x\n", (unsigned)es.stall_gpio_pending,
			(unsigned)es.stall_gpio_enabled, (unsigned)es.stall_gpio_masked);
	}
}

static void storage_section(dl_text_t *t)
{
	static const char *const loads[] = { "ram", "defaults", "loaded", "upgraded", "foreign" };
	static const char *const regions[] = { "none", "ours", "foreign" };
	static const char *const sources[] = { "none", "builtin", "open-firmware" };
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
	// The fields shown, read in place: no copy of the whole settings on the host task's stack (its deepest path
	// comes through here, stack-check.md), and the lock not held while formatting
	const settings_t *sv = settings_begin();
	unsigned auto_lock_s = sv->auto_lock_s, long_press_ms = sv->long_press_ms, repeat_ms = sv->repeat_ms;
	unsigned mic_gain = sv->mic_gain, theme = settings_theme(sv), ui_flags = sv->ui_flags;
	int ref_trim = sv->ref_trim_ppb;
	unsigned updown_home = settings_updown_home(sv), low_batt_pct = sv->low_batt_pct, auto_off_min = sv->auto_off_min;
	unsigned privacy_type = sv->privacy_type[0], privacy_active = sv->privacy_active;
	// bit 0 noise reduction off, bit 1 AGC on (settings_t.accent bits 6-7)
	unsigned audio_flags = (unsigned)((sv->accent & SETTINGS_AUDIO_MASK) >> SETTINGS_AUDIO_SHIFT);
	// The UI's language: its two code letters, "en" when none is stored (HIL tests read English text)
	char lang[3] = { sv->lang[0], sv->lang[1], 0 };

	settings_end(hal_millis());	// nothing changed: nothing to save
	dl_text(t, "settings.auto_lock_s=%u\nsettings.long_press_ms=%u\nsettings.repeat_ms=%u\nsettings.mic_gain=%u\n",
		auto_lock_s, long_press_ms, repeat_ms, mic_gain);
	dl_text(t, "settings.theme=%u\nsettings.ui_flags=%u\nsettings.updown_home=%u\nsettings.low_batt_pct=%u\n",
		theme, ui_flags, updown_home, low_batt_pct);
	dl_text(t, "settings.auto_off_min=%u\nsettings.privacy_type=%u\nsettings.privacy_active=%u\n", auto_off_min,
		privacy_type, privacy_active);
	// the stored language, or for an empty one (never picked) the build's default
	dl_text(t, "settings.audio_flags=%u\nsettings.lang=%s\n", audio_flags,
		lang[0] ? lang : lang_code(lang_find(0)));
	// The reference trim: stored, and in effect (the SETTING op's, or - on a radio without one)
	dl_text(t, "settings.ref_trim=%d\n", ref_trim);
	if (ref_trim_available()) {
		dl_text(t, "ref_trim=%d\n", (int)radio_ref_trim());
	} else {
		dl_text(t, "ref_trim=-\n");
	}
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
	uint32_t zone_edits;
	uint8_t zone_last;
	zone_edit_stats(&zone_edits, &zone_last);
	dl_text(t, "zone.edits=%u\nzone.edit_last=%u\n", (unsigned)zone_edits, zone_last);
#if CONFIG_CPS_OPENFW
	cps_stats_t c;

	cps_get_stats(&c);
	dl_text(t, "cps.built=1\ncps.requests=%u\ncps.reads=%u\ncps.commits=%u\n", (unsigned)c.requests,
		(unsigned)c.reads, (unsigned)c.commits);
	dl_text(t, "cps.refused=%u\ncps.overflows=%u\ncps.last=0x%02x\n", (unsigned)c.refused, (unsigned)c.overflows,
		c.last);
	dl_text(t, "cps.session=%u\ncps.pending=%u\ncps.junk=%u\n", c.session, c.pending, (unsigned)c.junk);
	dl_text(t, "cps.reboots_ignored=%u\n", (unsigned)c.reboots_ignored);
	// Where the image starts in the flash: what a CPS client adds to its addresses (0 on the STM32 radios)
	const hal_flash_map_t *fm = storage_map();
	dl_text(t, "cps.openfw_eeprom=0x%x\n", fm != NULL ? (unsigned)fm->openfw_eeprom : 0u);
#else
	dl_text(t, "cps.built=0\n");
#endif
#if CONFIG_CPS_FAST
	cps_fast_stats_t f;

	cps_fast_get_stats(&f);
	dl_text(t, "cpsf.state=%u\ncpsf.read=%u\ncpsf.written=%u\ncpsf.commits=%u\ncpsf.refused=%u\n", f.state,
		(unsigned)f.blocks_read, (unsigned)f.blocks_written, (unsigned)f.commits, (unsigned)f.refused);
#endif
}

static const char *const section_names[DL_VIEW_COUNT] = { "core", "channel", "dmr", "dmr_call", "dmr_stats", "hotspot",
						  "link", "features", "dmr_data", "storage", "dmr_ext",
						  "dmr_data_stats", "roam", "aprs", "aprs_set", "dmr_ip", "net", "snmp", "usrp",
						  "sfr", "tasks", "crash", "watchdog", "reset", "air", "net_svc",
						  "net_link" };

const char *debuglink_view_name(uint8_t section)
{
	return (section < DL_VIEW_COUNT) ? section_names[section] : NULL;
}

void debuglink_view_section(uint8_t section, const radio_view_t *v, dl_text_t *t)
{
	if (section >= DL_VIEW_COUNT) {
		return;
	}
	dl_text(t, "section=%s\n", section_names[section]);
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
	case DL_VIEW_APRS:
		debuglink_view_aprs(0, t);
#if CONFIG_APRS
		// The level an APRS RF transmission keys at (its own setting, else the radio's current one; HIL tests check
		// it before keying)
		dl_text(t, "aprs.tx_power=%u\n", aprs_tx_power(v->power));
#endif
		break;
	case DL_VIEW_APRS_SET:
		debuglink_view_aprs(1, t);
		break;
	case DL_VIEW_DMR_IP:
		debuglink_view_dmr_data(2, t);
		break;
	case DL_VIEW_NET:
		debuglink_view_net(t);
		break;
	case DL_VIEW_SNMP:
		debuglink_view_snmp(t);
		break;
	case DL_VIEW_SFR:
		sfr_section(t);
		break;
	case DL_VIEW_TASKS:
		debuglink_view_tasks(t);
		break;
	case DL_VIEW_CRASH:
		debuglink_view_crash(t);
		break;
	case DL_VIEW_WATCHDOG:
		debuglink_view_watchdog(t);
		break;
	case DL_VIEW_RESET:
		debuglink_view_reset(t);
		break;
	case DL_VIEW_AIR:
		debuglink_view_air(t);
		break;
	case DL_VIEW_NET_SVC:
		debuglink_view_net_svc(t);
		break;
	case DL_VIEW_NET_LINK:
		net_link_section(t);
		break;
	case DL_VIEW_USRP:
#if CONFIG_NET_USRP
		usrp_net_view(t);
#else
		dl_text(t, "usrp.built=0\n");
#endif
		break;
	default:
		break;
	}
}
