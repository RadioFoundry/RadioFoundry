// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Tier III MS engine (core/dmr/trunk/t3_ms) on raw bursts of a control channel, as the SDR bench decoded them
// off air: each record carries the 33-byte burst and the 24-bit CACH before it. The radio's own receive chain does the
// rest: CACH TACT and short LC (core/dmr/fec/dmrCach), slot type and BPTC (core/dmr/fec/dmrFec), then the engine.
// So this checks the decode the radio does and the engine's reading of a site's broadcasts together.
//
//   RFOUNDRY_T3_SITE=<recording.jsonl> make -C tests/host run-t3site
//
// Without RFOUNDRY_T3_SITE the test runs on data/t3_site_tscc.jsonl. A record is one JSON object per line:
//   {"t": <seconds>, "raw_hex": "<33 octets hex>", "cach": "<3 octets hex>", ...}
// The site's facts (system code, color code, control channel) come from the environment, with the defaults of the
// recording in data/: RFOUNDRY_T3_SITE_SYS="<model> <net> <site>", RFOUNDRY_T3_SITE_CC, RFOUNDRY_T3_SITE_HZ. The site must
// broadcast Chan_Freq (C_BCAST MBC) for the radio to learn a channel; RFOUNDRY_T3_SITE_LEARN=0 skips that check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/trunk/t3_ms.h"
#include "dmr/fec/dmrCach.h"
#include "dmr/fec/dmrFec.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// The hex string value of key in a JSON line, into out (n bytes). False if missing or short
static bool json_hex(const char *line, const char *key, uint8_t *out, size_t n)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"%s\": \"", key);
	const char *p = strstr(line, pat);
	if (p == NULL) {
		return false;
	}
	p += strlen(pat);
	for (size_t i = 0; i < n; i++) {
		int a = hexval(p[2 * i]), b = a < 0 ? -1 : hexval(p[2 * i + 1]);
		if (a < 0 || b < 0) {
			return false;
		}
		out[i] = (uint8_t)(a << 4 | b);
	}
	return true;
}

static bool json_num(const char *line, const char *key, double *out)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"%s\": ", key);
	const char *p = strstr(line, pat);
	if (p == NULL) {
		return false;
	}
	*out = strtod(p + strlen(pat), NULL);
	return true;
}

// ---- The receive chain, as the radio runs it ----

static struct {
	uint32_t rows[4];
	uint8_t n;		// rows taken of the short LC being assembled
} slc_rx;

static struct {
	uint32_t cach_ok, cach_bad, slc_ok, slc_bad, sysparms, bursts, data, bptc_bad, slot_type_bad;
	uint32_t at_busy[2], at_free[2];
	uint32_t slc_last;
} st;

static t3_ms_t ms;
static t3_cfg_t cfg;
static uint8_t ntune;
static t3_act_t tunes[16];

static void poll_all(uint32_t now)
{
	t3_act_t a;
	while (t3_ms_poll(&ms, now, &a)) {
		if (a.type == T3_ACT_TUNE && ntune < 16) {
			tunes[ntune++] = a;
		}
	}
}

// One CACH: the TACT names the slot that follows and its AT; the payload is a quarter of a short LC
static uint8_t cach_in(uint32_t now, uint32_t bits)
{
	uint8_t tact, at, tc, lcss;
	uint32_t payload;
	dmrCachDeinterleave(bits, &tact, &payload);
	if (dmrCachTactDecode(tact, &at, &tc, &lcss) < 0) {
		st.cach_bad++;
		return 0;
	}
	st.cach_ok++;
	uint8_t slot = (uint8_t)(tc + 1u);
	if (at) {
		st.at_busy[tc]++;
	} else {
		st.at_free[tc]++;
	}
	t3_ms_slot(&ms, now, slot, true, at != 0);
	if (lcss == DMR_LCSS_FIRST) {
		slc_rx.n = 0;
	} else if (slc_rx.n == 0 || lcss == DMR_LCSS_SINGLE) {
		return slot;	// a continuation with no start: wait for the next first fragment
	}
	slc_rx.rows[slc_rx.n++] = payload;
	if (lcss == DMR_LCSS_LAST || slc_rx.n == 4) {
		uint32_t slc;
		if (slc_rx.n == 4 && lcss == DMR_LCSS_LAST && dmrShortLcDecode(slc_rx.rows, &slc)) {
			st.slc_ok++;
			st.slc_last = slc;
			if ((slc >> 24) == 0x2u) {
				st.sysparms++;
			}
			t3_ms_slc(&ms, now, slc);
		} else {
			st.slc_bad++;
		}
		slc_rx.n = 0;
	}
	return slot;
}

static void burst_in(uint32_t now, uint8_t slot, const uint8_t b[DMR_FEC_BURST_BYTES])
{
	st.bursts++;
	uint8_t cc, dt;
	if (!dmrFecSlotTypeDecode(b, &cc, &dt)) {
		st.slot_type_bad++;
		return;
	}
	if (dt != T3_DT_CSBK && dt != T3_DT_MBC_HDR && dt != T3_DT_MBC_CONT) {
		return;
	}
	st.data++;
	uint8_t d[DMR_FEC_BPTC_PAYLOAD_BYTES];
	if (dmrFecBptcDecode(b, d) < 0) {
		st.bptc_bad++;
		return;
	}
	t3_ms_rx(&ms, now, slot, dt, cc, d);
}

int main(void)
{
	const char *path = getenv("RFOUNDRY_T3_SITE");
	if (path == NULL || path[0] == 0) {
		path = "data/t3_site_tscc.jsonl";
	}
	unsigned model = T3_MODEL_SMALL, net = 5, site = 1, cc = 2;
	uint32_t hz = 455012500u;	// the recording's own Chan_Freq names this pair (placeholders: 455.0125 / 450.0125)
	const char *e = getenv("RFOUNDRY_T3_SITE_SYS");
	if (e != NULL) {
		sscanf(e, "%u %u %u", &model, &net, &site);
	}
	if ((e = getenv("RFOUNDRY_T3_SITE_CC")) != NULL) {
		cc = (unsigned)atoi(e);
	}
	if ((e = getenv("RFOUNDRY_T3_SITE_HZ")) != NULL) {
		hz = (uint32_t)strtoul(e, NULL, 10);
	}
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("t3SiteTest: cannot open %s\n", path);
		return 1;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.own_id = 2345679u;
	cfg.auth.model = (uint8_t)model;
	cfg.auth.net = (uint16_t)net;
	cfg.auth.category = T3_PAR_A;
	cfg.plan.learn = true;
	cfg.ctrl[0] = (t3_chan_t){ .lcn = 0, .rx_hz = hz, .tx_hz = hz, .cc = (uint8_t)cc };
	cfg.nctrl = 1;
	cfg.tg[0] = (t3_tg_t){ 1, T3_TG_LISTEN | T3_TG_TALK };
	cfg.ntg = 1;
	cfg.opts = T3_OPT_LISTEN_ONLY | T3_OPT_PRIVATE;
	cfg.seed = 1;

	static char line[4096];
	bool started = false;
	double t0 = 0;
	uint32_t now = 0, camped_ms = 0, records = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		double t;
		uint8_t raw[DMR_FEC_BURST_BYTES], c3[3];
		if (!json_num(line, "t", &t) || !json_hex(line, "raw_hex", raw, sizeof(raw)) ||
		    !json_hex(line, "cach", c3, sizeof(c3))) {
			continue;
		}
		if (!started) {
			t0 = t;
			started = true;
			t3_ms_start(&ms, &cfg, 1000);
			poll_all(1000);
		}
		records++;
		now = 1000u + (uint32_t)((t - t0) * 1000.0 + 0.5);
		uint32_t bits = (uint32_t)c3[0] << 16 | (uint32_t)c3[1] << 8 | c3[2];
		uint8_t slot = cach_in(now, bits);
		if (slot != 0) {
			burst_in(now, slot, raw);
		}
		poll_all(now);
		if (camped_ms == 0 && ms.state == T3_CAMPED) {
			camped_ms = now - 1000u;
		}
	}
	fclose(f);

	printf("t3SiteTest: %s: %u records, CACH %u ok / %u bad, short LC %u ok / %u bad (%u C_SYS_Parms, last 0x%07x)\n",
	       path, (unsigned)records, (unsigned)st.cach_ok, (unsigned)st.cach_bad, (unsigned)st.slc_ok,
	       (unsigned)st.slc_bad, (unsigned)st.sysparms, (unsigned)st.slc_last);
	printf("  AT busy/free: TC1 %u/%u, TC2 %u/%u; data bursts %u (slot type bad %u, BPTC bad %u)\n",
	       (unsigned)st.at_busy[0], (unsigned)st.at_free[0], (unsigned)st.at_busy[1], (unsigned)st.at_free[1],
	       (unsigned)st.data, (unsigned)st.slot_type_bad, (unsigned)st.bptc_bad);
	printf("  engine: state %s, reg %s, camped after %u ms, syscode 0x%04x, csc %s %u, tscc slots %u\n",
	       t3_state_name(ms.state), t3_reg_name(ms.reg), (unsigned)camped_ms, ms.syscode, ms.csc_valid ? "yes" : "no",
	       ms.csc, ms.tscc_slots);
	printf("  aloha: version %u nrand_wait %u backoff %u mask %u reg %u; csbk %u crc_bad %u foreign %u/%u learned %u "
	       "(entries %u), tunes %u\n",
	       ms.aloha.version, ms.aloha.nrand_wait, ms.aloha.backoff, ms.aloha.mask, ms.aloha.reg,
	       (unsigned)ms.st.csbk, (unsigned)ms.st.crc_bad, (unsigned)ms.st.foreign, (unsigned)ms.st.foreign_slc,
	       (unsigned)ms.st.learned, ms.learned.n, ntune);
	for (uint8_t i = 0; i < ms.learned.n; i++) {
		const t3_chan_t *c = &ms.learned.e[i];
		printf("  learned LCN %u: MS rx %u, MS tx %u, cc %u\n", c->lcn, (unsigned)c->rx_hz, (unsigned)c->tx_hz,
		       c->cc);
	}

	t3_syscode_t want = { (uint8_t)model, (uint16_t)net, (uint16_t)site, T3_PAR_AB };
	CHECK(records > 0, "records read");
	CHECK(ms.state == T3_CAMPED, "camped (state %s)", t3_state_name(ms.state));
	CHECK(camped_ms <= 1000u, "camped within 1 s (%u ms)", (unsigned)camped_ms);
	CHECK(ms.syscode == t3_syscode_join(&want), "system code 0x%04x, want 0x%04x", ms.syscode,
	      t3_syscode_join(&want));
	CHECK(st.cach_bad == 0, "every CACH TACT decoded");
	CHECK(st.slc_ok > 0 && st.sysparms > 0, "C_SYS_Parms short LCs decoded");
	CHECK(ms.csc_valid, "Common_Slot_Counter taken from C_SYS_Parms");
	CHECK(ms.st.crc_bad == 0, "no CSBK / MBC with a bad CRC (%u)", (unsigned)ms.st.crc_bad);
	CHECK(ms.st.foreign == 0 && ms.st.foreign_slc == 0, "nothing from another system");
	CHECK(ms.reg == T3_REG_LISTEN || ms.reg == T3_REG_NOT_NEEDED, "listen only: reg %s", t3_reg_name(ms.reg));
	e = getenv("RFOUNDRY_T3_SITE_LEARN");
	if (e == NULL || atoi(e) != 0) {
		CHECK(ms.learned.n > 0, "a channel learned from Chan_Freq");
	}
	printf("t3SiteTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
