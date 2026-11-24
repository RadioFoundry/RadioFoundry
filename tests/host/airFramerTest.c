// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The framer (core/air/air_framer.c) on made-up symbol streams: frames found at any chunk phase, with and without
// soft values, at other deviations and tuning errors, across missed syncs, gaps, units of several lengths, units
// longer than the buffer, syncs in the middle of a frame, chance matches the head hook refuses, and noise. Then the
// transmit packer against a fake provider.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_framer.h"
#include "air/air_syms.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x1234567;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// ── A stream under construction: one dibit per byte, and what a receiver would make of it ──
#define STREAM_MAX	40000
static uint8_t st_sym[STREAM_MAX];
static uint32_t st_n;

static void st_reset(void) { st_n = 0; }
static void st_random(uint32_t n) { while (n--) st_sym[st_n++] = (uint8_t)(rng() & 3); }
static void st_packed(const uint8_t *p, uint16_t n) { for (uint16_t i = 0; i < n; i++) st_sym[st_n++] = air_sym_get(p, i); }

// What the mode saw
#define FRAMES_MAX	400
typedef struct {
	uint32_t t_us;
	uint16_t nsyms, offset;
	uint8_t flags, sync_id, sync_errs;
	uint32_t at;			// stream index of the frame's first symbol (from t_us)
	uint32_t errs;			// symbols that differ from the stream
	bool soft;
} seen_t;
static seen_t seen[FRAMES_MAX];
static uint16_t nseen, nlost, nhead;
static uint16_t head_len_at;		// the test mode's length field: symbol index, 0 = fixed length
static uint8_t head_magic;		// the two symbols after the sync must be this (4 bits), 0xFF = any
static bool head_again;
static int soft_err_max;		// largest |soft - level x 32| seen in frames

#define T0_US		1000000u
static uint16_t sym_rate = 4800;

static uint32_t at_of(uint32_t t_us) { return (uint32_t)(((uint64_t)(t_us - T0_US) * sym_rate + 500000) / 1000000); }

static void on_frame(const air_frame_t *f, void *ctx)
{
	seen_t *s = &seen[nseen];

	if (nseen >= FRAMES_MAX)
		return;
	memset(s, 0, sizeof(*s));
	s->t_us = f->t_us;
	s->nsyms = f->nsyms;
	s->offset = f->offset;
	s->flags = f->flags;
	s->sync_id = f->sync_id;
	s->sync_errs = f->sync_errs;
	s->at = at_of(f->t_us);
	s->soft = f->soft != NULL;
	for (uint16_t i = 0; i < f->nsyms; i++) {
		uint32_t k = s->at + f->offset + i;

		if (k >= st_n || air_sym_get(f->hard, i) != st_sym[k])
			s->errs++;
		else if (f->soft) {
			int e = abs(f->soft[i] - 32 * air_sym_level(st_sym[k]));

			if (e > soft_err_max)
				soft_err_max = e;
		}
	}
	nseen++;
}

static void on_lost(void *ctx) { nlost++; }

// The test mode's head: an optional magic nibble behind the sync, then an optional length in symbols (four dibits,
// x 8) at head_len_at
static uint16_t on_head(const air_frame_t *f, void *ctx)
{
	const air_mode_t *m = ctx;
	uint16_t n = 0;

	nhead++;
	CHECK(f->flags & AIR_FRAME_HEAD, "head flag");
	if (head_magic != 0xFF && ((air_sym_get(f->hard, 24) << 2) | air_sym_get(f->hard, 25)) != head_magic)
		return 0;
	if (!head_len_at)
		return m->frame_syms;
	if (head_again && f->nsyms < head_len_at + 4)
		return (uint16_t)(AIR_HEAD_AGAIN | (head_len_at + 4));
	for (uint8_t i = 0; i < 4; i++)
		n = (uint16_t)((n << 2) | air_sym_get(f->hard, (uint16_t)(head_len_at + i)));
	return (uint16_t)(n * 8);
}

static const uint8_t FS48[6] = { 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF };	// 24 outer symbols
static const uint8_t FSW20[6] = { 0xCD, 0xF5, 0x90, 0, 0, 0 };		// 10 symbols, two of them inner

static air_mode_t mode;
static air_framer_t fr;
static uint8_t fbuf[(1024 + 3) / 4];
static int8_t fsoft[1024];

static void mode_p25ish(void)
{
	memset(&mode, 0, sizeof(mode));
	mode.name = "t";
	mode.profile.sym_rate = 4800;
	mode.profile.levels = 4;
	mode.profile.nsyncs = 1;
	mode.profile.sync[0].id = 7;
	mode.profile.sync[0].nsyms = 24;
	mode.profile.sync[0].max_errs = 4;
	memcpy(mode.profile.sync[0].syms, FS48, 6);
	mode.frame_syms = 200;
	mode.lost_frames = 2;
	mode.rx_frame = on_frame;
	mode.rx_lost = on_lost;
	head_len_at = 0;
	head_magic = 0xFF;
	head_again = false;
	sym_rate = 4800;
}

static void begin(uint16_t buf_syms, bool soft)
{
	nseen = nlost = nhead = 0;
	soft_err_max = 0;
	air_framer_init(&fr, &mode, &mode, fbuf, soft ? fsoft : NULL, buf_syms);
}

// Feed stream symbols [from, to) in chunks of chunk symbols (0 = random 1..144). gain_pct / off / noise shape the
// soft values (noise: peak, in soft counts); soft false: hard dibits only
typedef struct { int gain_pct, off, noise; bool soft; uint8_t flags_first; } chan_t;

static void feed(uint32_t from, uint32_t to, uint16_t chunk, const chan_t *ch)
{
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];
	int8_t soft[AIR_CHUNK_SYMS_MAX];
	bool first = true;

	while (from < to) {
		air_chunk_t c;
		uint16_t n = chunk ? chunk : (uint16_t)(1 + rng() % AIR_CHUNK_SYMS_MAX);

		if (n > to - from)
			n = (uint16_t)(to - from);
		memset(&c, 0, sizeof(c));
		for (uint16_t i = 0; i < n; i++) {
			int lv = air_sym_level(st_sym[from + i]);
			int v = ch->off + (32 * lv * ch->gain_pct) / 100;

			if (ch->noise)
				v += (int)(rng() % (2 * ch->noise + 1)) - ch->noise;
			v = v > 127 ? 127 : v < -127 ? -127 : v;
			soft[i] = (int8_t)v;
			// The provider's own slicer: nominal levels, no offset
			air_sym_put(hard, i, ch->soft ? (v >= 64 ? 1 : v >= 0 ? 0 : v > -64 ? 2 : 3) : st_sym[from + i]);
		}
		c.t_us = T0_US + (uint32_t)(((uint64_t)from * 1000000) / sym_rate);
		c.nsyms = n;
		c.sync_at = -1;
		c.flags = first ? ch->flags_first : 0;
		c.hard = hard;
		c.soft = ch->soft ? soft : NULL;
		air_framer_rx(&fr, &c);
		from += n;
		first = false;
	}
}

static const chan_t CLEAN = { 100, 0, 0, true, 0 };
static const chan_t HARD = { 100, 0, 0, false, 0 };

// frames of len symbols, each opening with the 24-symbol sync; returns the stream index of the first
static uint32_t st_frames(uint16_t count, uint16_t len)
{
	uint32_t at = st_n;

	for (uint16_t i = 0; i < count; i++) {
		st_packed(FS48, 24);
		st_random((uint32_t)(len - 24));
	}
	return at;
}

static uint32_t total_errs(void)
{
	uint32_t e = 0;

	for (uint16_t i = 0; i < nseen; i++)
		e += seen[i].errs;
	return e;
}

static void testFixedFrames(void)
{
	static const uint16_t chunks[] = { 144, 1, 7, 143, 100, 0, 0, 0 };

	for (uint8_t soft = 0; soft < 2; soft++) {
		for (uint8_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
			uint32_t at;

			mode_p25ish();
			st_reset();
			st_random(50 + rng() % 300);
			at = st_frames(20, 200);
			st_random(100);
			begin(200, true);
			feed(0, st_n, chunks[k], soft ? &CLEAN : &HARD);
			CHECK(nseen == 20, "chunk %u soft %u: %u frames", chunks[k], soft, nseen);
			for (uint16_t i = 0; i < 20 && i < nseen; i++) {
				CHECK(seen[i].at == at + 200u * i, "frame %u at %u", i, (unsigned)seen[i].at);
				CHECK(seen[i].errs == 0 && seen[i].nsyms == 200 && seen[i].offset == 0, "frame %u content", i);
				CHECK(seen[i].flags == 0 && seen[i].sync_id == 7 && seen[i].sync_errs == 0, "frame %u flags %x", i, seen[i].flags);
				CHECK(seen[i].soft == (soft != 0), "frame %u soft", i);
			}
			CHECK(fr.stats.locks == 1 && fr.stats.syncs == 20, "locks %u syncs %u", fr.stats.locks, fr.stats.syncs);
			// The 100 symbols behind the last frame hold no sync: one miss, under lost_frames
			CHECK(nlost == 0 && air_framer_locked(&fr), "lost %u", nlost);
			if (soft)
				CHECK(soft_err_max <= 1, "soft error %d", soft_err_max);
		}
	}
}

// Other deviations, a tuning error and noise: the levels come from the sync
static void testLevels(void)
{
	static const chan_t ch[] = {
		{ 60, 0, 0, true, 0 }, { 130, 0, 0, true, 0 }, { 93, 25, 0, true, 0 }, { 93, -30, 0, true, 0 },
		{ 62, 12, 0, true, 0 }, { 93, 10, 8, true, 0 }, { 110, -15, 10, true, 0 },
	};

	for (uint8_t k = 0; k < sizeof(ch) / sizeof(ch[0]); k++) {
		uint32_t at;

		mode_p25ish();
		st_reset();
		st_random(200);
		at = st_frames(30, 200);
		begin(200, true);
		feed(0, st_n, 0, &ch[k]);
		CHECK(nseen == 30, "channel %u: %u frames", k, nseen);
		CHECK(seen[0].at == at, "channel %u first at %u", k, (unsigned)seen[0].at);
		// Noise of a quarter of the eye at most: no symbol may be wrong
		CHECK(total_errs() == 0, "channel %u: %u symbol errors", k, (unsigned)total_errs());
		CHECK(fr.stats.misses == 0, "channel %u misses %u", k, fr.stats.misses);
		// Scaled back to nominal: +3 reads 96 whatever came in (within the noise and the fit's rounding)
		CHECK(soft_err_max <= (ch[k].noise ? 3 * ch[k].noise : 6), "channel %u soft error %d", k, soft_err_max);
	}
}

// Under about 0.58 of the nominal deviation a sync is refused: a run of inner symbols looks the same
static void testLowLevel(void)
{
	static const chan_t low = { 45, 0, 0, true, 0 };

	mode_p25ish();
	st_reset();
	st_random(200);
	st_frames(10, 200);
	begin(200, true);
	feed(0, st_n, 0, &low);
	CHECK(nseen == 0 && fr.stats.locks == 0, "%u frames at 0.45 of nominal", nseen);
	// 24 inner symbols with the sync's signs in the payload of a good signal are no sync either
	st_reset();
	st_random(600);
	for (uint8_t i = 0; i < 24; i++)
		st_sym[300 + i] = (uint8_t)(air_sym_get(FS48, i) & 2);
	begin(200, true);
	feed(0, st_n, 0, &CLEAN);
	CHECK(nseen == 0 && fr.stats.locks == 0, "%u locks on an inner-symbol run", fr.stats.locks);
}

static void testMissedSyncAndLoss(void)
{
	uint32_t at;

	mode_p25ish();
	mode.lost_frames = 3;
	st_reset();
	st_random(77);
	at = st_frames(10, 200);
	// Frame 4's sync is wrecked: placed by counting. Frames 7 and 8 too: still under lost_frames
	for (uint8_t i = 0; i < 24; i++) {
		st_sym[at + 4 * 200 + i] = (uint8_t)(rng() & 3);
		st_sym[at + 7 * 200 + i] = 0;
		st_sym[at + 8 * 200 + i] = 2;
	}
	st_random(900);
	begin(200, true);
	feed(0, st_n, 0, &CLEAN);
	CHECK(nseen >= 10, "%u frames", nseen);
	for (uint16_t i = 0; i < 10 && i < nseen; i++) {
		bool nosync = i == 4 || i == 7 || i == 8;

		CHECK(seen[i].at == at + 200u * i && seen[i].errs == 0, "frame %u at %u errs %u", i, (unsigned)seen[i].at, (unsigned)seen[i].errs);
		CHECK(!(seen[i].flags & AIR_FRAME_NO_SYNC) == !nosync, "frame %u flags %x", i, seen[i].flags);
	}
	// After the 10th: two more by counting, then lost at the third missing sync
	CHECK(nseen == 12 && nlost == 1, "%u frames, %u lost", nseen, nlost);
	CHECK(!air_framer_locked(&fr), "still locked");
}

static void testGap(void)
{
	uint32_t at;
	chan_t gap = CLEAN;

	mode_p25ish();
	st_reset();
	st_random(30);
	at = st_frames(12, 200);
	begin(200, true);
	feed(0, at + 5 * 200 + 60, 144, &CLEAN);
	CHECK(nseen == 5 && nlost == 0, "before the gap: %u frames", nseen);
	// 317 symbols never arrive; the next chunk says so
	gap.flags_first = AIR_CHUNK_GAP;
	feed(at + 5 * 200 + 60 + 317, st_n, 144, &gap);
	CHECK(nlost == 1, "lost %u", nlost);
	// Frames 5 and 6 are gone (cut, and begun inside the hole); 7 on are found again at their own place
	CHECK(nseen == 5 + 5, "%u frames", nseen);
	for (uint16_t i = 5; i < nseen; i++)
		CHECK(seen[i].at == at + 200u * (i + 2) && seen[i].errs == 0, "frame %u at %u", i, (unsigned)seen[i].at);
	CHECK(fr.stats.locks == 2, "locks %u", fr.stats.locks);
}

// Units of several lengths, the length in a field behind the sync
static void testVariableUnits(void)
{
	static const uint16_t lens[] = { 864, 72, 216, 400, 864, 864, 72 };
	uint32_t at[8];

	for (uint8_t again = 0; again < 2; again++) {
		mode_p25ish();
		mode.head = on_head;
		mode.head_syms = 30;
		head_len_at = again ? 40 : 26;
		head_again = again != 0;
		st_reset();
		st_random(11);
		for (uint8_t i = 0; i < 7; i++) {
			at[i] = st_n;
			st_packed(FS48, 24);
			st_random((uint32_t)(lens[i] - 24));
			for (uint8_t j = 0; j < 4; j++)
				st_sym[at[i] + head_len_at + j] = (uint8_t)(((lens[i] / 8) >> (6 - 2 * j)) & 3);
		}
		begin(864, true);
		feed(0, st_n, 0, &CLEAN);
		CHECK(nseen == 7, "again %u: %u units", again, nseen);
		for (uint16_t i = 0; i < 7 && i < nseen; i++)
			CHECK(seen[i].at == at[i] && seen[i].nsyms == lens[i] && seen[i].errs == 0 && seen[i].flags == 0,
			      "again %u unit %u: at %u len %u errs %u", again, i, (unsigned)seen[i].at, seen[i].nsyms, (unsigned)seen[i].errs);
		CHECK(nhead == (again ? 14 : 7), "again %u: %u head calls", again, nhead);
	}
}

// A unit longer than the buffer comes in pieces
static void testPieces(void)
{
	uint32_t at;

	mode_p25ish();
	mode.frame_syms = 1000;
	st_reset();
	st_random(5);
	at = st_frames(3, 1000);
	begin(300, true);
	feed(0, st_n, 0, &CLEAN);
	CHECK(nseen == 12, "%u pieces", nseen);
	for (uint16_t i = 0; i < 12 && i < nseen; i++) {
		uint16_t k = i % 4;

		CHECK(seen[i].at == at + 1000u * (i / 4) && seen[i].offset == 300 * k, "piece %u at %u + %u", i, (unsigned)seen[i].at, seen[i].offset);
		CHECK(seen[i].nsyms == (k < 3 ? 300 : 100) && seen[i].errs == 0, "piece %u len %u errs %u", i, seen[i].nsyms, (unsigned)seen[i].errs);
		CHECK(!(seen[i].flags & AIR_FRAME_MORE) == (k == 3), "piece %u flags %x", i, seen[i].flags);
	}
}

// The sync in the middle of the frame: its first symbols come from the history ring
static void testSyncInside(void)
{
	for (uint8_t soft = 0; soft < 2; soft++) {
		uint32_t at;

		mode_p25ish();
		mode.frame_syms = 144;
		mode.profile.sync[0].frame_at = -66;
		mode.lost_frames = 8;
		st_reset();
		st_random(400 + rng() % 100);
		at = st_n;
		for (uint8_t i = 0; i < 25; i++) {
			st_random(66);
			// Only one frame in six carries the sync, as a voice superframe does
			if (i % 6 == 0)
				st_packed(FS48, 24);
			else
				st_random(24);
			st_random(54);
		}
		begin(144, true);
		feed(0, st_n, 0, soft ? &CLEAN : &HARD);
		CHECK(nseen == 25, "soft %u: %u frames", soft, nseen);
		for (uint16_t i = 0; i < nseen; i++) {
			CHECK(seen[i].at == at + 144u * i && seen[i].errs == 0, "soft %u frame %u at %u errs %u", soft, i, (unsigned)seen[i].at, (unsigned)seen[i].errs);
			CHECK(!(seen[i].flags & AIR_FRAME_NO_SYNC) == (i % 6 == 0), "frame %u flags %x", i, seen[i].flags);
		}
	}
}

// A second signal at another place takes over once the first one's sync was missed, and never before
static void testTakeover(void)
{
	uint32_t a, b;

	mode_p25ish();
	mode.lost_frames = 4;
	st_reset();
	st_random(20);
	a = st_frames(4, 200);
	st_random(137);			// the first signal ends; another starts 137 symbols on
	b = st_frames(6, 200);
	begin(200, true);
	feed(0, st_n, 0, &CLEAN);
	// The first signal's 5th frame: 137 symbols of nothing, then the new sync inside it
	CHECK(nlost == 1 && fr.stats.locks == 2, "lost %u locks %u", nlost, fr.stats.locks);
	CHECK(nseen == 10, "%u frames", nseen);
	for (uint16_t i = 0; i < 4 && i < nseen; i++)
		CHECK(seen[i].at == a + 200u * i && seen[i].errs == 0, "first %u", i);
	for (uint16_t i = 4; i < nseen; i++)
		CHECK(seen[i].at == b + 200u * (i - 4) && seen[i].errs == 0, "second %u at %u", i, (unsigned)seen[i].at);

	// A sync pattern inside the payload of a frame whose own sync was seen changes nothing
	mode_p25ish();
	st_reset();
	a = st_frames(5, 200);
	for (uint8_t i = 0; i < 24; i++)
		st_sym[a + 2 * 200 + 90 + i] = air_sym_get(FS48, i);
	begin(200, true);
	feed(0, st_n, 0, &CLEAN);
	CHECK(nseen == 5 && nlost == 0 && fr.stats.locks == 1, "payload sync: %u frames %u lost", nseen, nlost);
	for (uint16_t i = 0; i < nseen; i++)
		CHECK(seen[i].at == a + 200u * i && seen[i].errs == 0, "payload sync frame %u", i);
}

// A short sync with inner symbols and a head hook that tells the real frames from chance matches
static void testShortSync(void)
{
	uint32_t at;
	static const chan_t noisy = { 100, 5, 6, true, 0 };

	for (uint8_t pass = 0; pass < 3; pass++) {
		memset(&mode, 0, sizeof(mode));
		mode.name = "s";
		mode.profile.sym_rate = 4800;
		mode.profile.levels = 4;
		mode.profile.nsyncs = 1;
		mode.profile.sync[0].id = 1;
		mode.profile.sync[0].nsyms = 10;
		mode.profile.sync[0].max_errs = 0;
		memcpy(mode.profile.sync[0].syms, FSW20, 6);
		mode.frame_syms = 192;
		mode.lost_frames = 3;
		mode.head = on_head;
		mode.head_syms = 26;
		mode.rx_frame = on_frame;
		mode.rx_lost = on_lost;
		head_len_at = 0;
		head_magic = 0x9;	// the test hook reads symbols 24 and 25
		head_again = false;
		sym_rate = 4800;
		st_reset();
		st_random(3000);
		// A chance match the hook will refuse, with the real first frame starting inside its head
		at = st_n + 14;
		for (uint8_t i = 0; i < 10; i++)
			st_sym[st_n++] = air_sym_get(FSW20, i);
		st_random(4);
		for (uint8_t f = 0; f < 40; f++) {
			uint32_t s = st_n;

			for (uint8_t i = 0; i < 10; i++)
				st_sym[st_n++] = air_sym_get(FSW20, i);
			st_random(182);
			st_sym[s + 24] = 2;
			st_sym[s + 25] = 1;
		}
		begin(192, pass != 1);
		feed(0, st_n, 0, pass == 0 ? &CLEAN : pass == 1 ? &HARD : &noisy);
		{
			uint16_t good = 0;

			for (uint16_t i = 0; i < nseen; i++)
				if (seen[i].at >= at && (seen[i].at - at) % 192 == 0 && seen[i].errs == 0)
					good++;
			CHECK(good == 40, "pass %u: %u of 40 frames (%u seen, %u rejects)", pass, good, nseen, fr.stats.rejects);
			CHECK(nseen == good, "pass %u: %u frames that are none", pass, nseen - good);
			CHECK(fr.stats.rejects >= 1, "pass %u: the chance match was not refused", pass);
		}
	}
}

// Nothing but noise: how often does a 24-symbol sync "lock"?
static void testNoise(void)
{
	uint32_t n = 0;

	mode_p25ish();
	begin(200, true);
	for (uint8_t r = 0; r < 60; r++) {	// 60 x 36000 symbols = 7.5 minutes at 4800
		chan_t ch = { 100, 0, 0, true, 0 };

		st_reset();
		st_random(36000);
		// Full-swing noise, as a discriminator gives with no carrier
		ch.gain_pct = 30;
		ch.noise = 110;
		feed(0, st_n, 144, &ch);
		n += st_n;
	}
	printf("noise: %u symbols, %u locks, %u frames\n", (unsigned)n, fr.stats.locks, fr.stats.frames);
	CHECK(fr.stats.locks <= 2, "%u locks on noise", fr.stats.locks);
}

// Random chunk sizes, flags and content: nothing may crash or write outside the buffers (run under ASan)
static void testFuzz(void)
{
	static uint8_t guard_hard[(256 + 3) / 4 + 8];
	static int8_t guard_soft[256 + 8];

	for (uint16_t round = 0; round < 300; round++) {
		uint16_t buf = (uint16_t)(48 + rng() % 200);

		mode_p25ish();
		mode.frame_syms = (uint16_t)(30 + rng() % 500);
		mode.lost_frames = (uint8_t)(1 + rng() % 4);
		mode.profile.nsyncs = (uint8_t)(1 + rng() % 3);
		for (uint8_t i = 0; i < mode.profile.nsyncs; i++) {
			air_sync_t *s = &mode.profile.sync[i];

			s->id = i;
			s->nsyms = (uint8_t)(8 + rng() % 17);
			s->max_errs = (uint8_t)(rng() % 5);
			s->flags = (uint8_t)(rng() & 1);
			s->frame_at = (int8_t)((int)(rng() % 120) - 90);
			for (uint8_t j = 0; j < 6; j++)
				s->syms[j] = (uint8_t)rng();
		}
		if (rng() & 1) {
			mode.head = on_head;
			mode.head_syms = (uint16_t)(26 + rng() % 20);
			head_len_at = (uint16_t)(rng() % 3 ? 26 + rng() % 30 : 0);
			head_again = rng() & 1;
			head_magic = rng() & 1 ? 0xFF : (uint8_t)(rng() & 15);
		}
		memset(guard_hard, 0xA5, sizeof(guard_hard));
		memset(guard_soft, 0x5A, sizeof(guard_soft));
		nseen = nlost = nhead = 0;
		air_framer_init(&fr, &mode, &mode, guard_hard, rng() & 1 ? guard_soft : NULL, buf);
		st_reset();
		for (uint8_t k = 0; k < 20; k++) {
			st_random(rng() % 300);
			st_packed(mode.profile.sync[rng() % mode.profile.nsyncs].syms, mode.profile.sync[0].nsyms);
		}
		for (uint32_t from = 0; from < st_n;) {
			chan_t ch = { (int)(40 + rng() % 120), (int)(rng() % 60) - 30, (int)(rng() % 20), (rng() & 3) != 0,
				      (uint8_t)((rng() % 10) ? 0 : (rng() & 0x0F)) };
			uint32_t to = from + 1 + rng() % 500;

			feed(from, to > st_n ? st_n : to, 0, &ch);
			from = to;
		}
		for (uint16_t i = (uint16_t)((buf + 3) / 4); i < sizeof(guard_hard); i++)
			CHECK(guard_hard[i] == 0xA5, "round %u: wrote past the hard buffer at %u", round, i);
		for (uint16_t i = buf; i < sizeof(guard_soft); i++)
			CHECK(guard_soft[i] == 0x5A, "round %u: wrote past the soft buffer at %u", round, i);
		for (uint16_t i = 0; i < nseen; i++)
			CHECK(seen[i].nsyms <= buf, "round %u: a frame of %u in a buffer of %u", round, seen[i].nsyms, buf);
	}
}

// ── Transmit: a fake provider ──
static air_tx_chunk_t q[16];
static uint8_t qn, q_depth = 4, q_ends;
static bool q_queue(const air_tx_chunk_t *c)
{
	if (qn >= q_depth)
		return false;
	q[qn++] = *c;
	return true;
}
static uint8_t q_space(void) { return (uint8_t)(q_depth - qn); }
static void q_end(void) { q_ends++; }
static hal_air_t fake = { .tx_queue = q_queue, .tx_space = q_space, .tx_end = q_end, .tx_chunk_syms = 144, .tx_queue_depth = 4 };

static void testTx(void)
{
	uint8_t syms[250];

	for (uint16_t i = 0; i < sizeof(syms); i++)
		syms[i] = (uint8_t)rng();
	mode_p25ish();
	begin(200, false);
	qn = q_ends = 0;
	air_framer_tx_start(&fr, AIR_FIT_TX);
	CHECK(air_framer_tx_space(&fr, &fake) == 4 * 144, "space %u", air_framer_tx_space(&fr, &fake));
	CHECK(air_framer_tx(&fr, &fake, syms, 0, 100), "100");
	CHECK(qn == 0 && air_framer_tx_space(&fr, &fake) == 4 * 144 - 100, "open chunk");
	CHECK(air_framer_tx(&fr, &fake, syms, 100, 400), "400");
	CHECK(qn == 3 && air_framer_tx_space(&fr, &fake) == 144 - 68, "3 queued, %u", air_framer_tx_space(&fr, &fake));
	CHECK(!air_framer_tx(&fr, &fake, syms, 0, 77), "over the space");
	CHECK(air_framer_tx(&fr, &fake, syms, 500, 76), "the rest");
	CHECK(qn == 4 && air_framer_tx_space(&fr, &fake) == 0, "full");
	for (uint16_t i = 0; i < 576; i++)
		if (air_sym_get(q[i / 144].syms, i % 144) != air_sym_get(syms, i)) {
			CHECK(false, "symbol %u", i);
			break;
		}
	for (uint8_t i = 0; i < 4; i++)
		CHECK(q[i].nsyms == 144 && q[i].flags == 0, "chunk %u", i);
	// Nothing open: the end goes to the provider
	CHECK(air_framer_tx_finish(&fr, &fake) && q_ends == 1, "finish on a chunk boundary");
	// A short last chunk, refused while the queue is full, taken later
	qn = 3;
	air_framer_tx_start(&fr, AIR_FIT_TX);
	CHECK(air_framer_tx(&fr, &fake, syms, 8, 30), "30");
	qn = 4;
	CHECK(!air_framer_tx_finish(&fr, &fake), "finish into a full queue");
	qn = 0;
	CHECK(air_framer_tx_finish(&fr, &fake) && qn == 1 && q[0].nsyms == 30 && q[0].flags == AIR_TX_LAST && q_ends == 1, "short last chunk");
	CHECK(air_sym_get(q[0].syms, 29) == air_sym_get(syms, 37), "its content");
	// Half rate: every symbol twice
	qn = 0;
	air_framer_tx_start(&fr, AIR_FIT_TX | AIR_FIT_RATE_DOUBLED);
	CHECK(air_framer_tx_space(&fr, &fake) == 2 * 144, "doubled space");
	CHECK(air_framer_tx(&fr, &fake, syms, 0, 100) && qn == 1, "doubled");
	for (uint16_t i = 0; i < 144; i++)
		if (air_sym_get(q[0].syms, i) != air_sym_get(syms, i / 2)) {
			CHECK(false, "doubled symbol %u", i);
			break;
		}
}

int main(void)
{
	testFixedFrames();
	testLevels();
	testLowLevel();
	testMissedSyncAndLoss();
	testGap();
	testVariableUnits();
	testPieces();
	testSyncInside();
	testTakeover();
	testShortSync();
	testNoise();
	testFuzz();
	testTx();
	printf("airFramerTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
