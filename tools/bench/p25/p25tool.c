// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 Phase 1 bench tool on the firmware's own code: the units, FEC and call layer of core/p25, the framer of
// core/air and the soft modem's receiver (core/air/air_softmodem.c), built for the host. tools/bench/p25tool.py is
// its front end (options, JSON comparisons, the self-test); this program writes JSON lines on stdout.
//
//   p25tool gen    OUT.dibits EXPECT.json [options]	a conventional voice transmission as the transmit path builds it
//   p25tool decode IN.dibits [options]			symbols through the framer and the call layer
//   p25tool disc   IN.raw [options]			discriminator samples through the soft modem, then as decode
//   p25tool mod    IN.dibits OUT.raw [options]		a C4FM model: symbols to discriminator samples (for tests)
//   p25tool cc     OUT.dibits EXPECT.json [options]	a trunked system's control channel: signaling blocks (TSDU)
//
// decode and disc --trunk also take trunking signaling and multi-block trunking packets, and print their blocks.
// Dibit files: four symbols to a byte, the first in bits 7-6; 01 = +3, 00 = +1, 10 = -1, 11 = -3 (hal/hal_air.h).
// --unpacked: one symbol a byte, in its low two bits.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "p25/p25.h"
#include "p25/p25_tsbk.h"

#define SYM_RATE	4800

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "p25tool: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	exit(2);
}

// ── Small helpers ──
static uint64_t rng_s = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return rng_s;
}

static void rng_seed(uint64_t s)
{
	rng_s = s ? s * 0x9E3779B97F4A7C15ull : 0x9E3779B97F4A7C15ull;
	for (int i = 0; i < 8; i++)
		rng();
}

static double rng_unit(void)
{
	return (double)(rng() >> 11) / 9007199254740992.0;	// [0, 1)
}

static double gauss(void)
{
	double u = 1.0 - rng_unit(), v = rng_unit();

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void parse_hex(const char *s, uint8_t *out, size_t n, const char *what)
{
	if (strlen(s) != 2 * n)
		die("wrong length of hex", what);
	for (size_t i = 0; i < n; i++) {
		int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);

		if (a < 0 || b < 0)
			die("not hex", what);
		out[i] = (uint8_t)(a << 4 | b);
	}
}

static void put_hex(FILE *f, const uint8_t *p, size_t n)
{
	fputc('"', f);
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%02x", p[i]);
	fputc('"', f);
}

static const char *duid_name(uint8_t duid)
{
	switch (duid) {
	case P25_DUID_HDU: return "HDU";
	case P25_DUID_TDU: return "TDU";
	case P25_DUID_LDU1: return "LDU1";
	case P25_DUID_LDU2: return "LDU2";
	case P25_DUID_PDU: return "PDU";
	case P25_DUID_TSDU: return "TSDU";
	case P25_DUID_TDULC: return "TDULC";
	default: return "?";
	}
}

// A whole file in memory
static uint8_t *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p = NULL;
	size_t cap = 0, len = 0;

	if (!f)
		die("can't open", path);
	for (;;) {
		if (len == cap) {
			cap = cap ? 2 * cap : 1 << 16;
			p = realloc(p, cap);
			if (!p)
				die("out of memory", NULL);
		}
		size_t r = fread(p + len, 1, cap - len, f);

		if (!r)
			break;
		len += r;
	}
	fclose(f);
	*n = len;
	return p;
}

// Symbols of a dibit file, one a byte
static uint8_t *read_syms(const char *path, int unpacked, size_t *nsyms)
{
	size_t n;
	uint8_t *raw = slurp(path, &n), *s;

	if (unpacked) {
		for (size_t i = 0; i < n; i++)
			raw[i] &= 3;
		*nsyms = n;
		return raw;
	}
	s = malloc(4 * n + 1);
	if (!s)
		die("out of memory", NULL);
	for (size_t i = 0; i < 4 * n; i++)
		s[i] = (uint8_t)((raw[i / 4] >> (6 - 2 * (i & 3))) & 3);
	free(raw);
	*nsyms = 4 * n;
	return s;
}

static void write_syms(const char *path, const uint8_t *s, size_t n, int unpacked, uint8_t idle)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		die("can't write", path);
	if (unpacked) {
		fwrite(s, 1, n, f);
	} else {
		// The last byte filled up with the profile's idle symbols
		for (size_t i = 0; i < n; i += 4) {
			uint8_t b = 0;

			for (size_t k = 0; k < 4; k++)
				b = (uint8_t)(b << 2 | (i + k < n ? s[i + k] : (idle >> (6 - 2 * k)) & 3));
			fputc(b, f);
		}
	}
	fclose(f);
}

// Options: --name value pairs after the positional arguments
typedef struct {
	int argc;
	char **argv;
} opts_t;

static const char *opt(const opts_t *o, const char *name)
{
	for (int i = 0; i < o->argc; i++)
		if (!strcmp(o->argv[i], name))
			return i + 1 < o->argc ? o->argv[i + 1] : "";
	return NULL;
}

static int flag(const opts_t *o, const char *name)
{
	for (int i = 0; i < o->argc; i++)
		if (!strcmp(o->argv[i], name))
			return 1;
	return 0;
}

static long opt_long(const opts_t *o, const char *name, long dflt, int base)
{
	const char *v = opt(o, name);
	char *end;
	long r;

	if (!v)
		return dflt;
	r = strtol(v, &end, base);
	if (!*v || *end)
		die("bad number for", name);
	return r;
}

static double opt_double(const opts_t *o, const char *name, double dflt)
{
	const char *v = opt(o, name);
	char *end;
	double r;

	if (!v)
		return dflt;
	r = strtod(v, &end);
	if (!*v || *end)
		die("bad number for", name);
	return r;
}

// ── gen: the transmit path of p25_call.c, unit by unit ──
#define GEN_MAX_SYMS	(1u << 22)

static void lc_json(FILE *f, const p25_lc_t *lc)
{
	fprintf(f, "{\"lcf\":%u,\"mfid\":%u", lc->lcf, lc->mfid);
	if (lc->lcf == P25_LCF_GROUP)
		fprintf(f, ",\"tgid\":%u,\"emergency\":%s", lc->tgid, lc->emergency ? "true" : "false");
	else if (lc->lcf == P25_LCF_UNIT)
		fprintf(f, ",\"dst\":%u", lc->dst);
	fprintf(f, ",\"src\":%u}", lc->src);
}

static int cmd_gen(const char *out_path, const char *exp_path, const opts_t *o)
{
	static p25_call_tx_t t;
	static uint8_t frame[P25_IMBE_BYTES], fixed[P25_IMBE_BYTES] = {
		0xA5, 0xC3, 0xF0, 0x0F, 0x5A, 0x3C, 0x96, 0x69, 0x0F, 0x1E, 0x2D };
	uint16_t nac = (uint16_t)opt_long(o, "--nac", P25_NAC_DEFAULT, 16);
	long pairs = opt_long(o, "--pairs", 3, 10), lead = opt_long(o, "--lead", 0, 10), tail = opt_long(o, "--tail", 0, 10);
	long seed_voice = opt_long(o, "--random-voice", -1, 10);
	double ber = opt_double(o, "--ber", 0);
	const char *ss_name = opt(o, "--ss") ? opt(o, "--ss") : "unknown";
	uint8_t ss = P25_SS_UNKNOWN, idle = p25_air_mode.profile.idle;
	p25_lc_t lc;
	uint8_t *syms = malloc(GEN_MAX_SYMS);
	size_t n = 0;
	long frames_left, frames_total, nflip = 0;
	FILE *e = fopen(exp_path, "w"), *vf = NULL;
	// Voice frames go in ahead of their LDU's head (the transmit path asks for them early): they wait here and go
	// in the expect file with the LDU that carries them, P25_VOICE_FRAMES each
	static uint8_t vq[4 * P25_VOICE_FRAMES][P25_IMBE_BYTES];
	unsigned vq_head = 0, vq_n = 0;
	int first_unit = 1, ended = 0;
	uint8_t unit_duid = 0xFF;
	size_t unit_at = 0;

	if (!syms || !e)
		die("can't write", exp_path);
	if (nac > 0xFFF || pairs < 0 || pairs > 1000 || lead < 0 || tail < 0 || ber < 0 || ber > 0.5)
		die("an option is out of range", NULL);
	if (!strcmp(ss_name, "unknown"))
		ss = P25_SS_UNKNOWN;
	else if (!strcmp(ss_name, "talkaround"))
		ss = P25_SS_TALKAROUND;
	else if (!strcmp(ss_name, "busy"))
		ss = P25_SS_BUSY;
	else if (!strcmp(ss_name, "idle"))
		ss = P25_SS_IDLE;
	else
		die("--ss: unknown, talkaround, busy or idle", ss_name);
	memset(&lc, 0, sizeof lc);
	lc.src = (uint32_t)opt_long(o, "--src", 1, 10);
	if (opt(o, "--dst")) {
		lc.lcf = P25_LCF_UNIT;
		lc.dst = (uint32_t)opt_long(o, "--dst", 0, 10);
	} else {
		lc.lcf = P25_LCF_GROUP;
		lc.tgid = (uint16_t)opt_long(o, "--tg", 1, 10);
		lc.emergency = flag(o, "--emergency");
	}
	if (opt(o, "--voice"))
		parse_hex(opt(o, "--voice"), fixed, sizeof fixed, "--voice");
	if (seed_voice >= 0)
		rng_seed((uint64_t)seed_voice);
	// --voice-file: full-rate frames, 11 bytes each, in order (tests/data/voice); the last one repeats
	if (opt(o, "--voice-file")) {
		vf = fopen(opt(o, "--voice-file"), "rb");
		if (!vf)
			die("can't read", opt(o, "--voice-file"));
	}

	fprintf(e, "{\"format\":\"p25-expect/1\",\"nac\":%u,\"ss\":%u,\"lc\":", nac, ss);
	lc_json(e, &lc);
	fprintf(e, ",\"pairs\":%ld,\"lead_syms\":%ld,\"units\":[", pairs, lead);

	for (long i = 0; i < lead; i++)
		syms[n++] = (uint8_t)((idle >> (6 - 2 * (i & 3))) & 3);
	p25_call_tx_begin(&t, nac, &lc, ss);
	frames_total = frames_left = pairs * 2 * P25_VOICE_FRAMES;
	while (!p25_call_tx_done(&t)) {
		uint16_t first, k;

		// The last frame given, the end comes at once, as a talker letting go of the key: otherwise the next LDU's
		// head (released ahead of its first voice frame) would go out and the end would fill it with silence
		if (frames_left == 0 && !ended) {
			p25_call_tx_end(&t, fixed);
			ended = 1;
		}
		k = p25_call_tx_ready(&t, &first);
		if (k) {
			if (t.sent == 0) {
				// A unit starts: its data unit ID from its own NID
				uint16_t unac;

				p25_nid_get(t.unit, &unac, &unit_duid);
				unit_at = n;
				fprintf(e, "%s{\"duid\":%u,\"name\":\"%s\",\"at\":%zu,\"nsyms\":%u", first_unit ? "" : ",",
					unit_duid, duid_name(unit_duid), unit_at, t.nsyms);
				first_unit = 0;
			}
			if (n + k > GEN_MAX_SYMS)
				die("too long", NULL);
			for (uint16_t i = 0; i < k; i++)
				syms[n++] = air_sym_get(t.unit, (uint16_t)(first + i));
			if (t.sent + k == t.nsyms) {
				// The unit is whole: its symbols for the checker
				if (unit_duid == P25_DUID_LDU1 || unit_duid == P25_DUID_LDU2) {
					fputs(",\"voice\":[", e);
					for (unsigned v = 0; v < P25_VOICE_FRAMES && vq_n > 0; v++, vq_n--) {
						fprintf(e, "%s", v ? "," : "");
						put_hex(e, vq[vq_head], P25_IMBE_BYTES);
						vq_head = (vq_head + 1) % (sizeof vq / sizeof vq[0]);
					}
					fputc(']', e);
				}
				fputs(",\"syms\":", e);
				put_hex(e, t.unit, (size_t)(t.nsyms + 3) / 4);
				fputc('}', e);
			}
			p25_call_tx_sent(&t, k);
			continue;
		}
		if (p25_call_tx_wants_voice(&t) && frames_left > 0) {
			if (seed_voice >= 0)
				for (int i = 0; i < P25_IMBE_BYTES; i++)
					frame[i] = (uint8_t)rng();
			else if (vf && fread(frame, 1, sizeof frame, vf) == sizeof frame)
				memcpy(fixed, frame, sizeof fixed);
			else
				memcpy(frame, fixed, sizeof frame);
			if (vq_n == sizeof vq / sizeof vq[0])
				die("voice frames queued past their LDUs", NULL);
			memcpy(vq[(vq_head + vq_n++) % (sizeof vq / sizeof vq[0])], frame, sizeof frame);
			p25_call_tx_voice(&t, frame);
			frames_left--;
		} else if (!ended) {
			p25_call_tx_end(&t, fixed);
			ended = 1;
		} else {
			die("the transmit path stalled", NULL);
		}
	}
	for (long i = 0; i < tail; i++)
		syms[n++] = (uint8_t)((idle >> (6 - 2 * (i & 3))) & 3);
	// Bit errors on the air, after the checker's copy
	if (ber > 0) {
		rng_seed((uint64_t)opt_long(o, "--seed", 1, 10) + 0x5EED);
		for (size_t i = 0; i < n; i++)
			for (int b = 0; b < 2; b++)
				if (rng_unit() < ber) {
					syms[i] ^= (uint8_t)(1 << b);
					nflip++;
				}
	}
	fprintf(e, "],\"frames\":%ld,\"total_syms\":%zu,\"ber\":%g,\"bit_errors_applied\":%ld}\n", frames_total, n, ber,
		nflip);
	fclose(e);
	if (vf)
		fclose(vf);
	write_syms(out_path, syms, n, flag(o, "--unpacked"), idle);
	printf("{\"ev\":\"gen\",\"syms\":%zu,\"frames\":%ld,\"bit_errors_applied\":%ld}\n", n, frames_total, nflip);
	free(syms);
	return 0;
}

// ── cc: a control channel. Its broadcasts in turn (network and RFSS status, the channel identifier, an adjacent
// site, a secondary control channel), one to three blocks a unit, units back to back. Answers to one unit go ahead of
// the broadcasts in the units their --X-at names: a registration response (--reg), an affiliation response (--aff),
// a group voice grant (--grant), a deny (--deny) or queued (--queue) response. For the bench and the tests: a radio
// sends none of this ──
static void tsbk_json(FILE *f, const uint8_t blk[P25_TSBK_BYTES]);
static void isp_json(FILE *f, const uint8_t blk[P25_TSBK_BYTES]);

// An answer's units: --X-at FIRST[:COUNT[:EVERY]] (COUNT 0 or left out: to the end; EVERY: each EVERYth unit from
// FIRST, so answers given all through a file leave its units room for the broadcasts)
typedef struct {
	bool on;
	long first, count, every;
	p25_tsbk_t m;
} cc_answer_t;

static void cc_at(const opts_t *o, const char *name, long dflt, cc_answer_t *a)
{
	const char *v = opt(o, name);

	a->first = dflt;
	a->count = 0;
	a->every = 1;
	if (v && sscanf(v, "%ld:%ld:%ld", &a->first, &a->count, &a->every) < 1)
		die("FIRST[:COUNT[:EVERY]] units for", name);
	if (a->first < 0 || a->count < 0 || a->every < 1)
		die("an option is out of range", name);
}

static bool cc_answer_in(const cc_answer_t *a, long u)
{
	return a->on && u >= a->first && (a->count == 0 || u < a->first + a->count) && (u - a->first) % a->every == 0;
}

static int cmd_cc(const char *out_path, const char *exp_path, const opts_t *o)
{
	static uint8_t unit[P25_UNIT_BYTES_MAX];
	uint16_t nac = (uint16_t)opt_long(o, "--nac", P25_NAC_DEFAULT, 16);
	uint32_t wacn = (uint32_t)opt_long(o, "--wacn", 0xA4C21, 16);
	uint16_t sysid = (uint16_t)opt_long(o, "--sysid", 0x2F3, 16);
	uint8_t rfss = (uint8_t)opt_long(o, "--rfss", 1, 10), site = (uint8_t)opt_long(o, "--site", 1, 10);
	long units = opt_long(o, "--units", 40, 10), grant_at = 10;
	long base = opt_long(o, "--base", 851006250, 10), spacing = opt_long(o, "--spacing", 6250, 10);
	uint16_t ch = (uint16_t)opt_long(o, "--ch", 0x1004, 16);
	long g_tg = -1, g_ch = 0, g_src = 1, g_svc = 0, tx_off = opt_long(o, "--tx-offset", 0, 10);
	long r_unit, r_rv = P25_RV_ACCEPT, a_unit, a_tg, a_gav = P25_RV_ACCEPT, d_unit, d_svc, d_reason;
	cc_answer_t ans[5];
	uint8_t *syms;
	size_t n = 0;
	FILE *e = fopen(exp_path, "w");

	memset(ans, 0, sizeof ans);
	if (opt(o, "--grant") && sscanf(opt(o, "--grant"), "%ld:%lx:%ld:%li", &g_tg, &g_ch, &g_src, &g_svc) < 2)
		die("--grant TG:CH[:SRC[:SVC]] (CH in hex)", opt(o, "--grant"));
	if (!e || nac > 0xFFF || wacn > 0xFFFFF || sysid > 0xFFF || units < 1 || units > 100000 || base <= 0 ||
	    base / 5 > 0xFFFFFFFFl || spacing <= 0 || spacing % 125 || spacing / 125 > 1023 || tx_off % 250000 ||
	    tx_off / 250000 > 8191 || tx_off / 250000 < -8191)
		die("an option is out of range", NULL);
	if (g_tg >= 0) {
		ans[0].on = true;
		cc_at(o, "--grant-at", grant_at, &ans[0]);
		ans[0].m.kind = P25_TSBK_GRANT;
		ans[0].m.opcode = P25_OP_GRP_V_GRANT;
		ans[0].m.u.grant.svc = (uint8_t)g_svc;
		ans[0].m.u.grant.ch = (uint16_t)g_ch;
		ans[0].m.u.grant.tg = (uint16_t)g_tg;
		ans[0].m.u.grant.src = (uint32_t)g_src;
	}
	if (opt(o, "--reg")) {
		if (sscanf(opt(o, "--reg"), "%ld:%ld", &r_unit, &r_rv) < 1 || r_rv < 0 || r_rv > 3)
			die("--reg UNIT[:RV] (RV 0 accept, 1 fail, 2 deny, 3 refused)", opt(o, "--reg"));
		ans[1].on = true;
		cc_at(o, "--reg-at", 5, &ans[1]);
		ans[1].m.kind = P25_TSBK_REG_RSP;
		ans[1].m.u.reg.rv = (uint8_t)r_rv;
		ans[1].m.u.reg.sysid = sysid;
		ans[1].m.u.reg.id = (uint32_t)r_unit;
		ans[1].m.u.reg.addr = (uint32_t)r_unit;
	}
	if (opt(o, "--aff")) {
		if (sscanf(opt(o, "--aff"), "%ld:%ld:%ld", &a_unit, &a_tg, &a_gav) < 2 || a_gav < 0 || a_gav > 3)
			die("--aff UNIT:TG[:GAV] (GAV 0 accept, 1 fail, 2 deny, 3 refused)", opt(o, "--aff"));
		ans[2].on = true;
		cc_at(o, "--aff-at", 8, &ans[2]);
		ans[2].m.kind = P25_TSBK_AFF_RSP;
		ans[2].m.u.aff.gav = (uint8_t)a_gav;
		ans[2].m.u.aff.ann = (uint16_t)a_tg;
		ans[2].m.u.aff.tg = (uint16_t)a_tg;
		ans[2].m.u.aff.dst = (uint32_t)a_unit;
	}
	for (int q = 0; q < 2; q++) {
		const char *name = q ? "--queue" : "--deny";

		if (!opt(o, name))
			continue;
		d_svc = P25_ISP_GRP_V_REQ;
		d_reason = q ? 0x2F : 0x10;
		if (sscanf(opt(o, name), "%ld:%li:%li", &d_unit, &d_reason, &d_svc) < 1)
			die("UNIT[:REASON[:SVC]] for", name);
		ans[3 + q].on = true;
		cc_at(o, q ? "--queue-at" : "--deny-at", 12, &ans[3 + q]);
		ans[3 + q].m.kind = q ? P25_TSBK_QUEUED : P25_TSBK_DENY;
		ans[3 + q].m.u.rsp.svc = (uint8_t)d_svc;
		ans[3 + q].m.u.rsp.reason = (uint8_t)d_reason;
		ans[3 + q].m.u.rsp.dst = (uint32_t)d_unit;
	}
	syms = malloc((size_t)units * p25_blocks_syms(3));
	if (!syms)
		die("out of memory", NULL);
	fprintf(e, "{\"format\":\"p25-cc-expect/1\",\"nac\":%u,\"wacn\":%u,\"sysid\":%u,\"units\":[", nac, wacn, sysid);
	for (long u = 0, rot = 0; u < units; u++) {
		uint8_t b[3][P25_TSBK_BYTES];
		p25_tsbk_t m;
		unsigned k = 0, nb = 1 + (unsigned)(u % 3);

		// Answers first; the broadcasts fill the unit to its one to three blocks
		for (int q = 0; q < 5; q++) {
			if (!cc_answer_in(&ans[q], u))
				continue;
			if (k == 3)
				die("more than three answers in one unit", NULL);
			p25_tsbk_make(&ans[q].m, b[k++]);
		}
		if (nb < k)
			nb = k;
		while (k < nb) {
			uint8_t which = (uint8_t)(rot++ % 5);

			memset(&m, 0, sizeof m);
			switch (which) {
			case 0:
				m.kind = P25_TSBK_NET;
				m.u.net.lra = 1;
				m.u.net.wacn = wacn;
				m.u.net.sysid = sysid;
				m.u.net.ch = ch;
				m.u.net.ssc = 0x70;
				break;
			case 1:
			case 3:
				m.kind = which == 1 ? P25_TSBK_RFSS : P25_TSBK_ADJ;
				m.u.site.lra = 1;
				m.u.site.sysid = sysid;
				m.u.site.rfss = rfss;
				m.u.site.site = (uint8_t)(m.kind == P25_TSBK_RFSS ? site : site + 1);
				m.u.site.ch = (uint16_t)(m.kind == P25_TSBK_RFSS ? ch : ch + 2);
				m.u.site.ssc = 0x70;
				break;
			case 2:
				m.kind = P25_TSBK_IDEN;
				m.u.iden.id = (uint8_t)(ch >> 12);
				m.u.iden.form = P25_OP_IDEN_UP;
				m.u.iden.base = (uint32_t)(base / 5);
				m.u.iden.spacing = (uint16_t)(spacing / 125);
				m.u.iden.bw_khz8 = 100;
				m.u.iden.tx_off = (int16_t)(tx_off / 250000);
				break;
			default:
				m.kind = P25_TSBK_SCCB;
				m.u.sccb.rfss = rfss;
				m.u.sccb.site = site;
				m.u.sccb.ch[0] = (uint16_t)(ch + 1);
				break;
			}
			p25_tsbk_make(&m, b[k++]);
		}
		p25_unit_begin(unit, p25_blocks_syms((uint8_t)nb), nac, P25_DUID_TSDU, P25_SS_IDLE);
		fprintf(e, "%s{\"at\":%zu,\"nsyms\":%u,\"blocks\":[", u ? "," : "", n, p25_blocks_syms((uint8_t)nb));
		for (k = 0; k < nb; k++) {
			b[k][0] = (uint8_t)((b[k][0] & 0x7F) | (k == nb - 1 ? P25_TSBK_LB : 0));
			p25_tsbk_seal(b[k]);
			p25_block_put(unit, (uint8_t)k, b[k]);
			if (k)
				fputc(',', e);
			tsbk_json(e, b[k]);
		}
		fputs("]}", e);
		for (uint16_t i = 0; i < p25_blocks_syms((uint8_t)nb); i++)
			syms[n++] = air_sym_get(unit, i);
	}
	fprintf(e, "],\"total_syms\":%zu}\n", n);
	fclose(e);
	write_syms(out_path, syms, n, flag(o, "--unpacked"), p25_air_mode.profile.idle);
	printf("{\"ev\":\"cc\",\"syms\":%zu,\"units\":%ld}\n", n, units);
	free(syms);
	return 0;
}

// ── inbound: the bursts a radio sends on the control channel's inbound frequency, as p25_follow builds them (a
// registration, an affiliation and a group voice request, idle symbols between), for checking the decode side ──
static int cmd_inbound(const char *out_path, const char *exp_path, const opts_t *o)
{
	static uint8_t unit[P25_UNIT_BYTES_MAX];
	uint16_t nac = (uint16_t)opt_long(o, "--nac", P25_NAC_DEFAULT, 16);
	long gap = opt_long(o, "--gap", 480, 10), reps = opt_long(o, "--reps", 1, 10);
	uint8_t idle = p25_air_mode.profile.idle, *syms;
	p25_isp_t m[3];
	size_t n = 0;
	FILE *e = fopen(exp_path, "w");

	if (!e || nac > 0xFFF || gap < 0 || gap > 48000 || reps < 1 || reps > 1000)
		die("an option is out of range", NULL);
	memset(m, 0, sizeof m);
	m[0].opcode = P25_ISP_U_REG_REQ;
	m[1].opcode = P25_ISP_GRP_AFF_REQ;
	m[2].opcode = P25_ISP_GRP_V_REQ;
	for (int i = 0; i < 3; i++) {
		m[i].wacn = (uint32_t)opt_long(o, "--wacn", 0xA4C21, 16);
		m[i].sysid = (uint16_t)opt_long(o, "--sysid", 0x2F3, 16);
		m[i].tg = (uint16_t)opt_long(o, "--tg", 1, 10);
		m[i].src = (uint32_t)opt_long(o, "--src", 1, 10);
	}
	m[2].svc = (uint8_t)opt_long(o, "--svc", 0, 0);
	syms = malloc((size_t)reps * 3 * (size_t)(gap + P25_UNIT_SYMS_MAX) + (size_t)gap);
	if (!syms)
		die("out of memory", NULL);
	for (long i = 0; i < gap; i++)
		syms[n++] = (uint8_t)((idle >> (6 - 2 * (i & 3))) & 3);
	fprintf(e, "{\"format\":\"p25-cc-expect/1\",\"inbound\":true,\"nac\":%u,\"units\":[", nac);
	for (long r = 0; r < reps; r++)
		for (int i = 0; i < 3; i++) {
			uint8_t blk[P25_TSBK_BYTES];
			uint16_t ns;

			p25_isp_make(&m[i], blk);
			ns = p25_isp_unit(unit, nac, blk);
			fprintf(e, "%s{\"at\":%zu,\"nsyms\":%u,\"blocks\":[", r || i ? "," : "", n, ns);
			isp_json(e, blk);
			fputs("]}", e);
			for (uint16_t k = 0; k < ns; k++)
				syms[n++] = air_sym_get(unit, k);
			for (long k = 0; k < gap; k++)
				syms[n++] = (uint8_t)((idle >> (6 - 2 * (k & 3))) & 3);
		}
	fprintf(e, "],\"total_syms\":%zu}\n", n);
	fclose(e);
	write_syms(out_path, syms, n, flag(o, "--unpacked"), idle);
	printf("{\"ev\":\"inbound\",\"syms\":%zu,\"units\":%ld}\n", n, reps * 3);
	free(syms);
	return 0;
}

// ── decode and disc: units out through the framer and the call layer ──
static p25_call_rx_t g_call;
static air_framer_t g_fr;
static air_mode_t g_mode;
static void (*g_call_unit)(const p25_rx_unit_t *u, void *ctx);
static const int8_t *g_frame_soft;
static uint32_t g_units, g_voice_frames;
static int g_disc;			// times are a capture's: no symbol positions

static void frame_hook(const air_frame_t *f, void *ctx)
{
	g_frame_soft = f->soft;
	p25_air_mode.rx_frame(f, ctx);
	g_frame_soft = NULL;
}

static const char *const KIND[] = { "other", "mfr", "grant", "iden", "net", "rfss", "adj", "sccb", "sys_srv", "reg_rsp",
				     "aff_rsp", "deny", "queued" };

static void tsbk_json(FILE *f, const uint8_t blk[P25_TSBK_BYTES])
{
	p25_tsbk_t m;
	bool known = p25_tsbk_parse(blk, &m);

	fprintf(f, "{\"raw\":");
	put_hex(f, blk, P25_TSBK_BYTES);
	fprintf(f, ",\"opcode\":%u,\"mfid\":%u,\"last\":%s,\"prot\":%s,\"kind\":\"%s\"", m.opcode, m.mfid,
		m.last ? "true" : "false", m.prot ? "true" : "false", m.prot ? "protected" : KIND[m.kind]);
	if (!known) {
		fputc('}', f);
		return;
	}
	switch (m.kind) {
	case P25_TSBK_GRANT:
		fprintf(f, ",\"unit\":%s,\"ch\":%u,\"tg\":%u,\"ch2\":%u,\"tg2\":%u,\"dst\":%u,\"src\":%u",
			m.u.grant.unit ? "true" : "false", m.u.grant.ch, m.u.grant.tg, m.u.grant.ch2, m.u.grant.tg2,
			m.u.grant.dst, m.u.grant.src);
		if (m.u.grant.has_svc)
			fprintf(f, ",\"svc\":%u", m.u.grant.svc);
		break;
	case P25_TSBK_IDEN:
		fprintf(f, ",\"id\":%u,\"base_hz\":%llu,\"spacing_hz\":%u,\"tx_off\":%d,\"bw_hz\":%u,\"tdma\":%s,"
			"\"slots\":%u", m.u.iden.id, (unsigned long long)m.u.iden.base * 5, m.u.iden.spacing * 125u,
			m.u.iden.tx_off, m.u.iden.bw_khz8 * 125u, m.u.iden.tdma ? "true" : "false", m.u.iden.slots);
		break;
	case P25_TSBK_NET:
		fprintf(f, ",\"wacn\":%u,\"sysid\":%u,\"ch\":%u,\"ssc\":%u", m.u.net.wacn, m.u.net.sysid, m.u.net.ch,
			m.u.net.ssc);
		break;
	case P25_TSBK_RFSS:
	case P25_TSBK_ADJ:
		fprintf(f, ",\"sysid\":%u,\"rfss\":%u,\"site\":%u,\"ch\":%u,\"ssc\":%u", m.u.site.sysid,
			m.u.site.rfss, m.u.site.site, m.u.site.ch, m.u.site.ssc);
		break;
	case P25_TSBK_SCCB:
		fprintf(f, ",\"rfss\":%u,\"site\":%u,\"ch\":[%u,%u]", m.u.sccb.rfss, m.u.sccb.site, m.u.sccb.ch[0],
			m.u.sccb.ch[1]);
		break;
	case P25_TSBK_REG_RSP:
		fprintf(f, ",\"rv\":%u,\"sysid\":%u,\"id\":%u,\"addr\":%u", m.u.reg.rv, m.u.reg.sysid, m.u.reg.id,
			m.u.reg.addr);
		break;
	case P25_TSBK_AFF_RSP:
		fprintf(f, ",\"global\":%s,\"gav\":%u,\"ann\":%u,\"tg\":%u,\"dst\":%u", m.u.aff.global ? "true" : "false",
			m.u.aff.gav, m.u.aff.ann, m.u.aff.tg, m.u.aff.dst);
		break;
	case P25_TSBK_DENY:
	case P25_TSBK_QUEUED:
		fprintf(f, ",\"aiv\":%s,\"svc\":%u,\"reason\":%u,\"info\":%u,\"dst\":%u", m.u.rsp.aiv ? "true" : "false",
			m.u.rsp.svc, m.u.rsp.reason, m.u.rsp.info, m.u.rsp.dst);
		break;
	default:
		break;
	}
	fputc('}', f);
}

// An inbound block (a radio's, on the control channel's inbound frequency): its own opcodes
static int g_inbound;

static void isp_json(FILE *f, const uint8_t blk[P25_TSBK_BYTES])
{
	p25_isp_t m;
	bool known = p25_isp_parse(blk, &m);

	fprintf(f, "{\"raw\":");
	put_hex(f, blk, P25_TSBK_BYTES);
	fprintf(f, ",\"inbound\":true,\"opcode\":%u,\"mfid\":%u,\"last\":%s", blk[0] & 0x3F, blk[1],
		(blk[0] & P25_TSBK_LB) ? "true" : "false");
	if (!known) {
		fputs(",\"kind\":\"other\"}", f);
		return;
	}
	switch (m.opcode) {
	case P25_ISP_GRP_V_REQ:
		fprintf(f, ",\"kind\":\"grp_v_req\",\"svc\":%u,\"tg\":%u,\"src\":%u", m.svc, m.tg, m.src);
		break;
	case P25_ISP_GRP_AFF_REQ:
		fprintf(f, ",\"kind\":\"grp_aff_req\",\"sysid\":%u,\"tg\":%u,\"src\":%u", m.sysid, m.tg, m.src);
		break;
	default:
		fprintf(f, ",\"kind\":\"u_reg_req\",\"wacn\":%u,\"sysid\":%u,\"src\":%u", m.wacn, m.sysid, m.src);
		break;
	}
	fputc('}', f);
}

// A unit of trunking blocks: each block's corrected bits (-1: its CRC failed) and its message
static void blocks_json(const p25_rx_unit_t *u)
{
	uint8_t blk[P25_TSBK_BYTES];
	uint8_t n = u->duid == P25_DUID_PDU ? 1 : u->nsyms <= p25_blocks_syms(1) ? 1 : u->nsyms <= p25_blocks_syms(2) ? 2 : 3;

	printf(",\"blocks\":[");
	for (uint8_t k = 0; k < n; k++) {
		int r = p25_block_get(u->air, k, blk);

		printf("%s{\"errs\":%d,\"msg\":", k ? "," : "", r);
		if (u->duid == P25_DUID_PDU) {
			p25_mbt_t h;

			if (r >= 0 && p25_mbt_parse(blk, &h))
				printf("{\"mbt\":true,\"format\":%u,\"sap\":%u,\"mfid\":%u,\"blocks\":%u,\"opcode\":%u}",
				       h.format, h.sap, h.mfid, h.blocks, h.opcode);
			else
				printf("{\"mbt\":false}");
		} else if (g_inbound) {
			isp_json(stdout, blk);
		} else {
			tsbk_json(stdout, blk);
		}
		printf("}");
	}
	printf("]");
}

static void unit_hook(const p25_rx_unit_t *u, void *ctx)
{
	uint8_t buf[P25_HDR_BYTES], ssn[4] = { 0 };
	uint16_t nss = u->nsyms / 36;
	int r;

	printf("{\"ev\":\"unit\",\"i\":%u,\"duid\":%u,\"name\":\"%s\",\"nac\":%u,\"nid_errs\":%d,\"t_us\":%u", g_units++,
	       u->duid, duid_name(u->duid), u->nac, u->nid_errs, u->t_us);
	if (!g_disc)
		printf(",\"at\":%u", (uint32_t)(((uint64_t)u->t_us * SYM_RATE + 500000) / 1000000));
	printf(",\"nsyms\":%u,\"no_sync\":%s", u->nsyms, (u->flags & AIR_FRAME_NO_SYNC) ? "true" : "false");
	for (uint16_t k = 0; k < nss; k++)
		ssn[p25_status_get(u->air, k)]++;
	printf(",\"ss\":[%u,%u,%u,%u]", ssn[0], ssn[1], ssn[2], ssn[3]);
	if (u->duid == P25_DUID_HDU) {
		r = p25_hdu_get(u->air, buf);
		printf(",\"hdr_rs\":%d", r);
		if (r >= 0) {
			printf(",\"hdr\":{\"mi\":");
			put_hex(stdout, buf, 9);
			printf(",\"mfid\":%u,\"algid\":%u,\"kid\":%u,\"tgid\":%u}", buf[9], buf[10], buf[11] << 8 | buf[12],
			       buf[13] << 8 | buf[14]);
		}
	} else if (u->duid == P25_DUID_LDU1 || u->duid == P25_DUID_TDULC) {
		p25_lc_t lc;

		r = u->duid == P25_DUID_LDU1 ? p25_ldu_lc_get(u->air, buf) : p25_tdulc_get(u->air, buf);
		printf(",\"lc_rs\":%d", r);
		if (r >= 0) {
			printf(",\"lc_raw\":");
			put_hex(stdout, buf, P25_LC_BYTES);
			if (p25_lc_parse(buf, &lc)) {
				printf(",\"lc\":");
				lc_json(stdout, &lc);
			}
		}
	} else if (u->duid == P25_DUID_LDU2) {
		r = p25_ldu_es_get(u->air, buf);
		printf(",\"es_rs\":%d", r);
		if (r >= 0) {
			printf(",\"es\":{\"mi\":");
			put_hex(stdout, buf, 9);
			printf(",\"algid\":%u,\"kid\":%u}", buf[9], buf[10] << 8 | buf[11]);
		}
	}
	if (u->duid == P25_DUID_TSDU || u->duid == P25_DUID_PDU)
		blocks_json(u);
	if (u->duid == P25_DUID_LDU1 || u->duid == P25_DUID_LDU2) {
		r = p25_ldu_lsd_get(u->air, buf);
		printf(",\"lsd_errs\":%d,\"lsd\":", r);
		put_hex(stdout, buf, 2);
		printf(",\"voice\":[");
		for (uint8_t v = 0; v < P25_VOICE_FRAMES; v++) {
			uint8_t imbe[P25_IMBE_BYTES];
			int errs = P25_VOICE_ERRS_TOTAL(p25_ldu_voice_get(u->air, v, imbe));

			printf("%s{\"imbe\":", v ? "," : "");
			put_hex(stdout, imbe, sizeof imbe);
			printf(",\"errs\":%d}", errs);
		}
		printf("]");
	}
	printf(",\"syms\":");
	put_hex(stdout, u->air, (size_t)(u->nsyms + 3) / 4);
	if (g_frame_soft) {
		printf(",\"soft\":");
		put_hex(stdout, (const uint8_t *)g_frame_soft, u->nsyms);
	}
	printf("}\n");
	g_call_unit(u, ctx);
}

static void call_json(const char *ev, const p25_call_t *c)
{
	printf("{\"ev\":\"%s\",\"nac\":%u,\"late\":%s,\"have_lc\":%s,\"lc\":", ev, c->nac, c->late ? "true" : "false",
	       c->have_lc ? "true" : "false");
	lc_json(stdout, &c->lc);
	printf(",\"have_alg\":%s,\"algid\":%u,\"kid\":%u}\n", c->have_alg ? "true" : "false", c->algid, c->kid);
}

static void on_start(const p25_call_t *c, void *ctx) { (void)ctx; call_json("call_start", c); }
static void on_update(const p25_call_t *c, void *ctx) { (void)ctx; call_json("call_update", c); }
static void on_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	(void)imbe;
	(void)errs;
	(void)ctx;
	g_voice_frames++;
}
static void on_end(void *ctx) { (void)ctx; printf("{\"ev\":\"call_end\"}\n"); }

static const p25_call_sink_t sink = { on_start, on_update, on_voice, on_end, NULL };

static void rx_setup(const opts_t *o)
{
	static uint8_t hard[P25_UNIT_BYTES_MAX];
	static int8_t soft[P25_UNIT_SYMS_MAX];
	uint16_t nac = (uint16_t)opt_long(o, "--nac", P25_NAC_ANY, 16);

	g_mode = p25_air_mode;
	g_mode.rx_frame = frame_hook;
	p25_call_rx_init(&g_call, nac, &sink, NULL);
	g_call_unit = g_call.rx.unit;
	g_call.rx.unit = unit_hook;
	g_inbound = flag(o, "--inbound");
	g_call.rx.trunk = flag(o, "--trunk") || g_inbound;
	air_framer_init(&g_fr, &g_mode, &g_call.rx, hard, soft, P25_UNIT_SYMS_MAX);
}

static void rx_stats(void)
{
	const air_framer_stats_t *s = &g_fr.stats;
	const p25_call_stats_t *c = &g_call.stats;

	printf("{\"ev\":\"stats\",\"framer\":{\"syncs\":%u,\"misses\":%u,\"locks\":%u,\"rejects\":%u,\"frames\":%u},"
	       "\"call\":{\"units\":%u,\"voice\":%u,\"voice_bad\":%u,\"voice_bits\":%u,\"lc_fail\":%u,\"es_fail\":%u,"
	       "\"calls\":%u},\"voice_out\":%u}\n", s->syncs, s->misses, s->locks, s->rejects, s->frames, c->units,
	       c->voice, c->voice_bad, c->voice_bits, c->lc_fail, c->es_fail, c->calls, g_voice_frames);
}

static int cmd_decode(const char *in, const opts_t *o)
{
	size_t n, nsoft = 0;
	uint8_t *s = read_syms(in, flag(o, "--unpacked"), &n);
	int8_t *soft = NULL;
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];

	if (opt(o, "--soft")) {
		soft = (int8_t *)slurp(opt(o, "--soft"), &nsoft);
		if (nsoft < n)
			die("--soft: fewer soft values than symbols", opt(o, "--soft"));
	}
	rx_setup(o);
	for (size_t at = 0; at < n; at += AIR_CHUNK_SYMS_MAX) {
		air_chunk_t c;
		uint16_t k = (uint16_t)(n - at < AIR_CHUNK_SYMS_MAX ? n - at : AIR_CHUNK_SYMS_MAX);

		memset(&c, 0, sizeof c);
		for (uint16_t i = 0; i < k; i++)
			air_sym_put(hard, i, s[at + i]);
		c.t_us = (uint32_t)(((uint64_t)at * 1000000u) / SYM_RATE);
		c.nsyms = k;
		c.sync_at = -1;
		c.flags = at ? 0 : AIR_CHUNK_GAP;
		c.hard = hard;
		c.soft = soft ? soft + at : NULL;
		air_framer_rx(&g_fr, &c);
	}
	// The end of the input is the end of the signal
	{
		air_chunk_t c;
		uint8_t z = 0;

		memset(&c, 0, sizeof c);
		c.t_us = (uint32_t)(((uint64_t)n * 1000000u) / SYM_RATE);
		c.nsyms = 1;
		c.sync_at = -1;
		c.flags = AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL;
		c.hard = &z;
		air_framer_rx(&g_fr, &c);
	}
	printf("{\"ev\":\"input\",\"syms\":%zu}\n", n);
	rx_stats();
	free(s);
	free(soft);
	return 0;
}

// The soft modem's sync findings. It keeps them in its state, not in its interface: read here for the report only
static air_softmodem_t g_m;
static uint32_t g_gain0;
static double g_cpk;			// tap counts per kHz, 0 unknown
static int g_invert;
static uint32_t g_sm_syncs, g_sm_chunks, g_sm_gaps, g_sm_free;

static void sm_out(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	g_sm_chunks++;
	if (c->flags & AIR_CHUNK_GAP)
		g_sm_gaps++;
	if (c->flags & AIR_CHUNK_FREE_RUN)
		g_sm_free++;
	if (c->sync_at >= 0) {
		const air_profile_t *p = &p25_air_mode.profile;
		// The fitted outer level in line units x 256 is 2^21 / scale, nominal 4096; the gain moved from its start
		// when a sync rescaled the line
		double rel = 512.0 / g_m.scale * (double)g_gain0 / g_m.gain;
		double off_lines = g_m.off / 256.0 * (double)g_gain0 / g_m.gain;	// line units at the starting gain
		double sign = g_invert ? -1 : 1;

		g_sm_syncs++;
		printf("{\"ev\":\"sync\",\"t_ms\":%.3f", (c->t_us + c->sync_at * 1e6 / SYM_RATE) / 1000.0);
		if (g_cpk > 0) {
			double dc_hz = g_m.dc / 1024.0 / g_cpk * 1000.0;

			// (a tap of unknown scale has no level to give: its gain followed the input)
			printf(",\"level_rel\":%.4f,\"outer_hz\":%.1f,\"offset_hz\":%.1f", rel, rel * p->dev_outer_hz,
			       sign * (dc_hz + off_lines * p->dev_outer_hz / 48.0));
		}
		printf("}\n");
	}
	air_framer_rx(&g_fr, c);
}

static int cmd_disc(const char *in, const opts_t *o)
{
	size_t nbytes, n;
	uint8_t *raw = slurp(in, &nbytes);
	long bits = opt_long(o, "--bits", 16, 10), rate = opt_long(o, "--rate", 48000, 10);
	long shift = opt_long(o, "--shift", 8, 10);
	double full = opt_double(o, "--full", 0), skip = opt_double(o, "--skip", 0);
	int16_t *x;
	uint16_t cpk16;

	g_disc = 1;
	g_invert = flag(o, "--invert");
	if ((bits != 8 && bits != 16) || shift < 0 || shift > 8)
		die("--bits: 8 or 16, --shift: 0 to 8", NULL);
	n = bits == 16 ? nbytes / 2 : nbytes;
	x = malloc((n + 1) * sizeof *x);
	if (!x)
		die("out of memory", NULL);
	for (size_t i = 0; i < n; i++) {
		// 8-bit samples are 16-bit ones shifted right by --shift: moved back, so the tap's scale holds
		int32_t v = bits == 16 ? (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) : (int8_t)raw[i] * (1 << shift);

		if (g_invert)
			v = -v;
		x[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	free(raw);
	// The tap's scale: counts per kHz of 16-bit samples (as the debug link's tap reports it), or Hz at full scale
	if (opt(o, "--counts-per-khz"))
		g_cpk = opt_double(o, "--counts-per-khz", 0);
	else if (full > 0)
		g_cpk = 32768.0 / (full / 1000.0);
	if (g_cpk > 65535)
		die("the scale is past what the soft modem takes (counts per kHz under 65536)", NULL);
	cpk16 = (uint16_t)lround(g_cpk);
	g_cpk = cpk16;
	rx_setup(o);
	if (!air_softmodem_rx_open(&g_m, &p25_air_mode.profile, (uint32_t)rate, cpk16, sm_out, NULL))
		die("the soft modem takes 8 to 48 kHz", NULL);
	g_gain0 = g_m.gain;
	{
		size_t from = (size_t)(skip * rate);

		for (size_t at = from; at < n; at += 256) {
			uint16_t k = (uint16_t)(n - at < 256 ? n - at : 256);

			air_softmodem_rx(&g_m, x + at, k, (uint32_t)(((uint64_t)(at - from) * 1000000u) / (uint64_t)rate));
		}
	}
	// The end of the capture is the end of the signal
	{
		air_chunk_t c;
		uint8_t z = 0;

		memset(&c, 0, sizeof c);
		c.nsyms = 1;
		c.sync_at = -1;
		c.flags = AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL;
		c.hard = &z;
		air_framer_rx(&g_fr, &c);
	}
	printf("{\"ev\":\"input\",\"samples\":%zu,\"rate\":%ld,\"seconds\":%.3f,\"counts_per_khz\":%.0f,\"invert\":%s}\n", n,
	       rate, (double)n / rate, g_cpk, g_invert ? "true" : "false");
	printf("{\"ev\":\"softmodem\",\"syncs\":%u,\"chunks\":%u,\"gap_chunks\":%u,\"free_run_chunks\":%u}\n", g_sm_syncs,
	       g_sm_chunks, g_sm_gaps, g_sm_free);
	rx_stats();
	free(x);
	return 0;
}

// ── mod: a C4FM transmitter and an FM discriminator, modeled at 240 kHz (as tests/host/airSoftmodemTest.c) ──
#define FS	240000
#define SPAN	8

// The C4FM pulse (TIA-102.BAAA-A 9.3: raised cosine 0.2 times the shaper (pi f T) / sin(pi f T)), DC gain 1
static void c4fm_pulse(double *h, int osr)
{
	int n = 2 * SPAN * osr + 1;
	double rate = (double)FS / osr, sum = 0, alpha = 0.2, f1 = rate * (1 - alpha) / 2, f2 = rate * (1 + alpha) / 2;

	for (int i = 0; i < n; i++) {
		double t = (double)(i - SPAN * osr) / FS, v = 0;

		for (double f = 2.5; f < f2; f += 5) {
			double hf = f < f1 ? 1 : 0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1)), xx = M_PI * f / rate;

			v += hf * xx / sin(xx) * cos(2 * M_PI * f * t);
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

static int cmd_mod(const char *in, const char *out_path, const opts_t *o)
{
	size_t n;
	uint8_t *s = read_syms(in, flag(o, "--unpacked"), &n);
	long rate = opt_long(o, "--rate", 48000, 10);
	double full = opt_double(o, "--full", 5000), dev = 1800 * opt_double(o, "--dev-scale", 100) / 100;
	double off = opt_double(o, "--offset", 0), noise = opt_double(o, "--noise", 0), ppm = opt_double(o, "--ppm", 0);
	double gap = opt_double(o, "--carrier-gap", 0.2), hi = opt_double(o, "--lpf", 0);
	int osr = FS / SYM_RATE, inv = flag(o, "--invert");
	// An unmodulated carrier before the first symbol, as many transmitters key up: the discriminator goes quiet
	size_t lead = (size_t)(gap * FS), quiet = (size_t)(opt_double(o, "--carrier-lead", 0) * FS), wn, dec;
	double *w, h[2 * SPAN * (FS / SYM_RATE) + 1], lpf[241], lsum = 0;
	FILE *f;

	if (rate < 8000 || FS % rate)
		die("--rate must divide 240000 (8000, 12000, 16000, 24000, 48000 ...)", NULL);
	dec = (size_t)(FS / rate);
	if (hi <= 0)
		hi = rate * 0.45 < 6000 ? rate * 0.45 : 6000;
	rng_seed((uint64_t)opt_long(o, "--seed", 7, 10));
	wn = lead + quiet + (size_t)((double)n * osr / (1 + ppm * 1e-6)) + lead + 2 * 120;
	w = calloc(wn, sizeof *w);
	if (!w)
		die("out of memory", NULL);
	c4fm_pulse(h, osr);
	for (size_t k = 0; k < n; k++) {
		long at = (long)(lead + quiet) + 120 + lround((double)k * osr / (1 + ppm * 1e-6)) - SPAN * osr;
		double lv = air_sym_level(s[k]) * dev / 3.0;

		for (int i = 0; i <= 2 * SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)wn)
				w[at + i] += lv * h[i];
	}
	for (int i = -120; i <= 120; i++) {
		double xx = 2 * M_PI * hi / FS * i, win = 0.54 + 0.46 * cos(M_PI * i / 120);

		lpf[i + 120] = (i ? sin(xx) / xx : 1) * win;
		lsum += lpf[i + 120];
	}
	f = fopen(out_path, "wb");
	if (!f)
		die("can't write", out_path);
	for (size_t j = 120; j + 120 < wn; j += dec) {
		double v = 0;
		int carrier = j >= lead && j < wn - lead;

		for (int i = -120; i <= 120; i++)
			v += w[j + (size_t)i] * lpf[i + 120];
		v = v / lsum + off;
		// No carrier: discriminator noise, well past the signal's deviation
		v += carrier ? noise * gauss() : 2.5 * dev * gauss();
		v = v / full * 32768;
		if (inv)
			v = -v;
		v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		int16_t q = (int16_t)lround(v);

		fputc(q & 0xFF, f);
		fputc((q >> 8) & 0xFF, f);
	}
	fclose(f);
	printf("{\"ev\":\"mod\",\"syms\":%zu,\"rate\":%ld,\"full_hz\":%.0f,\"dev_hz\":%.1f,\"lpf_hz\":%.0f}\n", n, rate, full,
	       dev, hi);
	free(w);
	free(s);
	return 0;
}

int main(int argc, char **argv)
{
	opts_t o;

	if (argc < 3) {
		fprintf(stderr, "usage: p25tool gen OUT EXPECT | cc OUT EXPECT | inbound OUT EXPECT | decode IN | disc IN | mod IN OUT [options]  "
			"(tools/bench/p25tool.py)\n");
		return 2;
	}
	if (!strcmp(argv[1], "gen") && argc >= 4) {
		o.argc = argc - 4;
		o.argv = argv + 4;
		return cmd_gen(argv[2], argv[3], &o);
	}
	if (!strcmp(argv[1], "cc") && argc >= 4) {
		o.argc = argc - 4;
		o.argv = argv + 4;
		return cmd_cc(argv[2], argv[3], &o);
	}
	if (!strcmp(argv[1], "inbound") && argc >= 4) {
		o.argc = argc - 4;
		o.argv = argv + 4;
		return cmd_inbound(argv[2], argv[3], &o);
	}
	if (!strcmp(argv[1], "mod") && argc >= 4) {
		o.argc = argc - 4;
		o.argv = argv + 4;
		return cmd_mod(argv[2], argv[3], &o);
	}
	o.argc = argc - 3;
	o.argv = argv + 3;
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argv[2], &o);
	if (!strcmp(argv[1], "disc"))
		return cmd_disc(argv[2], &o);
	die("unknown command", argv[1]);
	return 2;
}
