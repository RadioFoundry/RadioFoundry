// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI host simulator's command line (docs/spec/ui.md 7):
//
//   uiSim [--geom 160x120i5|160x128|128x64|240x320|WxH[iN|m]] [--map K|M|V|F] [--scale N] [--out FILE.png]
//         [--script "ok 4 down ok expect:zone=1"]
//   uiSim --gallery DIR      every screen at every display class: DIR/<geom>/<screen>.png, plus the contact sheets
//                            in DIR/sheets. A color page is drawn again in each highlight color UI_GALLERY_COLORS
//                            names (space separated, sim_color_find: "red yellow light_blue"), as
//                            <screen>__<color>.png right after it, on color displays only (none when it is unset).
//                            The theme pages are drawn again in the light and high-contrast themes too, as
//                            <screen>__light.png and <screen>__high_contrast.png
//   uiSim --sheets DIR [GEOM] contact sheets only: DIR/<geom>.png, every gallery screen at 2x under its name in one
//                            palette PNG, at one display class or all of them (a quick layout audit)
//   uiSim --ellipsis [GEOM]  every text shortened with an ellipsis on the gallery screens and the settings pages
//                            (lists scrolled to their end), at one display class or all of them
//   uiSim --layout [GEOM]    the layout check (sim_layout_issues) on the gallery screens, the settings pages and
//                            the worst-case screens (longest names, IDs, aliases, messages, every status mark),
//                            normal and worst-case content, every display class, every theme on color ones: exit
//                            status 1 on any problem
//
// The script runs from Home after a reset (see tests/host/sim/ui_sim.c for the tokens); the screen at the end is
// written to --out. Exit status 1 if a key isn't on the key map or an expect: fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "config.h"
#include "ui/ui_screens.h"
#include "ui/gfx.h"

// Screens for the gallery: a name and the script that reaches it from Home (zone 0, channel 3, a DMR channel)
static const struct { const char *name, *script; } gallery[] = {
	{ "boot", "splash" },
	{ "stub_entry", "splash:loader" },	// while the flashing tools have the radio (web flasher)
	{ "home_dmr", "" },
	{ "home_dmr_lastheard", "call:3101234,91,g wait:11000" },
	{ "home_reply", "call:3101234,91,g" },
	{ "home_fm", "down" },
	{ "home_temporary", "down quick 1 2 up back" },
	{ "home_vfo", "ok 4 up ok" },
	{ "home_vfo_entry", "ok 4 up ok 4 4 6" },
	{ "home_vfo_dmr", "ok 4 up ok quick 4 back" },
	{ "home_large_text", "ok 7 2 down down down down down ok long:back" },
	{ "home_monitor", "down mondown" },
	{ "home_plain", "strip:off" },	// Settings > Display > Filled status bar off
	{ "home_locked", "long:quick 5" },
	{ "home_usb_ip", "usbnet:on" },
	{ "home_usb_ip_up", "usbnet:up" },
	{ "rx_group", "rx:3101234,91,g,K1ABC_Joe_-_Boston,_MA" },
	{ "rx_private", "rx:3105678,0,p" },
	{ "tx", "pttdown" },
	{ "tx_private", "5 6 3 ok wait:5000 pttdown" },
	{ "tx_dtmf", "down pttdown dtmf:5551234#" },	// DTMF sent on FM (radio-features.md 3.7)
	{ "banner_message", "msg:3101234,Meet_at_the_north_lot_in_15_minutes._Bring_the_spare_battery." },
	{ "banner_call_alert", "alert:3105678" },
	{ "banner_radio_check", "call:3101234,91,g wait:11000 ctx 4 wait:1000" },
	{ "message_view", "msg:3101234,Meet_at_the_north_lot_in_15_minutes._Bring_the_spare_battery. ctx" },
	{ "quick_dmr", "quick" },
	{ "quick_fm", "down quick" },
	{ "quick_fm_squelch", "down quick 2 up" },
	{ "quick_cc_editor", "quick 3 7" },
	{ "quick_vfo_fm", "ok 4 up ok quick" },
	{ "home_copy_to_vfo", "down quick 4" },
	{ "quick_vfo_dmr", "ok 4 up ok quick 4" },
	{ "quick_vfo_cc_editor", "ok 4 up ok quick 4 3 7" },
	{ "find_name", "5 6 3" },
	{ "find_number", "3 1 0 0" },
	{ "find_channel", "1 2" },
	{ "caller_actions", "call:3101234,91,g wait:11000 ctx" },
	{ "compose", "call:3101234,91,g wait:11000 ctx 3" },
	{ "hub", "ok" },
	{ "contacts", "ok 1" },
	{ "contacts_find", "ok 1 5 6" },
	{ "contact_actions", "ok 1 5 6 ctx" },
	{ "messages", "msg:3101234,Meet_at_the_north_lot msg:3105678,On_my_way wait:9000 ok 2" },
	{ "messages_empty", "ok 2" },
	{ "recent", "call:3101234,91,g call:3105678,3100,g call:3102222,0,p ok 3" },
	{ "zones", "ok 4" },
	{ "channels", "ok 5" },
	{ "channel", "ok 5 ok" },
	{ "channel_power", "ok 5 ok down down down down down ok enc+ enc+" },
	{ "channel_changed", "ok 5 ok down down down down down ok enc+ enc+ ok down down down ok" },
	{ "channel_rx_entry", "ok 5 ok down down down ok 4 4 6 0" },
	{ "channel_name", "ok 5 ok ok 4 4 4 wait:1200 6 6" },
	{ "channel_contact", "ok 5 ok down down down down down down down down down ok" },
	{ "channel_discard", "ok 5 ok down down down down down ok enc+ ok back" },
	{ "channel_new", "ok 5 up up up ok" },
#if CONFIG_ZONE_SHARE
	{ "channel_share", "ok 5 ok up up" },	// the channel editor's Share channel to...
#endif
	{ "zone_channels", "ok 4 ctx" },
#if CONFIG_ZONE_EDIT
	{ "zone_share_to", "ok 4 ctx down down ctx 5" },	// Channel actions' Share channel to...
#else
	{ "zone_share", "ok 4 ctx down down ctx" },
	{ "zone_share_to", "ok 4 ctx down down ctx 1" },
#endif
	{ "banner_zone_offer", "zoneoffer:3101234" },
	{ "zone_offer", "zoneoffer:3101234 ctx" },
	{ "zone_offer_preview", "zoneoffer:3101234 ctx 2" },
	{ "zone_offer_ch", "zoneoffer:3101234,c ctx" },
	{ "tools_zone_offer", "zoneoffer:3101234 wait:9000 ok 6 down down down down down down down down down" },	// Tools > Zones/Chs Received
	{ "zone_offer_channel", "zoneoffer:3101234,c ctx 1" },
	{ "zone_imported", "zoneoffer:3101234 ctx 1 wait:300" },
#if CONFIG_ZONE_EDIT
	// The zone editor (ui.md 2.2)
	{ "zone_actions", "ok 4 ctx ctx" },
	{ "zone_add", "ok 4 ctx 2 ok" },
	{ "zone_options", "ok 4 ctx up up up up ok" },
#if CONFIG_ZONE_SHARE
	{ "zone_delete", "ok 4 ctx up up up up ok 4" },	// after Share zone to...
#else
	{ "zone_delete", "ok 4 ctx up up up up ok 3" },
#endif
#endif
	{ "tools", "ok 6" },
	{ "position", "ok 6 2" },
	{ "gps_sky", "ok 6 2 2" },
	{ "gps_signal", "ok 6 2 3" },
	{ "gps_grid", "ok 6 2 4" },
	{ "hotspot_info", "ok 6 3" },
	{ "diagnostics", "ok 6 7" },
	{ "roaming", "ok 6 6 1 wait:4000" },
	{ "settings", "ok 7" },
	{ "settings_display", "ok 7 2" },
	{ "settings_editor", "ok 7 2 ok up" },
	{ "settings_freq_trim", "trim ok 7 0 down down down" },	// on a radio with a reference trim (radio-features.md 6.1)
	{ "freq_trim", "trim ok 7 0 down down down ok ctx 4 5 ok" },
	{ "freq_trim_typing", "trim ok 7 0 down down down ok 1 2" },
	{ "settings_advanced", "ok 7 0" },
	{ "factory_reset", "ok 7 0 down down down ok" },
	{ "about", "ok 7 0 ok" },
	{ "crash_log", "crash:on ok 7 0 down down down down ok" },
	{ "crash_prompt", "crash:ask wait:2500" },
	{ "crash_send_to", "crash:on ok 7 0 down down down down ok 2" },
	{ "talk_target_picker", "call:3101234,91,g tt" },
	{ "talk_target_picker_vfo", "ok 4 up ok quick 4 tt" },
	{ "scan_actions", "ok 6 1 ctx" },
#if CONFIG_DUAL_WATCH
	// Dual watch (docs/ux/dual-watch.md): A = channel 3, B = channel 5 of the zone unless the script picks another
	{ "dual_pick", "quick 7 ctx 3" },
	{ "dual_zones", "quick 7 ctx 3 up up up up ok" },
	{ "home_dual", "quick 7 ctx 3 5 ok wait:3000" },
	{ "home_dual_rx_b", "quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g tune:a call:3107777,3100,g wait:4000 tune:b rx:3101234,91,g wait:2500" },
	{ "home_dual_heard", "quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g tune:a call:3107777,3100,g wait:4000" },
	// The wait after a call on B: a look at A found a signal there
	{ "home_dual_busy", "quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g busy:a" },
	{ "home_dual_on_b", "quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g tune:a call:3107777,3100,g wait:4000 ctx 1 wait:3000" },
	{ "home_dual_tx", "quick 7 ctx 3 5 ok wait:3000 pttdown" },
	{ "home_dual_vfo", "quick 7 ctx 1 vfokey ctx 1 wait:3000" },
	// A name that needs row 1 whole beside the PTT pill: the pill moves to row 3 where there is one
	{ "home_dual_pill_row3", "quick 7 ctx 1 wait:3000" },
	{ "home_dual_other_zone", "quick 7 ctx 1 zone+ ctx 1 wait:3000 tune:b fmrx" },
	{ "home_dual_fm", "quick 7 ctx 3 2 ok wait:3000" },
	{ "home_dual_large_text", "ok 7 2 down down down down down ok long:back quick 7 ctx 3 5 ok wait:3000 tune:b rx:3101234,91,g" },
	// A radio with a receiver per channel: calls on both at once, the second one muted
	{ "home_dual_two_rx", "receivers:2 quick 7 ctx 3 5 ok wait:3000 rxa:3101234,3100,g rxb:3107777,91,g wait:1000" },
	// Talking on B while a call arrives on A
	{ "home_dual_tx_b", "receivers:2 quick 7 ctx 3 5 ok wait:3000 ctx 1 wait:500 rxa:3101234,3100,g wait:1000 pttdown" },
	{ "home_dual_two_rx_large_text", "ok 7 2 down down down down down ok long:back receivers:2 quick 7 ctx 3 5 ok wait:3000 rxa:3101234,3100,g rxb:3107777,91,g wait:1000" },
	{ "home_dual_talk_back", "ok 7 3 down down down down down down ok long:back quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g wait:4000" },
	{ "dual_actions", "quick 7 ctx 3 5 ok wait:3000 tune:b call:3101234,91,g tune:a call:3107777,3100,g wait:4000 ctx" },
#endif
	{ "emergency", "emergency:3101234" },
	{ "volume", "softvol volkey up up" },
#if CONFIG_DMR
	{ "dmr_id", "ok 7 9 ok" },		// Settings > My radio
	{ "dmr_id_entry", "ok 7 9 ok 3 1 0 9" },
#endif
	{ "volume_muted", "softvol volkey down down down down down down down down" },
	{ "hotspot", "hotspot:on" },
};

// More screens for --ellipsis: every settings group and the pages under them, lists scrolled through
#define DOWN12 " down down down down down down down down down down down down"
static const struct { const char *name, *script; } audit_extra[] = {
	{ "hub", "ok" DOWN12 },
	{ "tools", "ok 6" DOWN12 },
	{ "settings", "ok 7" DOWN12 },
	{ "settings_sound", "ok 7 1" DOWN12 },
	{ "settings_display", "ok 7 2" DOWN12 },
	{ "settings_talk", "ok 7 3" DOWN12 },
	{ "settings_privacy", "ok 7 4" DOWN12 },
	{ "settings_location", "ok 7 5" DOWN12 },
	{ "settings_connections", "ok 7 6" DOWN12 },
	{ "settings_power", "ok 7 7" DOWN12 },
	{ "settings_keys", "ok 7 8" DOWN12 },
	{ "settings_my_radio", "ok 7 9" DOWN12 },
	{ "settings_advanced", "ok 7 0" DOWN12 },
	{ "about_scrolled", "ok 7 0 ok" DOWN12 DOWN12 },
	{ "codeplug", "ok 7 0 down ok" },
	{ "battery", "ok 6 9" },
	{ "my_radio", "ok 7 9 ok" },
	{ "caller_actions", "call:3101234,91,g wait:11000 ctx" DOWN12 },
	{ "contact_actions", "ok 1 5 6 ctx" DOWN12 },
	{ "recent", "call:3101234,91,g call:3105678,3100,g call:3102222,0,p ok 3" DOWN12 },
	{ "quick_dmr_all", "quick" DOWN12 },
	{ "quick_fm_all", "down quick" DOWN12 },
	{ "picker", "call:3101234,91,g tt" DOWN12 },
};

// Worst-case screens for --layout (they run with sim_worst on, as the gallery screens do a second time)
#define LONG_MSG "Meet_at_the_north_lot_of_the_county_fairgrounds_at_1500._Bring_the_spare_battery,_the_mag_mount_" \
	"antenna_and_the_printed_ICS-205_for_the_net_control."
static const struct { const char *name, *script; } worst_screens[] = {
	{ "w_rx_alias", "rx:3109999,16776415,g,KD9ABC_Christopher_W,_Boston_MA" },
	{ "w_rx_unknown", "rx:3101999,16776414,g,KD9ABC_Christopher_W,_Boston_MA" },
	{ "w_rx_private", "rx:3108765,0,p" },
	{ "w_reply", "call:3109999,16776415,g" },
	{ "w_lastheard", "call:3109999,16776415,g wait:11000" },
	{ "w_lastheard_unknown", "call:3101999,16776414,g wait:11000" },
	{ "w_status_marks", "msg:3109999," LONG_MSG " msg:3108765,OK wait:9000 quick 5 6 back ok 6 1 power:3 "
		"battery:5 long:quick" },
	{ "w_fm_marks", "down quick 5 7 1 back battery:100" },
	{ "w_tx", "power:3 pttdown" },
	{ "w_tx_private", "3 1 0 9 9 9 9 ok wait:5000 pttdown" },
	{ "w_banner_message", "msg:3109999," LONG_MSG },
	{ "w_banner_unknown", "msg:3101999," LONG_MSG },
	{ "w_message_view", "msg:3109999," LONG_MSG " ctx" },
	{ "w_messages", "msg:3109999," LONG_MSG " msg:3101999,On_my_way wait:9000 ok 2" },
	{ "w_banner_alert", "alert:3109999" },
	{ "w_banner_check", "call:3109999,16776415,g wait:11000 ctx 4 wait:1000" },
	{ "w_caller_actions", "call:3109999,16776415,g wait:11000 ctx" },
	{ "w_compose", "call:3109999,16776415,g wait:11000 ctx 3" },
	{ "w_recent", "call:3109999,16776415,g call:3108765,9100091,g call:3101999,0,p call:3102999,16776414,g ok 3" },
	{ "w_picker", "call:3109999,16776415,g tt" },
	{ "w_contacts", "ok 1" },
	{ "w_contact_actions", "ok 1 ctx" },
	{ "w_find", "2 7" },
	{ "w_find_number", "1 6 7 7 6 4 1 5" },
	{ "w_channels", "ok 4" },
	{ "w_zone", "ok 4 ctx" },
	{ "w_zone_end", "ok 4 ctx up" },
#if CONFIG_ZONE_EDIT
	{ "w_zone_actions", "ok 4 ctx ctx" },
	{ "w_zone_delete", "ok 4 ctx up up up up ok 3" },
#endif
	{ "w_quick", "quick" },
	{ "w_quick_fm", "down quick" },
	{ "w_quick_cc", "quick 3 1 5" },
	{ "w_emergency", "emergency:3109999" },
	{ "w_emergency_unknown", "emergency:3101999" },
	{ "w_vfo", "ok 4 up ok 4 4 6 0 0 6 2 5 ok" },
	{ "w_vfo_entry", "ok 4 up ok 4 4 6 0 0 6 2" },
	{ "w_temporary", "quick 1 2 3 1 5 ok back" },
	{ "w_scan", "ok 6 1" },
	{ "w_lock_toast", "long:quick 5" },
	{ "w_large", "ok 7 2 down down down down down ok long:back" },
	{ "w_large_fm", "ok 7 2 down down down down down ok long:back down" },
	{ "w_off_banner", "offfailed" },
	{ "w_low_battery", "battery:5" },
	{ "w_hub_unread", "msg:3109999,OK msg:3108765,OK wait:9000 ok 6 1 ok" },
	{ "w_roaming", "ok 6 6 1 wait:4000" },
	{ "w_aprs", "ok 6 8" },
	{ "w_aprs_nocall", "aprs:nocall ok 6 8" },
	{ "w_aprs_position", "ok 6 8 9 2 3 3 7 4 9 0 ok 3 8 4 3 8 8 0 down ok wait:3000" },
	{ "w_aprs_position_edit", "ok 6 8 9 2 3 3 7" },
	{ "w_usb", "ok 6 4" },
	{ "w_sfr", "ok 6 5" },
	{ "w_battery", "battery:5 ok 6 9" },
	{ "w_my_radio", "ok 7 9 ok" },
	{ "w_codeplug", "ok 7 0 down ok" },
	{ "w_settings_number", "ok 7 3 ok up" },
	{ "w_freq_trim", "trim ok 7 0 down down down ok ctx 1 0 0 ok" },
	{ "w_settings_privacy", "ok 7 4 ok ok down ok up" },
	{ "w_settings_range", "ok 7 7 down down ok 9 9 ok" },
	{ "w_hotspot", "hotspot:on" },
	{ "w_hotspot_tx", "hotspot:on pttdown" },
};

// The layout check also counts texts shortened in the middle (gfx_text_fit) with the usability-test codeplug: a name
// or label of ordinary length must fit. A preview cut at its end (a message) is fine; so is worst-case content
static void audit_text(const char *s, char *o, size_t len, bool key);
static int layout_cut;
static char layout_cut_text[64];
static void layout_fit_hook(const char *src, const char *out)
{
	size_t n = strlen(out);
	if (n > 0 && (unsigned char)out[n - 1] != 0x80) {
		layout_cut++;
		audit_text(src, layout_cut_text, sizeof layout_cut_text, false);
	}
}

static int run_layout(const char *only)
{
	char err[256], found[SIM_LAYOUT_MAX][160];
	int problems = 0, screens = 0;
	const char *shots = getenv("RFOUNDRY_LAYOUT_SHOTS");
	for (int g = 0; g < sim_geom_count; g++) {
		if (only != NULL && strcmp(only, sim_geoms[g].name) != 0)
			continue;
		// Readability (principles.md 5): list rows and the status strip keep a pixel between them and the next
		// line on displays 100 px and higher, never squeezed to the font's bare cell to fit one more row
		sim_reset(&sim_geoms[g], SIM_MAP_K);
		const ui_layout_t *L = &ui_layout;
		int pitch = L->small->h + (gfx_width() >= 220 ? 6 : gfx_height() >= UI_TALL_PX ? 3 : gfx_height() >= 100 ? 1 : 0);
		if (L->row_h < pitch || (gfx_height() >= 100 && L->strip_h < L->small->h + 1)) {
			printf("%-10s rows squeezed: row pitch %d, strip %d (small font %d px high)\n", sim_geoms[g].name,
				L->row_h, L->strip_h, L->small->h);
			problems++;
		}
		// Every theme on color displays (Settings > Display): a text in its background's color shows up only in one
		for (int th = 0; th < (sim_geoms[g].bpp == 1 ? 1 : SIM_THEME_COUNT); th++)
		for (int pass = 0; pass < 4; pass++) {
			size_t n = pass < 2 ? sizeof gallery / sizeof gallery[0] : pass == 2 ?
				sizeof audit_extra / sizeof audit_extra[0] : sizeof worst_screens / sizeof worst_screens[0];
			for (size_t i = 0; i < n; i++) {
				char name[96], script[1024];
				snprintf(name, sizeof name, "%s%s%s", th ? sim_themes[th] : "", th ? " " : "",
					 pass < 2 ? gallery[i].name : pass == 2 ? audit_extra[i].name : worst_screens[i].name);
				snprintf(script, sizeof script, "%s%s%s", th ? "theme:" : "", th ? sim_themes[th] : "",
					 th ? " " : "");
				size_t sl = strlen(script);
				snprintf(script + sl, sizeof script - sl, "%s", pass < 2 ? gallery[i].script : pass == 2 ?
					 audit_extra[i].script : worst_screens[i].script);
				sim_reset(&sim_geoms[g], SIM_MAP_K);
				if (pass == 1 || pass == 3)
					sim_worst(true);
				layout_cut = 0;
				sim_set_fit_hook(th == 0 && (pass == 0 || pass == 2) ? layout_fit_hook : NULL);
				bool ran = sim_run(script, err, sizeof err);
				sim_set_fit_hook(NULL);
				if (ran && layout_cut) {
					printf("%-10s %-20s shortened in the middle: \"%s\"\n", sim_geoms[g].name, name,
					       layout_cut_text);
					problems++;
				}
				if (!ran) {
					printf("FAIL %s %s: %s\n", sim_geoms[g].name, name, err);
					problems++;
					continue;
				}
				screens++;
				if (shots != NULL) {	// RFOUNDRY_LAYOUT_SHOTS=DIR: every screen checked, as DIR/<geom>/<name>.png
					char path[512];
					snprintf(path, sizeof path, "%s/%s", shots, sim_geoms[g].name);
					mkdir(shots, 0755);
					mkdir(path, 0755);
					snprintf(path, sizeof path, "%s/%s/%s%s%s%s%s.png", shots, sim_geoms[g].name,
						 th ? sim_themes[th] : "", th ? "_" : "", pass == 1 ? "worst_" : "", pass == 2 ? "x_" : "",
						 pass < 2 ? gallery[i].name : pass == 2 ? audit_extra[i].name : worst_screens[i].name);
					sim_save_png(path, 1);
				}
				int k = sim_layout_issues(found, SIM_LAYOUT_MAX);
				for (int j = 0; j < k; j++)
					printf("%-10s %s%-20s %s\n", sim_geoms[g].name, pass == 1 ? "worst " : "", name, found[j]);
				problems += k;
			}
		}
	}
	printf("# layout: %d screens checked, %d problems\n", screens, problems);
	return problems != 0;
}

static const char *audit_geom, *audit_screen;
static char audit_seen[512][96];
static int audit_seen_n, audit_total;
// Texts shortened while the script ran, with the screen they were on; only the final screen's are listed
static struct { char scr[24], key[64], src[64], out[64]; } audit_run[256];
static int audit_run_n;

// Printable copy: the ellipsis and the dot glyphs as UTF-8; digits as # when key (so ages count once)
static void audit_text(const char *s, char *o, size_t len, bool key)
{
	size_t n = 0;
	for (; *s && n + 4 < len; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == 0x80 || c == 0x81) {	// GLYPH_ELLIPSIS, GLYPH_DOT
			memcpy(o + n, c == 0x80 ? "\xe2\x80\xa6" : "\xc2\xb7", c == 0x80 ? 3 : 2);
			n += c == 0x80 ? 3 : 2;
		} else {
			o[n++] = key && c >= '0' && c <= '9' ? '#' : (char)c;
		}
	}
	o[n] = 0;
}

static void audit_where(char *scr, size_t len)
{
	ui_app_status_t st;
	ui_app_status(&st);
	snprintf(scr, len, "%s|%u", st.screen, (unsigned)st.depth);
}

static void audit_hook(const char *src, const char *out)
{
	if (audit_run_n >= 256)
		return;
	audit_where(audit_run[audit_run_n].scr, sizeof audit_run[0].scr);
	audit_text(src, audit_run[audit_run_n].key, sizeof audit_run[0].key, true);
	audit_text(src, audit_run[audit_run_n].src, sizeof audit_run[0].src, false);
	audit_text(out, audit_run[audit_run_n].out, sizeof audit_run[0].out, false);
	audit_run_n++;
}

static void audit_flush(void)
{
	char scr[24], line[96];
	audit_where(scr, sizeof scr);
	for (int r = 0; r < audit_run_n; r++) {
		if (strcmp(audit_run[r].scr, scr) != 0)
			continue;
		snprintf(line, sizeof line, "%s|%s|%s", audit_geom, audit_screen, audit_run[r].key);
		bool seen = false;
		for (int i = 0; i < audit_seen_n && !seen; i++)
			seen = strcmp(audit_seen[i], line) == 0;
		if (seen)
			continue;
		if (audit_seen_n < 512)
			strcpy(audit_seen[audit_seen_n++], line);
		audit_total++;
		printf("%-10s %-22s \"%s\" -> \"%s\"\n", audit_geom, audit_screen, audit_run[r].src, audit_run[r].out);
	}
	audit_run_n = 0;
}

static int run_audit(const char *only)
{
	char err[256];
	sim_set_fit_hook(audit_hook);
	for (int g = 0; g < sim_geom_count; g++) {
		if (only != NULL && strcmp(only, sim_geoms[g].name) != 0)
			continue;
		audit_geom = sim_geoms[g].name;
		for (int pass = 0; pass < 2; pass++) {
			size_t n = pass == 0 ? sizeof gallery / sizeof gallery[0] : sizeof audit_extra / sizeof audit_extra[0];
			for (size_t i = 0; i < n; i++) {
				audit_screen = pass == 0 ? gallery[i].name : audit_extra[i].name;
				sim_reset(&sim_geoms[g], SIM_MAP_K);
				audit_run_n = 0;
				if (!sim_run(pass == 0 ? gallery[i].script : audit_extra[i].script, err, sizeof err))
					printf("FAIL %s %s: %s\n", audit_geom, audit_screen, err);
				audit_flush();
			}
		}
	}
	sim_set_fit_hook(NULL);
	printf("# %d texts shortened with an ellipsis\n", audit_total);
	return 0;
}

// Gallery screens drawn differently on color displays. G_COLOR: color displays only (on a monochrome one it would
// repeat another screen); G_COLORS: also once in each UI_GALLERY_COLORS highlight color, on color displays;
// G_THEMES: also in the light and high-contrast themes (Settings > Display), on color displays
#define G_COLOR		1u
#define G_COLORS	2u
#define G_THEMES	4u
static const struct { const char *name; unsigned flags; } gallery_color[] = {
	{ "home_dmr", G_COLORS | G_THEMES },	// the filled status bar (the default) in the color
	{ "home_plain", G_COLOR | G_THEMES },	// the plain strip's state pills
	{ "rx_group", G_THEMES },
	{ "tx", G_THEMES },
#if CONFIG_DUAL_WATCH
	{ "home_dual_rx_b", G_THEMES },
	{ "home_dual_two_rx", G_THEMES },
#endif
	{ "channels", G_COLORS | G_THEMES },	// the focus row in the color
	{ "settings_display", G_THEMES },
};

static unsigned gallery_flags(size_t i)
{
	for (size_t k = 0; k < sizeof gallery_color / sizeof gallery_color[0]; k++)
		if (strcmp(gallery_color[k].name, gallery[i].name) == 0)
			return gallery_color[k].flags;
	return 0;
}

// The gallery's highlight colors (UI_GALLERY_COLORS), in its order
static const char *gal_colors[8];
static int gal_ncolors;

static bool gallery_colors(void)
{
	static char buf[128];
	const char *env = getenv("UI_GALLERY_COLORS");
	snprintf(buf, sizeof buf, "%s", env != NULL ? env : "");
	gal_ncolors = 0;
	for (char *t = strtok(buf, " ,"); t != NULL; t = strtok(NULL, " ,")) {
		if (sim_color_find(t) < 0 || gal_ncolors == (int)(sizeof gal_colors / sizeof gal_colors[0])) {
			fprintf(stderr, "UI_GALLERY_COLORS: %s isn't a highlight color, or too many\n", t);
			return false;
		}
		gal_colors[gal_ncolors++] = t;
	}
	return true;
}

// Shots of a gallery screen: the screen itself, its highlight colors, then its themes after the dark one
#define GAL_SHOTS	(1 + gal_ncolors + SIM_THEME_COUNT - 1)

// Shot c of gallery screen i at display class g: c 0 the screen itself, 1.. its colors, then its themes. False: not
// at this class
static bool gallery_shot(size_t i, int c, const sim_geom_t *g, char *name, size_t nlen, char *script, size_t slen)
{
	unsigned flags = gallery_flags(i);
	bool theme = c > gal_ncolors;
	if (g->bpp == 1 && (c > 0 || (flags & G_COLOR)))
		return false;
	if (c > 0 && !(flags & (theme ? G_THEMES : G_COLORS)))
		return false;
	if (c == 0) {
		snprintf(name, nlen, "%s", gallery[i].name);
		snprintf(script, slen, "%s", gallery[i].script);
	} else {
		const char *v = theme ? sim_themes[c - gal_ncolors] : gal_colors[c - 1];
		snprintf(name, nlen, "%s__%s", gallery[i].name, v);
		snprintf(script, slen, "%s:%s %s", theme ? "theme" : "color", v, gallery[i].script);
	}
	return true;
}

// One display class's screens: DIR/<geom>/<screen>.png when shots, and the contact sheet (2x, every screen under its
// name) as SHEET_DIR/<geom>.png
static int render_geom(const sim_geom_t *g, const char *dir, bool shots, const char *sheet_dir)
{
	char path[512], err[256], name[64], script[512];
	int fails = 0, count = 0;
	size_t n = sizeof gallery / sizeof gallery[0];
	if (shots) {
		snprintf(path, sizeof path, "%s/%s", dir, g->name);
		mkdir(path, 0755);
	}
	for (size_t i = 0; i < n; i++)
		for (int c = 0; c < GAL_SHOTS; c++)
			count += gallery_shot(i, c, g, name, sizeof name, script, sizeof script);
	sim_sheet_begin(count, 2);
	for (size_t i = 0; i < n; i++) {
		for (int c = 0; c < GAL_SHOTS; c++) {
			if (!gallery_shot(i, c, g, name, sizeof name, script, sizeof script))
				continue;
			sim_reset(g, SIM_MAP_K);
			if (!sim_run(script, err, sizeof err)) {
				printf("FAIL gallery %s %s: %s\n", g->name, name, err);
				fails++;
				continue;
			}
			snprintf(path, sizeof path, "%s/%s/%s.png", dir, g->name, name);
			if (shots && !sim_save_png(path, 1)) {
				printf("FAIL can't write %s\n", path);
				fails++;
			}
			if (!sim_sheet_add(name)) {
				printf("FAIL sheet %s %s\n", g->name, name);
				fails++;
			}
		}
	}
	snprintf(path, sizeof path, "%s/%s.png", sheet_dir, g->name);
	if (!sim_sheet_save(path)) {
		printf("FAIL can't write %s\n", path);
		fails++;
	}
	return fails;
}

static int run_gallery(const char *dir)
{
	char sheets[512];
	int fails = 0;
	if (!gallery_colors())
		return 2;
	mkdir(dir, 0755);
	snprintf(sheets, sizeof sheets, "%s/sheets", dir);
	mkdir(sheets, 0755);
	for (int g = 0; g < sim_geom_count; g++)
		fails += render_geom(&sim_geoms[g], dir, true, sheets);
	printf("# gallery: %zu screens x %d display classes (color pages also in %d colors and %d themes) in %s, contact "
	       "sheets in %s\n", sizeof gallery / sizeof gallery[0], sim_geom_count, gal_ncolors, SIM_THEME_COUNT - 1, dir,
	       sheets);
	return fails != 0;
}

// Contact sheets only, at one display class (any geometry --geom takes) or every class
static int run_sheets(const char *dir, const char *geom)
{
	int fails = 0;
	if (!gallery_colors())
		return 2;
	mkdir(dir, 0755);
	if (geom != NULL) {
		const sim_geom_t *g = sim_geom_find(geom);
		if (g == NULL) {
			fprintf(stderr, "unknown geometry %s\n", geom);
			return 2;
		}
		fails += render_geom(g, dir, false, dir);
	} else {
		for (int g = 0; g < sim_geom_count; g++)
			fails += render_geom(&sim_geoms[g], dir, false, dir);
	}
	printf("# contact sheets in %s\n", dir);
	return fails != 0;
}

int main(int argc, char **argv)
{
	const sim_geom_t *g = &sim_geoms[0];
	sim_map_t map = SIM_MAP_K;
	const char *script = "", *out = NULL;
	int scale = 2;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(a, "--gallery") == 0 && v != NULL)
			return run_gallery(v);
		if (strcmp(a, "--sheets") == 0 && v != NULL)
			return run_sheets(v, i + 2 < argc ? argv[i + 2] : NULL);
		if (strcmp(a, "--ellipsis") == 0)
			return run_audit(v);
		if (strcmp(a, "--layout") == 0)
			return run_layout(v);
		if (strcmp(a, "--geom") == 0 && v != NULL) {
			g = sim_geom_find(v);
			if (g == NULL) {
				fprintf(stderr, "unknown geometry %s\n", v);
				return 2;
			}
			i++;
		} else if (strcmp(a, "--map") == 0 && v != NULL) {
			const char *maps = "KMVF";
			const char *p = strchr(maps, v[0]);
			if (p == NULL || v[0] == 0) {
				fprintf(stderr, "key map is one of K M V F\n");
				return 2;
			}
			map = (sim_map_t)(p - maps);
			i++;
		} else if (strcmp(a, "--script") == 0 && v != NULL) {
			script = v;
			i++;
		} else if (strcmp(a, "--out") == 0 && v != NULL) {
			out = v;
			i++;
		} else if (strcmp(a, "--scale") == 0 && v != NULL) {
			scale = atoi(v);
			i++;
		} else {
			fprintf(stderr, "usage: uiSim [--geom G] [--map K|M|V|F] [--script S] [--out F.png] [--scale N] | "
				"--gallery DIR | --sheets DIR [GEOM]\n");
			return 2;
		}
	}
	char err[256];
	sim_reset(g, map);
	bool ok = sim_run(script, err, sizeof err);
	if (!ok)
		printf("FAIL %s\n", err);
	printf("presses %d\n", sim_presses());
	if (out != NULL && !sim_save_png(out, scale)) {
		fprintf(stderr, "can't write %s\n", out);
		return 2;
	}
	return ok ? 0 : 1;
}
