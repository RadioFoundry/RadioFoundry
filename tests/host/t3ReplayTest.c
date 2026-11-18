// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine (core/dmr/trunk/t3_ms) on off-air recordings: the bursts and short LCs the
// SDR bench decoded from a live Tier III system's control channel and one of its payload channels, fed in time order.
// The engine hears a channel's records only while it is tuned there, as a radio would.
//
// The recordings, and what is known about the system they came from, are not in the repository. Point
// RFOUNDRY_T3_RECORDINGS at a manifest; without it the test skips.
//
//   RFOUNDRY_T3_RECORDINGS=<manifest> make -C tests/host run-t3replay
//
// The manifest, one item per line (# comments; paths relative to the manifest):
//   tscc <file> <rx_hz> <lcn>               the control channel's recording
//   payload <file> <rx_hz>                  a payload channel's recording
//   plan <tx_hz> <first> <sep> <tx_low> <split_hz>   the fixed plan (t3_fixed_plan_t)
//   system <model> <net> <site> <par> <cc>  the system code and the TSCC's color code
//   aloha <version> <nrand_wait> <backoff> <reg>
//   tg <id>...                              the talkgroups granted in the recording
//   lcn <n>...                              the payload channels granted
//   payload_tg <id>                         a talkgroup whose calls the payload recording holds
//   negative <file> <rx_hz>                 a control channel that isn't Tier III: never camp
//
// A recording is one JSON object per line:
//   {"k": "b", "dt": "CSBK", "slot": 1, "cc": 1, "d": "<12 octets hex>", "t": "<ISO 8601>", ...}   a burst
//   {"k": "slc", "ok": true, "raw": "<28 bits hex>", "t": ..., ...}                               a short LC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/trunk/t3_ms.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- The manifest ----

#define MAN_MAX 32

static struct {
	char dir[512];
	char tscc[512], payload[512];
	uint32_t tscc_hz, payload_hz;
	uint16_t tscc_lcn;
	t3_fixed_plan_t plan;
	uint8_t model, par, cc;
	uint16_t net, site;
	uint8_t version, nrand, backoff, reg;
	uint32_t tg[MAN_MAX];
	int ntg;
	uint16_t lcn[MAN_MAX];
	int nlcn;
	uint32_t payload_tg;
	char neg[4][512];
	uint32_t neg_hz[4];
	int nneg;
} man;

static void man_path(char *out, const char *rel)
{
	if (rel[0] == '/') {
		snprintf(out, 512, "%s", rel);
	} else {
		snprintf(out, 512, "%s/%s", man.dir, rel);
	}
}

static bool man_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("cannot open %s\n", path);
		return false;
	}
	snprintf(man.dir, sizeof(man.dir), "%s", path);
	char *slash = strrchr(man.dir, '/');
	if (slash != NULL) {
		*slash = 0;
	} else {
		snprintf(man.dir, sizeof(man.dir), ".");
	}
	char line[1024], key[32], a[512];
	while (fgets(line, sizeof(line), f)) {
		unsigned long v[6] = { 0 };
		if (line[0] == '#' || sscanf(line, "%31s", key) != 1) {
			continue;
		}
		const char *rest = line + strlen(key);
		if (strcmp(key, "tscc") == 0 && sscanf(rest, "%511s %lu %lu", a, &v[0], &v[1]) == 3) {
			man_path(man.tscc, a);
			man.tscc_hz = (uint32_t)v[0];
			man.tscc_lcn = (uint16_t)v[1];
		} else if (strcmp(key, "payload") == 0 && sscanf(rest, "%511s %lu", a, &v[0]) == 2) {
			man_path(man.payload, a);
			man.payload_hz = (uint32_t)v[0];
		} else if (strcmp(key, "negative") == 0 && man.nneg < 4 && sscanf(rest, "%511s %lu", a, &v[0]) == 2) {
			man_path(man.neg[man.nneg], a);
			man.neg_hz[man.nneg++] = (uint32_t)v[0];
		} else if (strcmp(key, "plan") == 0 && sscanf(rest, "%lu %lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3],
							      &v[4]) == 5) {
			man.plan = (t3_fixed_plan_t){ (uint32_t)v[0], (uint16_t)v[1], (uint8_t)v[2], v[3] != 0,
						      (uint32_t)v[4] };
		} else if (strcmp(key, "system") == 0 && sscanf(rest, "%lu %lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3],
								&v[4]) == 5) {
			man.model = (uint8_t)v[0];
			man.net = (uint16_t)v[1];
			man.site = (uint16_t)v[2];
			man.par = (uint8_t)v[3];
			man.cc = (uint8_t)v[4];
		} else if (strcmp(key, "aloha") == 0 &&
			   sscanf(rest, "%lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3]) == 4) {
			man.version = (uint8_t)v[0];
			man.nrand = (uint8_t)v[1];
			man.backoff = (uint8_t)v[2];
			man.reg = (uint8_t)v[3];
		} else if (strcmp(key, "payload_tg") == 0) {
			man.payload_tg = (uint32_t)strtoul(rest, NULL, 10);
		} else if (strcmp(key, "tg") == 0 || strcmp(key, "lcn") == 0) {
			char *p = (char *)rest, *end;
			for (;;) {
				unsigned long x = strtoul(p, &end, 10);
				if (end == p) {
					break;
				}
				p = end;
				if (key[0] == 't' && man.ntg < MAN_MAX) {
					man.tg[man.ntg++] = (uint32_t)x;
				} else if (key[0] == 'l' && man.nlcn < MAN_MAX) {
					man.lcn[man.nlcn++] = (uint16_t)x;
				}
			}
		} else {
			printf("manifest: can't read: %s", line);
			fclose(f);
			return false;
		}
	}
	fclose(f);
	return man.tscc[0] != 0 && man.payload[0] != 0 && man.plan.base_hz != 0;
}

enum { R_BURST, R_VOICE, R_SLC };

typedef struct {
	uint32_t t;		// ms
	uint32_t hz;		// the channel the record came from (MS receive)
	uint8_t kind;
	uint8_t dt;
	uint8_t slot;
	uint8_t cc;
	uint32_t slc;
	uint8_t d[T3_CSBK_BYTES];
} rec_t;

static rec_t *recs;
static size_t nrecs, caprecs;

static const char *val(const char *line, const char *key)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (p == NULL) {
		return NULL;
	}
	p += strlen(pat);
	while (*p == ' ') {
		p++;
	}
	return p;
}

static bool sval(const char *line, const char *key, char *out, size_t n)
{
	const char *p = val(line, key);
	if (p == NULL || *p != '"') {
		return false;
	}
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n) {
		out[i++] = *p++;
	}
	out[i] = 0;
	return true;
}

static long ival(const char *line, const char *key)
{
	const char *p = val(line, key);
	return p != NULL ? strtol(p, NULL, 10) : -1;
}

// "2026-11-04T18:18:06.581+00:00" to ms since the start of the month (recordings never span a month)
static uint32_t iso_ms(const char *s)
{
	int y, mo, d, h, mi;
	double sec;
	if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) {
		return 0;
	}
	return (uint32_t)((((d * 24 + h) * 60) + mi) * 60000) + (uint32_t)(sec * 1000.0 + 0.5);
}

static uint8_t dt_code(const char *dt)
{
	if (strcmp(dt, "CSBK") == 0) {
		return T3_DT_CSBK;
	}
	if (strcmp(dt, "MBC_H") == 0) {
		return T3_DT_MBC_HDR;
	}
	if (strcmp(dt, "MBC_C") == 0) {
		return T3_DT_MBC_CONT;
	}
	return 0xFF;
}

static bool is_voice(const char *dt)
{
	return strcmp(dt, "VOICE") == 0 || strcmp(dt, "VOICE_A") == 0 || strcmp(dt, "VLC") == 0 ||
	       strcmp(dt, "TLC") == 0;
}

static void push(const rec_t *r)
{
	if (nrecs == caprecs) {
		caprecs = caprecs ? caprecs * 2 : 65536;
		recs = realloc(recs, caprecs * sizeof(*recs));
		if (recs == NULL) {
			printf("out of memory\n");
			exit(2);
		}
	}
	recs[nrecs++] = *r;
}

static size_t load(const char *path, uint32_t hz)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("cannot open %s\n", path);
		failures++;
		return 0;
	}
	static char line[4096];
	size_t n0 = nrecs;
	while (fgets(line, sizeof(line), f)) {
		char k[8], t[40], s[40];
		rec_t r = { 0 };
		if (!sval(line, "k", k, sizeof(k)) || !sval(line, "t", t, sizeof(t))) {
			continue;
		}
		r.t = iso_ms(t);
		r.hz = hz;
		if (strcmp(k, "slc") == 0) {
			const char *ok = val(line, "ok");
			if (ok == NULL || strncmp(ok, "true", 4) != 0 || !sval(line, "raw", s, sizeof(s))) {
				continue;
			}
			r.kind = R_SLC;
			r.slc = (uint32_t)strtoul(s, NULL, 16);
			push(&r);
			continue;
		}
		if (strcmp(k, "b") != 0 || !sval(line, "dt", s, sizeof(s))) {
			continue;
		}
		r.slot = (uint8_t)ival(line, "slot");
		r.cc = (uint8_t)ival(line, "cc");
		if (is_voice(s)) {
			r.kind = R_VOICE;
			push(&r);
			continue;
		}
		r.dt = dt_code(s);
		char hex[32];
		if (r.dt == 0xFF || !sval(line, "d", hex, sizeof(hex)) || strlen(hex) != 2 * T3_CSBK_BYTES) {
			continue;
		}
		for (int i = 0; i < T3_CSBK_BYTES; i++) {
			unsigned v;
			sscanf(hex + 2 * i, "%2x", &v);
			r.d[i] = (uint8_t)v;
		}
		r.kind = R_BURST;
		push(&r);
	}
	fclose(f);
	return nrecs - n0;
}

static int by_time(const void *a, const void *b)
{
	const rec_t *x = a, *y = b;
	if (x->t != y->t) {
		return x->t < y->t ? -1 : 1;
	}
	return x->hz < y->hz ? -1 : x->hz > y->hz;
}

// ---- One run of the engine over what is loaded ----

#define FOLLOWS_MAX 2048

typedef struct {
	t3_call_t call;
	uint32_t end;
	uint32_t voice;		// voice bursts heard on the call's slot
} follow_t;

typedef struct {
	uint32_t first_camp;	// ms after the start, 0 = never
	uint32_t camped_ms;	// time spent camped or following
	follow_t f[FOLLOWS_MAX];
	int nf;
	t3_ms_t ms;
} run_t;

static void replay(run_t *run, const t3_cfg_t *cfg)
{
	memset(run, 0, sizeof(*run));
	t3_ms_t *ms = &run->ms;
	uint32_t t0 = recs[0].t, now = 1000, prev = 1000;
	uint32_t tuned = 0;
	uint8_t tuned_slot = 0;
	t3_ms_start(ms, cfg, now);
	for (size_t i = 0; i < nrecs; i++) {
		const rec_t *r = &recs[i];
		now = 1000u + (r->t - t0);
		if (ms->state == T3_CAMPED || ms->state == T3_FOLLOW) {
			run->camped_ms += now - prev;
		}
		prev = now;
		t3_state_t before = ms->state;
		t3_act_t a;
		while (t3_ms_poll(ms, now, &a)) {
			tuned = a.chan.rx_hz;
			tuned_slot = a.slot;
		}
		if (before == T3_FOLLOW && ms->state != T3_FOLLOW && run->nf > 0) {
			run->f[run->nf - 1].end = now;
		}
		if (r->hz != tuned) {
			continue;
		}
		before = ms->state;
		uint32_t followed = ms->st.followed;
		switch (r->kind) {
		case R_SLC:
			t3_ms_slc(ms, now, r->slc);
			break;
		case R_VOICE:
			t3_ms_activity(ms, now, r->slot);
			if (ms->state == T3_FOLLOW && r->slot == tuned_slot && run->nf > 0) {
				run->f[run->nf - 1].voice++;
			}
			break;
		default:
			t3_ms_rx(ms, now, r->slot, r->dt, r->cc, r->d);
			break;
		}
		if (run->first_camp == 0 && ms->state == T3_CAMPED) {
			run->first_camp = now - 1000u;
		}
		if (before == T3_FOLLOW && ms->state != T3_FOLLOW && run->nf > 0) {
			run->f[run->nf - 1].end = now;
		}
		if (ms->st.followed != followed && run->nf < FOLLOWS_MAX) {
			if (before == T3_FOLLOW && run->nf > 0) {
				run->f[run->nf - 1].end = now;
			}
			run->f[run->nf].call = ms->call;
			run->f[run->nf].end = 0;
			run->f[run->nf].voice = 0;
			run->nf++;
		}
		// The radio retunes at once; the engine's next poll hands it over
		while (t3_ms_poll(ms, now, &a)) {
			tuned = a.chan.rx_hz;
			tuned_slot = a.slot;
		}
	}
}

static void base_cfg(t3_cfg_t *cfg, uint32_t ctrl_hz, uint16_t lcn)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->own_id = 1u;
	cfg->auth.model = man.model;
	cfg->auth.net = man.net;
	cfg->auth.category = T3_PAR_A;
	cfg->plan.fixed = man.plan;
	cfg->plan.learn = true;
	cfg->ctrl[0] = (t3_chan_t){ .lcn = lcn, .rx_hz = ctrl_hz, .tx_hz = 0, .cc = T3_CC_ANY };
	cfg->nctrl = 1;
	cfg->opts = T3_OPT_LISTEN_ONLY | T3_OPT_PRIVATE;
}

static bool plan_channel(const t3_chan_t *c)
{
	t3_chan_t want;
	for (int i = 0; i < man.nlcn; i++) {
		if (c->lcn == man.lcn[i]) {
			return t3_fixed_lookup(&man.plan, c->lcn, &want) && c->rx_hz == want.rx_hz &&
			       c->tx_hz == want.tx_hz;
		}
	}
	return false;
}

static bool known_tg(uint32_t tg)
{
	for (int i = 0; i < man.ntg; i++) {
		if (man.tg[i] == tg) {
			return true;
		}
	}
	return false;
}

static void test_tier3(void)
{
	nrecs = 0;
	size_t nc = load(man.tscc, man.tscc_hz);
	size_t np = load(man.payload, man.payload_hz);
	if (nc == 0 || np == 0) {
		return;
	}
	t3_chan_t pc;
	uint16_t payload_lcn = 0;
	for (int i = 0; i < man.nlcn; i++) {
		if (t3_fixed_lookup(&man.plan, man.lcn[i], &pc) && pc.rx_hz == man.payload_hz) {
			payload_lcn = man.lcn[i];
		}
	}
	CHECK(payload_lcn != 0, "the payload recording's frequency is one of the plan's granted channels");
	qsort(recs, nrecs, sizeof(*recs), by_time);
	uint32_t span = recs[nrecs - 1].t - recs[0].t, gaps = 0, last_c = 0;
	for (size_t i = 0; i < nrecs; i++) {
		if (recs[i].hz == man.tscc_hz) {
			gaps += last_c != 0 && recs[i].t - last_c > T3_NOSIG_MS;
			last_c = recs[i].t;
		}
	}
	printf("Tier III recording: %zu control channel records, %zu payload, %u s\n", nc, np, span / 1000u);

	static run_t run;
	t3_cfg_t cfg;

	// A scanner's view: every talkgroup
	base_cfg(&cfg, man.tscc_hz, man.tscc_lcn);
	cfg.opts |= T3_OPT_MONITOR;
	replay(&run, &cfg);
	const t3_ms_t *ms = &run.ms;
	CHECK(run.first_camp != 0 && run.first_camp < 1500, "camped after %u ms", run.first_camp);
	CHECK(ms->confirmed && ms->tscc.cc == man.cc && ms->tscc_slots != 0, "TSCC cc %u slots %u", ms->tscc.cc,
	      ms->tscc_slots);
	t3_syscode_t s;
	t3_syscode_split(ms->syscode, &s);
	CHECK(s.model == man.model && s.net == man.net && s.site == man.site && s.par == man.par,
	      "syscode model %u net %u site %u par %u", s.model, s.net, s.site, s.par);
	CHECK(ms->aloha.version == man.version && ms->aloha.nrand_wait == man.nrand &&
	      ms->aloha.backoff == man.backoff && ms->aloha.reg == man.reg,
	      "Aloha version %u nrand %u backoff %u reg %u", ms->aloha.version, ms->aloha.nrand_wait, ms->aloha.backoff,
	      ms->aloha.reg);
	CHECK(ms->reg == (man.reg ? T3_REG_LISTEN : T3_REG_NOT_NEEDED), "reg %s", t3_reg_name(ms->reg));
	CHECK(ms->csc_valid, "common slot counter");
	CHECK(ms->st.foreign == 0 && ms->st.unmapped == 0, "foreign %u unmapped %u", ms->st.foreign, ms->st.unmapped);
	CHECK(run.camped_ms > span / 100u * 95u, "camped or following %u of %u s", run.camped_ms / 1000u, span / 1000u);
	// T_Nosig hunts: one at the start, one per gap in the recording longer than T_Nosig, a few for fades
	CHECK(ms->st.hunts <= 1u + gaps + gaps / 4u + 2u, "%u hunts, %u gaps in the recording", ms->st.hunts, gaps);
	CHECK(run.nf >= 20, "followed %d calls", run.nf);
	int bad = 0, onp = 0, voiced = 0;
	for (int i = 0; i < run.nf; i++) {
		const t3_call_t *c = &run.f[i].call;
		if (!plan_channel(&c->chan) || !known_tg(c->dst) || !c->group || c->slot < 1 || c->slot > 2) {
			if (bad++ < 5) {
				printf("  unexpected follow: tg %u lcn %u %u Hz slot %u\n", c->dst, c->chan.lcn,
				       c->chan.rx_hz, c->slot);
			}
		}
		if (c->chan.lcn == payload_lcn) {
			onp++;
			voiced += run.f[i].voice > 10;
		}
	}
	CHECK(bad == 0, "%d follows off the plan or to unknown talkgroups", bad);
	CHECK(onp > 0 && voiced > 0, "calls on the recorded payload channel: %d, with voice heard: %d", onp, voiced);
	printf("  monitor: %d calls followed (%d on the recorded payload channel, %d with voice), %u grants, %u returns,"
	       " %u hunts, %u CRC bad, %u foreign short LCs\n", run.nf, onp, voiced, ms->st.grants, ms->st.returns,
	       ms->st.hunts, ms->st.crc_bad, ms->st.foreign_slc);

	// One talkgroup: only its calls
	base_cfg(&cfg, man.tscc_hz, man.tscc_lcn);
	cfg.tg[0] = (t3_tg_t){ man.payload_tg, T3_TG_LISTEN };
	cfg.ntg = 1;
	replay(&run, &cfg);
	int others = 0, long_voice = 0;
	for (int i = 0; i < run.nf; i++) {
		others += run.f[i].call.dst != man.payload_tg;
		if (run.f[i].call.chan.lcn == payload_lcn && run.f[i].voice > 10 && run.f[i].end != 0 &&
		    run.f[i].end - run.f[i].call.start_ms > T3_INACTIVE_MS) {
			long_voice++;
		}
	}
	CHECK(run.nf > 0 && others == 0, "%d follows, %d to other talkgroups", run.nf, others);
	CHECK(long_voice > 0, "a call on the payload channel kept by its voice past the inactivity time");
	printf("  one talkgroup: %d calls followed, %d held by voice\n", run.nf, long_voice);

	// A system code the radio doesn't hold: never camps
	base_cfg(&cfg, man.tscc_hz, man.tscc_lcn);
	cfg.auth.net = (uint16_t)(man.net + 1u);
	cfg.opts |= T3_OPT_MONITOR;
	replay(&run, &cfg);
	CHECK(run.first_camp == 0 && run.nf == 0 && run.ms.st.foreign > 0, "another network: camped at %u, foreign %u",
	      run.first_camp, run.ms.st.foreign);
}

// A system that isn't Tier III (its control channel's CSBKs don't pass the standard CRC): the engine never camps
static void test_not_tier3(const char *path, uint32_t hz)
{
	nrecs = 0;
	if (load(path, hz) == 0) {
		return;
	}
	qsort(recs, nrecs, sizeof(*recs), by_time);
	static run_t run;
	t3_cfg_t cfg;
	base_cfg(&cfg, hz, 0);
	cfg.opts |= T3_OPT_MONITOR | T3_OPT_LEARN_SYSCODE;
	replay(&run, &cfg);
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	CHECK(run.first_camp == 0 && run.nf == 0, "%s: camped at %u ms, %d follows", name, run.first_camp, run.nf);
	printf("  not Tier III, %u Hz: %zu records, never camped, %u CRC bad\n", hz, nrecs, run.ms.st.crc_bad);
}

int main(void)
{
	const char *m = getenv("RFOUNDRY_T3_RECORDINGS");
	if (m == NULL || *m == 0) {
		printf("# t3ReplayTest skipped (RFOUNDRY_T3_RECORDINGS not set: the recordings stay outside the repository, so CI never runs it)\n");
		return 0;
	}
	if (!man_load(m)) {
		printf("t3ReplayTest: the manifest needs tscc, payload and plan\n");
		return 1;
	}
	test_tier3();
	for (int i = 0; i < man.nneg; i++) {
		test_not_tier3(man.neg[i], man.neg_hz[i]);
	}
	free(recs);
	printf("t3ReplayTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
