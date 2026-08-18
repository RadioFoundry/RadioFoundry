// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The automated UI checks of docs/ux/usability-tests.md 2, in the host simulator:
// - press budgets: each task flow of docs/ux/task-flows.md as a script on the reference key maps, at most its budget
//   (impossible on a key map that should have it = failure);
// - operate only: flows that only operate the radio never write the codeplug (P7);
// - crawler: every screen reachable from Home is left by BACK, long BACK reaches Home, field-use screens are at most
//   3 levels deep, the status strip (or the title row that carries the state pill) is drawn, PTT shows TX (P2, P4);
// - long presses keep their one meaning on every screen: long BACK Home, long QUICK lock, long CONTEXT Recent (P3).
#include <stdio.h>
#include <string.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"

static int fails, flows_run;

typedef struct {
	const char *name;
	const char *setup;	// not counted
	const char *script;	// counted
	const char *expect;	// checks after the script (expect: tokens)
	int budget;
	bool writes;		// the flow saves on purpose (else: zero codeplug writes)
} flow_t;

#define CALLER	"call:3101234,91,g wait:11000 "

static const flow_t flows[] = {
	{ "1 next channel", "", "up", "pos=4", 1, false },
	{ "1 channel 12 by number", "", "1 2 ok", "pos=12 screen=home", 3, false },
	{ "2 zone 2 positions away", "", "ok 4 down down ok", "zone=2 screen=home", 5, false },
	{ "3 one-shot TG, 2nd in picker", "", "tt down ptt", "txid=91 txpriv=0", 3, false },
	{ "3 one-shot typed TG 3100", "", "3 1 0 0 ptt", "txid=3100 txpriv=0", 5, false },
	{ "3 permanent TG", "", "tt down long:ok", "talk=91 writes=1", 4, true },
	{ "4 reply in the window", "call:3101234,91,g", "ptt", "txid=91", 1, false },
	{ "4 call back after the window", CALLER, "ctx ptt", "txid=91", 2, false },
	{ "4 3rd station in Recent", "call:3101234,91,g call:3105678,3100,g call:3102222,93,g wait:11000",
	  "long:ctx down down ptt", "txid=91", 4, false },
	{ "5 private call by name", "", "5 6 3 ptt", "txid=3101234 txpriv=1", 4, false },
	{ "5 private call by ID", "", "3 1 0 9 9 9 9 down ptt", "txid=3109999 txpriv=1", 9, false },
	{ "6 canned reply to last caller", CALLER, "ctx 3 ok", "sent=1 text=OK", 4, false },
	{ "6 canned to contact by name", "", "5 6 3 ctx 3 ok", "sent=1 text=OK", 6, false },
	{ "7 read message, banner up", "msg:3101234,Meet_at_the_north_lot", "ctx", "screen=message", 1, false },
	{ "7 read message later", "msg:3101234,Meet_at_the_north_lot wait:9000", "ok 2 ok", "screen=message", 3, false },
	{ "8 radio check last caller", CALLER, "ctx 4", "checks=1", 2, false },
	{ "8 radio check by name", "", "5 6 3 ctx 4", "checks=1", 5, false },
	{ "9 power one step", "", "quick 1 back", "power=8 screen=home", 3, false },
	{ "9 squelch 2 steps (FM)", "down", "quick 2 up up back", "sq=7 screen=home", 5, false },
	{ "10 privacy", "", "quick 6 back", "privacy=1 screen=home", 3, false },
	{ "11 scan start", "", "quick 7", "scanning=1", 2, false },
	{ "11 nuisance skip", "quick 7 back", "ctx 1", "pos=4 scanning=1", 2, false },
	{ "11 scan stop", "quick 7 back", "back", "scanning=0", 1, false },
	// task-flows.md 12 says 3: the VFO row is first but Channels opens on the current zone (flow 2), so one Up
	{ "12 into the VFO", "", "ok 4 up ok", "vfo=1 screen=home", 4, false },
	{ "12 446.000 in the VFO", "ok 4 up ok", "4 4 6 0 0 0 ok", "freq=446000000", 7, false },
	{ "13 lock", "", "long:quick", "locked=1", 1, false },
	{ "13 unlock", "long:quick", "long:quick", "locked=0", 1, false },
	{ "14 GPS details", "", "ok 5 2", "screen=info", 3, false },
	{ "15 hotspot page", "", "ok 5 3", "screen=info", 4, false },
	{ "15 stop hotspot", "hotspot:on", "ok", "hotspot=0", 2, false },
	{ "18 FM / DMR", "", "quick 4", "mode=fm", 2, false },
	{ "19 hear all latched", "", "quick 5", "hearall=1", 2, false },
	{ "20 timeslot", "", "quick 2 back", "ts=2 screen=home", 3, false },
	{ "20 color code 7", "", "quick 3 7 ok", "cc=7", 4, false },
	{ "20 save to channel", "quick 2", "long:ok", "writes=1", 1, true },
	{ "T22 Home and undo", "quick 2 back ok 6", "long:back back", "screen=home temporary=0", 2, false },
};

// Few-key radios (7.5): fallbacks
static const flow_t flows_f[] = {
	{ "F next channel", "", "up", "pos=4", 1, false },
	{ "F quick settings (long OK)", "", "long:ok", "screen=quick", 1, false },
	{ "F last caller (Hub item 0)", CALLER, "ok ok ptt", "txid=91", 3, false },
	{ "F zone", "", "ok down down down down ok down ok", "zone=1", 8, false },
};

static void run_flow(const flow_t *f, const sim_geom_t *g, sim_map_t map, const char *mapname)
{
	char err[300];
	char exp[300];
	sim_reset(g, map);
	flows_run++;
	if (!sim_run(f->setup, err, sizeof err)) {
		printf("FAIL %s [%s] setup: %s\n", f->name, mapname, err);
		fails++;
		return;
	}
	uint32_t writes0 = sim_rec.codeplug_writes;
	sim_count_reset();
	if (!sim_run(f->script, err, sizeof err)) {
		printf("FAIL %s [%s]: %s\n", f->name, mapname, err);
		fails++;
		return;
	}
	int n = sim_presses();
	// expect tokens: "a=b c=d" -> "expect:a=b expect:c=d"
	char *o = exp;
	const char *p = f->expect;
	while (*p && o < exp + sizeof exp - 16) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*p && *p != ' ' && o < exp + sizeof exp - 2)
			*o++ = *p++;
		*o++ = ' ';
	}
	*o = 0;
	if (!sim_run(exp, err, sizeof err)) {
		printf("FAIL %s [%s]: %s\n", f->name, mapname, err);
		fails++;
	}
	if (n > f->budget) {
		printf("FAIL %s [%s]: %d presses, budget %d\n", f->name, mapname, n, f->budget);
		fails++;
	}
	if (!f->writes && sim_rec.codeplug_writes != writes0) {
		printf("FAIL %s [%s]: wrote the codeplug while only operating (P7)\n", f->name, mapname);
		fails++;
	}
}

// ---- crawler ----

#define CRAWL_MAX	400
static const char *const crawl_keys[] = { "ok", "down", "up", "ctx", "quick", "1", "2", "3", "4", "5", "6", "7", "0" };
#define NKEYS	(sizeof crawl_keys / sizeof crawl_keys[0])

typedef struct {
	char path[96];
	char sig[40];
} node_t;
static node_t nodes[CRAWL_MAX];
static int n_nodes;

#define WORLD	"msg:3101234,Meet_at_the_north_lot call:3105678,3100,g wait:11000 "

static bool replay(const sim_geom_t *g, sim_map_t map, const char *path, ui_app_status_t *st)
{
	char err[300], buf[200];
	sim_reset(g, map);
	snprintf(buf, sizeof buf, WORLD "%s", path);
	if (!sim_run(buf, err, sizeof err))
		return false;
	ui_app_status(st);
	return true;
}

static void signature(const ui_app_status_t *st, char *sig, size_t len)
{
	snprintf(sig, len, "%s/%u/%s", st->screen, st->depth, st->locked ? "L" : "");
}

static void crawl(const sim_geom_t *g, sim_map_t map, const char *mapname)
{
	ui_app_status_t st;
	n_nodes = 0;
	if (!replay(g, map, "", &st))
		return;
	snprintf(nodes[0].path, sizeof nodes[0].path, "%s", "");
	signature(&st, nodes[0].sig, sizeof nodes[0].sig);
	n_nodes = 1;
	for (int i = 0; i < n_nodes; i++) {
		if (strlen(nodes[i].path) > 60)
			continue;
		for (size_t k = 0; k < NKEYS && n_nodes < CRAWL_MAX; k++) {
			char path[96];
			snprintf(path, sizeof path, "%s%s%s", nodes[i].path, nodes[i].path[0] ? " " : "", crawl_keys[k]);
			if (!replay(g, map, path, &st))
				continue;	// a key this map doesn't have
			if (strcmp(st.screen, "confirm") == 0 && strstr(path, "ok ok"))
				continue;
			char sig[40];
			signature(&st, sig, sizeof sig);
			bool seen = false;
			for (int j = 0; j < n_nodes && !seen; j++)
				seen = strcmp(nodes[j].sig, sig) == 0;
			if (seen)
				continue;
			snprintf(nodes[n_nodes].path, sizeof nodes[n_nodes].path, "%s", path);
			snprintf(nodes[n_nodes].sig, sizeof nodes[n_nodes].sig, "%s", sig);
			n_nodes++;
		}
	}
	// Field-use screens: the shortest way there is at most 3 levels (P4; quick settings and actions also open on
	// top of deeper screens, which is fine)
	for (int i = 0; i < n_nodes; i++) {
		char name[24];
		snprintf(name, sizeof name, "%s", nodes[i].sig);
		char *slash = strchr(name, '/');
		if (slash != NULL)
			*slash = 0;
		int best = 99;
		bool field = false;
		for (int j = 0; j < n_nodes; j++) {
			ui_app_status_t s;
			if (strncmp(nodes[j].sig, name, strlen(name)) != 0 || nodes[j].sig[strlen(name)] != '/')
				continue;
			unsigned d = 0;
			sscanf(nodes[j].sig + strlen(name) + 1, "%u", &d);
			if ((int)d < best)
				best = (int)d;
			if (j == i && replay(g, map, nodes[j].path, &s))
				field = s.field_use;
		}
		if (field && best > 3) {
			printf("FAIL crawl [%s] %s: field-use screen %d levels deep at best (P4)\n", mapname, name, best);
			fails++;
		}
	}
	// Check each screen found
	int screens = 0;
	for (int i = 0; i < n_nodes; i++) {
		ui_app_status_t s0, s1;
		char path[160], err[300];
		if (!replay(g, map, nodes[i].path, &s0) || s0.locked)
			continue;
		screens++;
		if (!s0.strip_drawn && strcmp(s0.screen, "diag") != 0) {
			printf("FAIL crawl [%s] %s: no status strip (P2)\n", mapname, s0.screen);
			fails++;
		}
		if (s0.depth > 0) {
			snprintf(path, sizeof path, "%s back", nodes[i].path);
			if (replay(g, map, path, &s1) && s1.depth >= s0.depth && strcmp(s1.screen, s0.screen) == 0) {
				printf("FAIL crawl [%s] %s: BACK doesn't leave it (P4) via '%s'\n", mapname, s0.screen,
				       nodes[i].path);
				fails++;
			}
		}
		snprintf(path, sizeof path, "%s long:back", nodes[i].path);
		if (replay(g, map, path, &s1) && (s1.depth != 0 || strcmp(s1.screen, "home") != 0) &&
				strcmp(s0.screen, "hotspot") != 0) {
			printf("FAIL crawl [%s] %s: long BACK doesn't reach Home (P4)\n", mapname, s0.screen);
			fails++;
		}
		if (map != SIM_MAP_F) {
			snprintf(path, sizeof path, "%s long:quick", nodes[i].path);
			if (replay(g, map, path, &s1) && !s1.locked) {
				printf("FAIL crawl [%s] %s: long QUICK doesn't lock (P3)\n", mapname, s0.screen);
				fails++;
			}
			snprintf(path, sizeof path, "%s long:ctx", nodes[i].path);
			if (replay(g, map, path, &s1) && strcmp(s1.screen, "recent") != 0) {
				printf("FAIL crawl [%s] %s: long CONTEXT isn't Recent (P3)\n", mapname, s0.screen);
				fails++;
			}
		}
		// PTT from any screen: the strip shows TX
		snprintf(path, sizeof path, "%s pttdown", nodes[i].path);
		if (replay(g, map, path, &s1)) {
			if (sim_view()->state != RS_TX && sim_view()->state != RS_TX_DENIED) {
				printf("FAIL crawl [%s] %s: PTT didn't transmit\n", mapname, s0.screen);
				fails++;
			} else if (!s1.strip_drawn && strcmp(s1.screen, "diag") != 0) {
				printf("FAIL crawl [%s] %s: no TX pill while transmitting (P2)\n", mapname, s0.screen);
				fails++;
			}
		}
		(void)err;
	}
	printf("# crawl [%s]: %d screens and states checked\n", mapname, screens);
}

int main(void)
{
	static const struct { sim_map_t map; const char *name, *geom; } runs[] = {
		{ SIM_MAP_K, "K DM-1701", "160x120i5" },
		{ SIM_MAP_M, "M GD-77", "128x64" },
		{ SIM_MAP_V, "V MD-9600", "160x128" },
		{ SIM_MAP_K, "K 240x320", "240x320" },
	};
	for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
		const sim_geom_t *g = sim_geom_find(runs[r].geom);
		for (size_t i = 0; i < sizeof flows / sizeof flows[0]; i++)
			run_flow(&flows[i], g, runs[r].map, runs[r].name);
	}
	const sim_geom_t *gf = sim_geom_find("160x128");
	for (size_t i = 0; i < sizeof flows_f / sizeof flows_f[0]; i++)
		run_flow(&flows_f[i], gf, SIM_MAP_F, "F few keys");
	printf("# %d flows run\n", flows_run);

	crawl(sim_geom_find("160x120i5"), SIM_MAP_K, "K DM-1701");
	crawl(sim_geom_find("128x64"), SIM_MAP_M, "M GD-77");
	crawl(gf, SIM_MAP_F, "F few keys");

	printf(fails ? "# FAILED uiFlowTest: %d\n" : "# PASSED uiFlowTest\n", fails);
	return fails != 0;
}
