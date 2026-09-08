// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI host simulator's command line (docs/spec/ui.md 7):
//
//   uiSim [--geom 160x120i5|160x128|128x64|240x320|WxH[iN|m]] [--map K|M|V|F] [--scale N] [--out FILE.png]
//         [--script "ok 4 down ok expect:zone=1"]
//   uiSim --gallery DIR      every screen at every display class: DIR/<geom>/<screen>.png
//
// The script runs from Home after a reset (see tests/host/sim/ui_sim.c for the tokens); the screen at the end is
// written to --out. Exit status 1 if a key isn't on the key map or an expect: fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sim/ui_sim.h"

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
	{ "talk_target_picker", "call:3101234,91,g tt" },
	{ "scan_actions", "quick 7 back ctx" },
	{ "emergency", "emergency:3101234" },
	{ "hotspot", "hotspot:on" },
};

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
