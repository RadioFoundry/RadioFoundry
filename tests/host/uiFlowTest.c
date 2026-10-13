// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The automated UI checks of docs/ux/usability-tests.md 2, in the host simulator:
// - press budgets: each task flow of docs/ux/task-flows.md as a script on the reference key maps, at most its budget
//   (impossible on a key map that should have it = failure);
// - operate only: flows that only operate the radio never write the codeplug (P7);
// - crawler: every screen reachable from Home is left by BACK, long BACK reaches Home, field-use screens are at most
//   3 levels deep, the status strip (or the title row that carries the state pill) is drawn, PTT shows TX (P2, P4);
// - long presses keep their one meaning on every screen: long BACK Home, long QUICK lock, long CONTEXT Recent (P3);
// - the Transmitting screen names a private call's contact and tells it apart from a group call (P2).
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
	{ "19 hear all off on BACK", "quick 5 back", "back", "hearall=0", 1, false },
	{ "19 hear all ends with the channel", "quick 5 back", "up", "hearall=0", 1, false },
	{ "21 VOX on (FM)", "down", "quick 6", "vox=1", 2, false },
	{ "21 VOX off again", "down quick 6", "6", "vox=0", 1, false },
	{ "21 monitor latch (FM)", "down", "quick 5", "monitor=1", 2, false },
	{ "3 focus follows the row", "", "tt down", "focusid=91", 2, false },
	{ "3 focus cleared on Home", "tt down", "long:back", "focusid=0", 1, false },
	{ "20 timeslot", "", "quick 2 back", "ts=2 screen=home", 3, false },
	{ "20 color code 7", "", "quick 3 7 ok", "cc=7", 4, false },
	{ "20 save to channel", "quick 2", "long:ok", "writes=1", 1, true },
	// Settings the radio core acts on reach it (ui_svc_apply), key presses ask for a key beep
	{ "S TX timeout to the radio", "", "ok 6 3 down ok", "radio_tot=300", 5, false },
	{ "S backlight time to the radio", "", "ok 6 2 down ok", "radio_backlight=60", 5, false },
	{ "S key beep", "", "up", "beeps=1", 1, false },
	{ "S long press time to input", "", "ok 6 8 ok up ok", "longpress=700", 6, false },
	{ "S auto lock after 30 s", "ok 6 4 down down ok long:back", "wait:31000", "locked=1", 0, false },
	{ "S auto lock waits for keys", "ok 6 4 down down ok long:back", "wait:20000 up wait:20000", "locked=0", 1, false },
	// DMR repeater roaming (docs/spec/dmr-roaming.md 6.2): Tools 8
	{ "R roaming on for this zone", "", "ok 5 8 1", "roamzone=1 roam=1 toast=Roaming_on_for", 4, false },
	{ "R roaming off again", "ok 5 8 1", "1", "roamzone=0 roam=0 toast=Roaming_off", 1, false },
	{ "R roam now", "ok 5 8 1 long:back", "ok 5 8 2", "roams=1 screen=home toast=Moved_to", 4, false },
	{ "R lock to this site", "ok 5 8 1", "3", "roam=3 toast=Locked_to", 1, false },
	{ "R roam now ends the lock", "ok 5 8 1 3", "2", "roam=1 roams=1", 1, false },
	{ "R not on the VFO", "ok 4 up ok", "ok 5 8 1", "roamzone=0 toast=Roaming_works", 4, false },
	// The survey and beacon rows (set33 survey, set34 beacon interval, set36 wake probe): radio-wide, OK cycles
	{ "R site survey per site", "", "ok 5 8 4", "set33=2", 4, false },
	{ "R beacon interval", "", "ok 5 8 5", "set34=120 toast=Must_match", 4, false },
	{ "R wake probe on", "", "ok 5 8 7", "set36=1 toast=Wake_probe_on", 4, false },
	{ "T22 Home and undo", "quick 2 back ok 6", "long:back back", "screen=home temporary=0", 2, false },
	// Settings that act now (uifix). set<N> = ui_setting_t N: 7 theme, 31 privacy key type, 32 privacy key
	{ "S PTT on when locked (default)", "long:quick", "ptt", "tx=1 pttblock=0", 1, false },
	{ "S PTT off when locked", "ok 6 4 down down down ok long:back long:quick", "ptt",
	  "tx=0 pttblock=1 toast=PTT_is_off", 1, false },
	{ "S PTT back with the unlock", "ok 6 4 down down down ok long:back long:quick", "long:quick ptt",
	  "tx=1 pttblock=0", 2, false },
	{ "S Up/Down on Home: zone", "ok 6 8 down down ok long:back", "up", "zone=1 screen=home", 1, false },
	{ "S Up/Down on Home: talk target", "ok 6 8 down down ok ok long:back", "up", "talk=91 toast=Talking_to", 1,
	  false },
	{ "S Up/Down: back to the default", "ok 6 8 down down ok ok long:back up", "down", "talk=3100", 1, false },
	{ "S hear all by default", "ok 6 3 down down down down ok long:back", "up up", "hearall=1 pos=5", 2, false },
	{ "S hear all by default: BACK", "ok 6 3 down down down down ok long:back up up", "back", "hearall=0", 1, false },
	{ "S message tone", "", "msg:3101234,Hi", "beep=2", 0, false },
	{ "S low battery warning", "", "battery:14", "banner=Battery_14%", 0, false },
	{ "S privacy key typed", "", "ok 6 4 ok down ok 1 2 3 ok", "set31=1 set32=123", 10, false },
	{ "S privacy key out of range", "ok 6 4 ok down", "ok 3 0 0 ok", "toast=Not_changed set32=0", 5, false },
	{ "S privacy key needs a type", "ok 6 4 down", "ok", "toast=Pick_a_key", 1, false },
	{ "S number typed", "", "ok 6 3 down down ok 3 ok", "sq=5 set12=3", 8, false },
	{ "S voice prompts not built", "ok 6 1 down down down", "ok", "toast=Voice_prompts", 1, false },
	{ "S mode tile in the VFO", "ok 4 up ok", "quick 4", "toast=VFO:_FM_only mode=fm", 2, false },
	{ "S scan in the VFO: the band", "ok 4 up ok", "ok 5 1", "toast=Scanning_the_band scanning=1", 3, false },
	{ "S scan on a channel: the zone", "", "ok 5 1", "toast=Scanning_this_zone scanning=1", 3, false },
	{ "S USB network on", "", "ok 5 4 ok", "usbnet=1 toast=USB_is_a_network screen=home", 4, false },
	{ "S USB network off", "ok 5 4 ok", "ok 5 4 ok", "usbnet=0 toast=USB_is_a_serial", 4, false },
	{ "S message failed text", "", "", "screen=home", 0, false },
	// APRS (docs/spec/aprs.md 10): Tools 9, one row each
	{ "A APRS page", "", "ok 5 9", "screen=aprs", 3, false },
	{ "A send position now", "", "ok 5 9 1", "aprs_sent=1 toast=Sending_position", 4, false },
	{ "A send without a callsign", "aprs:nocall", "ok 5 9 1", "aprs_sent=0 toast=Set_your_callsign", 4, false },
	{ "A beacons on", "", "ok 5 9 2", "aprs_auto=1 toast=Beacons_on", 4, false },
	{ "A beacons off again", "ok 5 9 2", "2", "aprs_auto=0 toast=Beacons_off", 1, false },
	{ "A next region", "", "ok 5 9 3", "aprs_region=1", 4, false },
	{ "A regions skip an unset custom", "ok 5 9 3 3 3 3 3", "3", "aprs_region=0", 1, false },
	{ "A regions with a custom frequency", "aprs:custom ok 5 9 3 3 3 3 3", "3", "aprs_region=6", 1, false },
	{ "A power as the channel", "ok 5 9", "down down down", "shows=As_channel aprs_power=0", 3, false },
	{ "A power of its own", "", "ok 5 9 4", "aprs_power=1", 4, false },
	{ "A path shows the default", "ok 5 9", "down down down down", "shows=WIDE1-1,WIDE2-1 aprs_path=2", 4, false },
	{ "A path: the next choice", "", "ok 5 9 5", "aprs_path=3 shows=WIDE2-1", 4, false },
	{ "A path: WIDE2-2 then none", "ok 5 9 5 5", "5", "aprs_path=0 shows=None", 1, false },
	{ "A path wraps to WIDE1-1", "ok 5 9 5 5 5", "5", "aprs_path=1", 1, false },
	{ "A interval", "", "ok 5 9 6", "aprs_interval=60", 4, false },
	{ "A SmartBeaconing", "", "ok 5 9 7", "aprs_smart=1", 4, false },
	{ "A send via DMR", "", "ok 5 9 8", "aprs_via=1", 4, false },
	{ "A position page", "", "ok 5 9 9", "screen=aprs-pos", 4, false },
	{ "A position source", "ok 5 9 9", "1", "aprs_pos=1", 1, false },
	{ "A no fixed position yet", "", "ok 5 9 9", "shows=Not_set", 4, false },
	{ "A position page found by latitude", "", "5 2 8 4 8 8 3 3", "shows=APRS_position", 8, false },
	{ "A latitude while typed", "ok 5 9 9", "2 3 3 7 4", "shows=33.74 aprs_lat=0", 5, false },
	{ "A fixed latitude", "ok 5 9 9", "2 3 3 7 4 9 0 ok", "aprs_lat=337490000", 8, false },
	{ "A fixed longitude west", "ok 5 9 9", "3 0 8 4 3 8 8 0 down ok", "aprs_lon=-843880000", 10, false },
	{ "A longitude's leading zero by itself", "ok 5 9 9", "3 8 4 3 8 8 0 down ok", "aprs_lon=-843880000", 9, false },
	{ "A longitude of 100 or more", "ok 5 9 9", "3 1 2 2 5 ok", "aprs_lon=1225000000", 6, false },
	{ "A latitude digits not typed are zeros", "ok 5 9 9", "2 4 5 ok", "aprs_lat=450000000", 4, false },
	{ "A latitude past 90 refused", "ok 5 9 9", "2 9 5", "toast=Up_to_90", 3, false },
	{ "A latitude after a refused digit", "ok 5 9 9", "2 9 5 ok", "aprs_lat=900000000", 4, false },
	{ "A latitude BACK takes a digit back", "ok 5 9 9", "2 3 9 back 3 ok", "aprs_lat=330000000", 6, false },
	{ "A latitude BACK leaves unchanged", "ok 5 9 9", "2 1 back back", "aprs_lat=0 screen=aprs-pos", 4, false },
	{ "A position in DMR calls", "", "ok 5 9 0", "aprs_dmrgps=1 toast=Position_goes_out", 4, false },
	{ "A KISS on USB", "ok 5 9", "up up ok", "aprs_kiss=1", 3, false },
	{ "A stations heard", "", "ok 5 9 up ok", "screen=aprs-heard count=2", 5, false },
	{ "A not in this build", "aprs:off", "ok 5 9 2", "toast=APRS:_not aprs_auto=0", 4, false },
};

// DM-1701 key map only: the top side key, long waits
static const flow_t flows_k[] = {
	{ "S theme light (color screens)", "", "ok 6 2 down down ok", "set7=1", 6, false },
	{ "E emergency: hold the top key", "", "long:emerg", "toast=EMERGENCY_sent", 1, false },
	{ "E emergency: hold again cancels", "long:emerg", "long:emerg", "toast=Emergency_canceled", 1, false },
	{ "E top key short: power", "", "user3", "power=5", 1, false },
	// Side keys (Settings > Keys rows 4-9, core/input/keyassign.h; set42 = side key 1 ... set47 = top key hold)
	{ "K side key 1: the next choice", "ok 6 8", "down down down ok", "set42=1 shows=Nothing", 4, false },
	{ "K side key 1: monitor", "ok 6 8 down down down ok", "ok", "set42=2", 1, false },
	{ "K side key 1 hold: none while it monitors", "ok 6 8", "down down down down ok", "set43=0 toast=Monitor_holds",
	  5, false },
	{ "K side key 1 hold once it doesn't", "ok 6 8 down down down ok down", "ok", "set43=1", 1, false },
	{ "K side key 2 hold", "ok 6 8", "down down down down down down ok", "set45=1", 7, false },
	{ "K top key hold: emergency after scan", "ok 6 8 up ok ok ok ok ok ok", "ok", "set47=8 shows=Emergency", 1,
	  false },
	{ "K top key hold wraps to default", "ok 6 8 up ok ok ok ok ok ok ok", "ok", "set47=0", 1, false },
	{ "K a key set to power", "", "powerkey", "power=5", 1, false },
	{ "K a key set to power, long", "", "long:powerkey", "power=5", 1, false },
	{ "K a key set to the next zone", "", "zone+", "zone=1", 1, false },
	{ "K a key set to the previous zone", "", "zone-", "zone=2", 1, false },
	{ "K a key set to scan, from a menu", "ok 6", "scankey", "scanning=1", 1, false },
	{ "K talk target on a long press", "", "long:tt", "screen=picker", 1, false },
	// The encoder: forward = the next channel on Home, the next row down a list, the next quick tile; back = up
	{ "N encoder: next channel", "", "enc+", "pos=4 screen=home", 1, false },
	{ "N encoder: previous channel", "", "enc-", "pos=2 screen=home", 1, false },
	{ "N encoder: down a list", "ok", "enc+ enc+", "focus=Recent", 2, false },
	{ "N encoder: back up a list", "ok enc+ enc+", "enc-", "focus=Messages", 1, false },
	{ "N encoder: wraps up to the last row", "ok", "enc-", "focus=Settings", 1, false },
	{ "N encoder: a number goes up", "ok 6 2 ok", "enc+", "focus=Brightness", 1, false },
	{ "P auto power off warns", "ok 6 7 down ok long:back", "wait:1741000", "banner=Switching_off poweroff=0", 0,
	  false },
	{ "P auto power off", "ok 6 7 down ok long:back", "wait:1801000", "poweroff=1", 0, false },
	{ "P a key keeps it on", "ok 6 7 down ok long:back wait:1700000", "up wait:200000", "poweroff=0", 1, false },
	{ "P the knob holds it", "ok 6 7 down ok long:back wait:1801000", "offfailed", "poweroff=0 banner=Couldn't", 0,
	  false },
};

// Few-key radios (7.5): fallbacks
static const flow_t flows_f[] = {
	{ "F next channel", "", "up", "pos=4", 1, false },
	{ "F quick settings (long OK)", "", "long:ok", "screen=quick", 1, false },
	{ "F last caller (Hub item 0)", CALLER, "ok ok ptt", "txid=91", 3, false },
	{ "F zone", "", "ok down down down down ok down ok", "zone=1", 8, false },
	{ "F no side keys to set", "ok up ok up up up ok", "down down down ok", "toast=This_radio_has_no set42=0", 4,
	  false },
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
		char name[sizeof nodes[i].sig];
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
		// About has no strip while idle (the owner's call); it comes back with RX / TX (checked below with PTT)
		if (!s0.strip_drawn && strcmp(s0.screen, "diag") != 0 && strcmp(s0.screen, "about") != 0) {
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

// About (Settings > Advanced): every word readable at every display class (nothing shortened with an ellipsis, all
// the way to the end), no status strip while idle, the strip back while transmitting
static void about_checks(void)
{
	char err[300];
	for (int g = 0; g < sim_geom_count; g++) {
		const char *gn = sim_geoms[g].name;
		ui_app_status_t st;
		sim_reset(&sim_geoms[g], SIM_MAP_K);
		if (!sim_run("ok 6 0 ok", err, sizeof err)) {
			printf("FAIL about [%s]: %s\n", gn, err);
			fails++;
			continue;
		}
		sim_rec.shortened = 0;
		sim_run("wait:200 down down down down down down down down down down down down down down down down down down down "
			"down down down", err, sizeof err);
		ui_app_status(&st);
		if (strcmp(st.screen, "about") != 0 || sim_rec.shortened != 0) {
			printf("FAIL about [%s]: on %s, %u texts shortened\n", gn, st.screen, (unsigned)sim_rec.shortened);
			fails++;
		}
		if (st.strip_drawn) {
			printf("FAIL about [%s]: the status strip is drawn while idle\n", gn);
			fails++;
		}
		sim_run("pttdown wait:200", err, sizeof err);
		ui_app_status(&st);
		if (!st.strip_drawn) {
			printf("FAIL about [%s]: no strip while transmitting (P2)\n", gn);
			fails++;
		}
		sim_run("pttup", err, sizeof err);
	}
}

// Transmitting, at every display class: a private call shows the contact's name, even a 16-character one (the ID
// gives way first), and its title says "Private" where a group call's has "TG"; a group call never says "Private"
static void call_screen_checks(void)
{
	static const struct { const char *name, *script, *expect; } cases[] = {
		{ "private to a contact", "3 1 0 1 2 3 4 ok wait:5000 pttdown", "txpriv=1 shows=Joe_Smith shows=Private_310" },
		{ "private, long name", "worst 3 1 0 9 9 9 9 ok wait:5000 pttdown",
		  "txpriv=1 shows=Christo shows=Wood shows=Private_310" },
		{ "private, no contact", "3 1 0 5 5 5 5 down ok wait:5000 pttdown", "txpriv=1 shows=3105555 shows=Private_310" },
		{ "group", "3 1 0 0 ok wait:5000 pttdown", "txpriv=0 shows=TG_3100 hides=Private" },
	};
	char err[300], buf[200];
	for (int g = 0; g < sim_geom_count; g++) {
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			sim_reset(&sim_geoms[g], SIM_MAP_K);
			snprintf(buf, sizeof buf, "%s", cases[i].script);
			bool ok = sim_run(buf, err, sizeof err);
			char *o = buf;	// "a=b c=d" -> "expect:a=b expect:c=d"
			for (const char *p = cases[i].expect; ok && *p && o < buf + sizeof buf - 16;) {
				while (*p == ' ')
					p++;
				memcpy(o, "expect:", 7);
				o += 7;
				while (*p && *p != ' ' && o < buf + sizeof buf - 2)
					*o++ = *p++;
				*o++ = ' ';
			}
			*o = 0;
			if (ok)
				ok = sim_run(buf, err, sizeof err);
			if (!ok) {
				printf("FAIL call screen %s [%s]: %s\n", cases[i].name, sim_geoms[g].name, err);
				fails++;
			}
		}
	}
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
	for (size_t i = 0; i < sizeof flows_k / sizeof flows_k[0]; i++)
		run_flow(&flows_k[i], sim_geom_find("160x120i5"), SIM_MAP_K, "K DM-1701");
	const sim_geom_t *gf = sim_geom_find("160x128");
	for (size_t i = 0; i < sizeof flows_f / sizeof flows_f[0]; i++)
		run_flow(&flows_f[i], gf, SIM_MAP_F, "F few keys");
	printf("# %d flows run\n", flows_run);
	about_checks();
	call_screen_checks();

	crawl(sim_geom_find("160x120i5"), SIM_MAP_K, "K DM-1701");
	crawl(sim_geom_find("128x64"), SIM_MAP_M, "M GD-77");
	crawl(gf, SIM_MAP_F, "F few keys");

	printf(fails ? "# FAILED uiFlowTest: %d\n" : "# PASSED uiFlowTest\n", fails);
	return fails != 0;
}
