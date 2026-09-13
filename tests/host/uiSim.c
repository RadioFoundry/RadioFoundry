// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI host simulator's command line (docs/spec/ui.md 7):
//
//   uiSim [--geom 160x120i5|160x128|128x64|240x320|WxH[iN|m]] [--map K|M|V|F] [--scale N] [--out FILE.png]
//         [--script "ok 4 down ok expect:zone=1"]
//   uiSim --gallery DIR      every screen at every display class: DIR/<geom>/<screen>.png
//   uiSim --ellipsis [GEOM]  every text shortened with an ellipsis on the gallery screens and the settings pages
//                            (lists scrolled to their end), at one display class or all of them
//
// The script runs from Home after a reset (see tests/host/sim/ui_sim.c for the tokens); the screen at the end is
// written to --out. Exit status 1 if a key isn't on the key map or an expect: fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"

// Screens for the gallery: a name and the script that reaches it from Home (zone 0, channel 3, a DMR channel)
static const struct { const char *name, *script; } gallery[] = {
	{ "boot", "splash" },
	{ "home_dmr", "" },
	{ "home_dmr_lastheard", "call:3101234,91,g wait:11000" },
	{ "home_reply", "call:3101234,91,g" },
	{ "home_fm", "down" },
	{ "home_temporary", "down quick 1 2 up back" },
	{ "home_vfo", "ok 4 up ok" },
	{ "home_vfo_entry", "ok 4 up ok 4 4 6" },
	{ "home_large_text", "ok 6 2 down down down ok long:back" },
	{ "home_locked", "long:quick 5" },
	{ "rx_group", "rx:3101234,91,g,Joe_-_Boston,_MA" },
	{ "rx_private", "rx:3105678,0,p" },
	{ "tx", "pttdown" },
	{ "banner_message", "msg:3101234,Meet_at_the_north_lot_in_15_minutes._Bring_the_spare_battery." },
	{ "banner_call_alert", "alert:3105678" },
	{ "banner_radio_check", "call:3101234,91,g wait:11000 ctx 4 wait:1000" },
	{ "message_view", "msg:3101234,Meet_at_the_north_lot_in_15_minutes._Bring_the_spare_battery. ctx" },
	{ "quick_dmr", "quick" },
	{ "quick_fm", "down quick" },
	{ "quick_fm_squelch", "down quick 2 up" },
	{ "quick_cc_editor", "quick 3 7" },
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
	{ "recent", "call:3101234,91,g call:3105678,3100,g call:3102222,0,p long:ctx" },
	{ "channels", "ok 4" },
	{ "zone_channels", "ok 4 ctx" },
	{ "tools", "ok 5" },
	{ "position", "ok 5 2" },
	{ "hotspot_info", "ok 5 3" },
	{ "diagnostics", "ok 5 7" },
	{ "roaming", "ok 5 8 1 wait:4000" },
	{ "settings", "ok 6" },
	{ "settings_display", "ok 6 2" },
	{ "settings_editor", "ok 6 2 ok up" },
	{ "settings_advanced", "ok 6 0" },
	{ "factory_reset", "ok 6 0 down down down ok" },
	{ "about", "ok 6 0 ok" },
	{ "talk_target_picker", "call:3101234,91,g tt" },
	{ "scan_actions", "quick 7 back ctx" },
	{ "emergency", "emergency:3101234" },
	{ "hotspot", "hotspot:on" },
};

// More screens for --ellipsis: every settings group and the pages under them, lists scrolled through
#define DOWN12 " down down down down down down down down down down down down"
static const struct { const char *name, *script; } audit_extra[] = {
	{ "hub", "ok" DOWN12 },
	{ "tools", "ok 5" DOWN12 },
	{ "settings", "ok 6" DOWN12 },
	{ "settings_sound", "ok 6 1" DOWN12 },
	{ "settings_display", "ok 6 2" DOWN12 },
	{ "settings_talk", "ok 6 3" DOWN12 },
	{ "settings_privacy", "ok 6 4" DOWN12 },
	{ "settings_location", "ok 6 5" DOWN12 },
	{ "settings_connections", "ok 6 6" DOWN12 },
	{ "settings_power", "ok 6 7" DOWN12 },
	{ "settings_keys", "ok 6 8" DOWN12 },
	{ "settings_my_radio", "ok 6 9" DOWN12 },
	{ "settings_advanced", "ok 6 0" DOWN12 },
	{ "about_scrolled", "ok 6 0 ok" DOWN12 DOWN12 },
	{ "codeplug", "ok 6 0 down ok" },
	{ "battery", "ok 5 4" },
	{ "my_radio", "ok 6 9 ok" },
	{ "caller_actions", "call:3101234,91,g wait:11000 ctx" DOWN12 },
	{ "contact_actions", "ok 1 5 6 ctx" DOWN12 },
	{ "recent", "call:3101234,91,g call:3105678,3100,g call:3102222,0,p long:ctx" DOWN12 },
	{ "quick_dmr_all", "quick" DOWN12 },
	{ "quick_fm_all", "down quick" DOWN12 },
	{ "picker", "call:3101234,91,g tt" DOWN12 },
};

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

static int run_gallery(const char *dir)
{
	char path[512], err[256];
	int fails = 0;
	mkdir(dir, 0755);
	for (int g = 0; g < sim_geom_count; g++) {
		snprintf(path, sizeof path, "%s/%s", dir, sim_geoms[g].name);
		mkdir(path, 0755);
		for (size_t i = 0; i < sizeof gallery / sizeof gallery[0]; i++) {
			sim_reset(&sim_geoms[g], SIM_MAP_K);
			if (!sim_run(gallery[i].script, err, sizeof err)) {
				printf("FAIL gallery %s %s: %s\n", sim_geoms[g].name, gallery[i].name, err);
				fails++;
				continue;
			}
			snprintf(path, sizeof path, "%s/%s/%s.png", dir, sim_geoms[g].name, gallery[i].name);
			if (!sim_save_png(path, 1)) {
				printf("FAIL can't write %s\n", path);
				fails++;
			}
		}
	}
	printf("# gallery: %zu screens x %d display classes in %s\n", sizeof gallery / sizeof gallery[0],
	       sim_geom_count, dir);
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
		if (strcmp(a, "--ellipsis") == 0)
			return run_audit(v);
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
				"--gallery DIR\n");
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
