// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The generic channel-mode layer (docs/spec/channel-modes.md) on a simulated NOR flash, with a synthetic kind and system
// type registered the way a mode module would: name check vectors, a tag's bytes, the live rule, a channel write that
// renames, makes plain or lands on a stale tag, a delete, a kind this build doesn't know, the parameter block, a full
// area, and the mode-scoped record families (a kind's radio-wide record, targets, receive lists, traffic keys)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The synthetic mode, as a module would register it; defined before the registry is included
#define TEST_KIND	0x80u
#define TEST_SYSTYPE	0x80u
#define CODEPLUG_KINDS_EXTRA(X)		X(0x80, TEST, "TST", 1)
#define CODEPLUG_SYSTEM_TYPES_EXTRA(X)	X(0x80, TEST, "TSY", 1)
#define CODEPLUG_MODE_RECORDS		1
#include "../../core/codeplug/codeplug_tag.c"
#include "../../core/codeplug/codeplug_records.c"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static channel_t make(const char *name, uint8_t mode, bool rx_only)
{
	channel_t c;
	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = c.tx_hz = 451000000;
	c.mode = mode;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	c.rx_only = rx_only;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

static channel_t get(uint16_t n)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	CHECK(codeplug_channel(n, &c));
	return c;
}

static int tag_bytes(uint16_t n, uint8_t *v, uint16_t max)
{
	return codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + n - 1u), v, max);
}

// A channel of the test kind at n: the record in its safe shape, then the tag, as the editor writes them
static void tagged(uint16_t n, const char *name, uint8_t flags, const uint8_t *params, uint16_t len)
{
	channel_t c = make(name, RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(n, &c));
	codeplug_tag_t t = { .kind = TEST_KIND, .system = 2, .flags = flags };
	CHECK(codeplug_tag_write(n, name, &t, params, len));
}

static void test_name_check(void)
{
	// The spec's vectors (docs/spec/channel-modes.md 2.2)
	CHECK(codeplug_name_check("") == 0x706Cu);
	CHECK(codeplug_name_check("A") == 0x18A9u);
	CHECK(codeplug_name_check("T3 Ops") == 0x1687u);
	CHECK(codeplug_name_check("Sixteen chars!!!") == 0xCDD2u);
	// A record's padding (0xFF) and what follows the end don't count
	char padded[CODEPLUG_NAME_LEN];
	memset(padded, 0xFF, sizeof padded);
	memcpy(padded, "A", 1);
	CHECK(codeplug_name_check(padded) == 0x18A9u);
	char tail[CODEPLUG_NAME_LEN] = { 'A', 0, 'x', 'y' };
	CHECK(codeplug_name_check(tail) == 0x18A9u);
}

static void test_tag(void)
{
	fresh();
	const uint8_t params[3] = { 0x11, 0x22, 0x33 };
	tagged(10, "T Ops", CODEPLUG_TAG_TX, params, sizeof params);
	uint8_t v[32];
	CHECK(tag_bytes(10, v, sizeof v) == 8);
	CHECK(v[0] == TEST_KIND && v[1] == 2 && v[2] == (codeplug_name_check("T Ops") & 0xFF) &&
	      v[3] == (codeplug_name_check("T Ops") >> 8) && v[4] == CODEPLUG_TAG_TX && v[5] == 0x11 && v[7] == 0x33);

	channel_t c = get(10);
	CHECK(c.kind == TEST_KIND && c.system == 2 && !c.rx_only && c.mode == RADIO_MODE_DMR);
	CHECK(codeplug_kind_built(c.kind) && strcmp(codeplug_kind_name(c.kind), "TST") == 0);
	CHECK(codeplug_systype_built(TEST_SYSTYPE) && strcmp(codeplug_systype_name(TEST_SYSTYPE), "TSY") == 0);
	// A kind the registry names with no engine behind it isn't built; one with an engine follows its switch
	CHECK(!codeplug_kind_built(0) && !codeplug_kind_built(0xFF) && !codeplug_kind_built(CODEPLUG_KIND_P25_TRUNK));
	CHECK(codeplug_kind_built(CODEPLUG_KIND_DMR_TRUNK) == (CONFIG_DMR_TRUNK != 0));
	CHECK(strcmp(codeplug_kind_name(CODEPLUG_KIND_DMR_TRUNK), "T3") == 0);

	// The parameter block, whole and cut
	uint8_t p[8 + CODEPLUG_TAG_HEAD_MAX];
	CHECK(codeplug_tag_params(10, &c, p, 8) == 3 && memcmp(p, params, 3) == 0);
	memset(p, 0, sizeof p);
	CHECK(codeplug_tag_params(10, &c, p, 2) == 3 && p[0] == 0x11 && p[1] == 0x22);
	channel_t plain = make("Plain", RADIO_MODE_FM, false);
	CHECK(codeplug_set_channel(11, &plain));
	plain = get(11);
	CHECK(plain.kind == CODEPLUG_KIND_NONE && codeplug_tag_params(11, &plain, p, 8) == -1);

	// With a fallback: the parameter block starts after it
	codeplug_tag_t t = { .kind = TEST_KIND, .system = 2, .flags = CODEPLUG_TAG_FALLBACK, .fb_chan = 3, .fb_check = 0xBEEF };
	CHECK(codeplug_tag_write(10, "T Ops", &t, params, 1));
	CHECK(tag_bytes(10, v, sizeof v) == 10 && v[5] == 3 && v[6] == 0 && v[7] == 0xEF && v[8] == 0xBE && v[9] == 0x11);
	codeplug_tag_t r;
	CHECK(codeplug_tag_read(10, &r) && r.fb_chan == 3 && r.fb_check == 0xBEEF && codeplug_tag_params_at(&r) == 9);
	c = get(10);
	CHECK(c.kind == TEST_KIND && c.rx_only);	// no TX flag: receive only
	CHECK(codeplug_tag_params(10, &c, p, 8) == 1 && p[0] == 0x11);
}

static void test_live_rule(void)
{
	fresh();
	tagged(10, "T Ops", CODEPLUG_TAG_TX, NULL, 0);
	// The tag of another name, or on a record no longer DMR receive only, isn't live
	codeplug_tag_t t = { .kind = TEST_KIND, .system = 2, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, "Other", &t, NULL, 0));
	CHECK(get(10).kind == CODEPLUG_KIND_NONE);
	CHECK(codeplug_tag_write(10, "T Ops", &t, NULL, 0));
	CHECK(get(10).kind == TEST_KIND);

	// A plain write over a tagged channel (an editor that doesn't know the kind keeps kind 0): FM, transmit. The tag
	// is this channel's, so it goes
	channel_t fm = make("T Ops", RADIO_MODE_FM, false);
	CHECK(codeplug_set_channel(10, &fm));
	uint8_t v[16];
	CHECK(tag_bytes(10, v, sizeof v) < 0);
	channel_t c = get(10);
	CHECK(c.kind == CODEPLUG_KIND_NONE && c.mode == RADIO_MODE_FM && !c.rx_only);

	// BASE_OWN: the record stands on its own, any shape
	CHECK(codeplug_tag_write(10, "T Ops", &(codeplug_tag_t){ .kind = TEST_KIND, .flags = CODEPLUG_TAG_BASE_OWN }, NULL, 0));
	c = get(10);
	CHECK(c.kind == TEST_KIND && c.mode == RADIO_MODE_FM && c.rx_only);
}

static void test_write_follows(void)
{
	fresh();
	const uint8_t params[2] = { 0xA5, 0x5A };
	tagged(10, "T Ops", CODEPLUG_TAG_TX, params, sizeof params);

	// Rename, new system, receive only: the tag follows, the record keeps its safe shape, the block stays
	channel_t c = get(10);
	strcpy(c.name, "T Renamed");
	c.system = 5;
	c.rx_only = true;
	c.mode = RADIO_MODE_FM;		// a kind's channel is never written FM without BASE_OWN
	CHECK(codeplug_set_channel(10, &c));
	uint8_t v[16];
	CHECK(tag_bytes(10, v, sizeof v) == 7);
	CHECK(v[1] == 5 && v[2] == (codeplug_name_check("T Renamed") & 0xFF) && (v[4] & CODEPLUG_TAG_TX) == 0);
	CHECK(v[5] == 0xA5 && v[6] == 0x5A);
	c = get(10);
	CHECK(c.kind == TEST_KIND && c.system == 5 && c.rx_only && c.mode == RADIO_MODE_DMR && strcmp(c.name, "T Renamed") == 0);

	// Transmit allowed again: the flag follows; the record stays receive only underneath
	c.rx_only = false;
	CHECK(codeplug_set_channel(10, &c));
	c = get(10);
	CHECK(c.kind == TEST_KIND && !c.rx_only);
	CHECK(codeplug_tag_remove(10));
	c = get(10);
	CHECK(c.kind == CODEPLUG_KIND_NONE && c.rx_only && c.mode == RADIO_MODE_DMR);

	// A copy at another number keeps the safe shape and gets no tag
	tagged(10, "T Ops", CODEPLUG_TAG_TX, NULL, 0);
	c = get(10);
	CHECK(codeplug_set_channel(12, &c));
	CHECK(tag_bytes(12, v, sizeof v) < 0);
	c = get(12);
	CHECK(c.kind == CODEPLUG_KIND_NONE && c.mode == RADIO_MODE_DMR && c.rx_only);

	// ... until the copy takes the tag: the new name's check, its system and transmit flag, the block as it was
	const uint8_t blk[3] = { 1, 2, 3 };
	tagged(10, "T Ops", CODEPLUG_TAG_TX, blk, sizeof blk);
	c = get(10);
	strcpy(c.name, "T Ops 2");
	c.system = 7;
	c.rx_only = true;
	CHECK(codeplug_set_channel(12, &c) && codeplug_tag_copy(10, 12, &c));
	CHECK(tag_bytes(12, v, sizeof v) == 8 && v[1] == 7 && v[2] == (codeplug_name_check("T Ops 2") & 0xFF));
	CHECK((v[4] & CODEPLUG_TAG_TX) == 0 && v[5] == 1 && v[7] == 3);
	c = get(12);
	CHECK(c.kind == TEST_KIND && c.system == 7 && c.rx_only && strcmp(c.name, "T Ops 2") == 0);
	CHECK(get(10).kind == TEST_KIND && !get(10).rx_only);	// the source is untouched
	// A mixed channel (its record a channel of its own: FM, wide, transmitting) copied on the radio: the copy is
	// written as the kind runs and so in the stand-in shape, then takes the source's tag and its record's own mode,
	// receive-only bit and bandwidth. Both halves arrive; the source keeps both
	channel_t fm = make("T Mixed", RADIO_MODE_FM, false);
	fm.bw = RADIO_BW_25K;
	CHECK(codeplug_set_channel(14, &fm));
	codeplug_tag_t own = { .kind = TEST_KIND, .system = 2, .flags = CODEPLUG_TAG_BASE_OWN | CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(14, "T Mixed", &own, blk, sizeof blk));
	c = get(14);
	strcpy(c.name, "T Mixed 2");
	c.mode = RADIO_MODE_DMR;
	c.bw = RADIO_BW_12K5;
	CHECK(codeplug_set_channel(15, &c));
	CHECK(get(15).kind == CODEPLUG_KIND_NONE && get(15).rx_only && get(15).mode == RADIO_MODE_DMR);	// the stand-in
	CHECK(codeplug_tag_copy(14, 15, &c));
	CHECK(tag_bytes(15, v, sizeof v) == 8 && v[4] == (CODEPLUG_TAG_BASE_OWN | CODEPLUG_TAG_TX) && v[5] == 1 && v[7] == 3);
	c = get(15);
	CHECK(c.kind == TEST_KIND && !c.rx_only && c.mode == RADIO_MODE_FM && c.bw == RADIO_BW_25K &&
	      strcmp(c.name, "T Mixed 2") == 0);
	CHECK(codeplug_tag_remove(15));
	c = get(15);
	CHECK(c.kind == CODEPLUG_KIND_NONE && !c.rx_only && c.mode == RADIO_MODE_FM && c.bw == RADIO_BW_25K);
	CHECK(codeplug_tag_remove(14) && get(14).mode == RADIO_MODE_FM && get(14).bw == RADIO_BW_25K && !get(14).rx_only);
	CHECK(codeplug_delete_channel(14) && codeplug_delete_channel(15));
	// No copy from a plain channel, a stale tag, to itself, or for a plain channel
	CHECK(!codeplug_tag_copy(10, 10, &c) && !codeplug_tag_copy(13, 12, &c));
	channel_t plain = make("T Ops", RADIO_MODE_DMR, true);
	CHECK(!codeplug_tag_copy(10, 12, &plain));
	codeplug_tag_t st = { .kind = TEST_KIND, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, "Gone", &st, NULL, 0) && !codeplug_tag_copy(10, 12, &c));
}

// A kind's block edited on the radio (codeplug_tag_rewrite): the system and transmit allowed from the writer, the
// front of the block; the fallback, the other flags and the block's later bytes as stored
static void test_rewrite(void)
{
	fresh();
	const uint8_t blk[5] = { 1, 2, 3, 4, 5 };
	const uint8_t front[2] = { 0x11, 0x22 };
	channel_t c = make("T Ops", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(10, &c));
	codeplug_tag_t t = { .kind = TEST_KIND, .system = 2, .fb_chan = 40, .fb_check = 0xBEEF,
			     .flags = CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_BASE_OWN | 0x48u };
	CHECK(codeplug_tag_write(10, "T Ops", &t, blk, sizeof blk));
	codeplug_tag_t w = { .kind = TEST_KIND, .system = 6, .flags = CODEPLUG_TAG_TX };
	uint8_t v[24];
	CHECK(codeplug_tag_rewrite(10, "T Ops", &w, front, sizeof front));
	CHECK(tag_bytes(10, v, sizeof v) == 14 && v[0] == TEST_KIND && v[1] == 6);
	CHECK(v[4] == (CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_BASE_OWN | CODEPLUG_TAG_TX | 0x48u) && v[5] == 40 &&
	      v[6] == 0 && v[7] == 0xEF && v[8] == 0xBE);
	CHECK(v[9] == 0x11 && v[10] == 0x22 && v[11] == 3 && v[12] == 4 && v[13] == 5);
	// A block longer than the stored one grows it; without a fallback it starts after the short header
	tagged(11, "T Two", 0, blk, 1);
	CHECK(codeplug_tag_rewrite(11, "T Two", &w, front, sizeof front));
	CHECK(tag_bytes(11, v, sizeof v) == 7 && v[4] == CODEPLUG_TAG_TX && v[5] == 0x11 && v[6] == 0x22);
	// Under a base-own tag the record is a channel of its own, here FM, wide, transmitting. A write from the radio
	// carries the kind's mode, receive-only bit and bandwidth in their place: the record keeps its own three, takes
	// the rest (the name, the frequency), and the tag stays live
	channel_t fm = make("T Mixed", RADIO_MODE_FM, false);
	fm.bw = RADIO_BW_25K;
	CHECK(codeplug_set_channel(13, &fm));
	codeplug_tag_t own = { .kind = TEST_KIND, .system = 2, .flags = CODEPLUG_TAG_BASE_OWN };
	CHECK(codeplug_tag_write(13, "T Mixed", &own, blk, sizeof blk));
	c = get(13);
	CHECK(c.kind == TEST_KIND && c.rx_only && c.mode == RADIO_MODE_FM && c.bw == RADIO_BW_25K);
	strcpy(c.name, "T Mixed 2");
	c.mode = RADIO_MODE_DMR;	// as a kind's channel runs: its own mode and bandwidth, the tag's receive only
	c.bw = RADIO_BW_12K5;
	c.rx_hz += 12500;
	CHECK(codeplug_set_channel(13, &c) && codeplug_tag_rewrite(13, "T Mixed 2", &w, front, sizeof front));
	c = get(13);
	CHECK(c.kind == TEST_KIND && !c.rx_only && c.mode == RADIO_MODE_FM && c.bw == RADIO_BW_25K &&
	      c.rx_hz == fm.rx_hz + 12500 && strcmp(c.name, "T Mixed 2") == 0);
	CHECK(codeplug_tag_remove(13));
	c = get(13);
	CHECK(c.kind == CODEPLUG_KIND_NONE && !c.rx_only && c.mode == RADIO_MODE_FM && c.bw == RADIO_BW_25K);
	// No tag, another kind's, one under another name: nothing written
	codeplug_tag_t other = { .kind = TEST_KIND + 1u, .system = 6, .flags = CODEPLUG_TAG_TX };
	CHECK(!codeplug_tag_rewrite(12, "T Ops", &w, front, sizeof front) && tag_bytes(12, v, sizeof v) < 0);
	CHECK(!codeplug_tag_rewrite(10, "T Ops", &other, front, sizeof front));
	CHECK(!codeplug_tag_rewrite(10, "Other", &w, front, sizeof front));
	CHECK(tag_bytes(10, v, sizeof v) == 14 && v[1] == 6 && v[9] == 0x11);
}

static void test_stale_and_delete(void)
{
	fresh();
	tagged(10, "T Ops", CODEPLUG_TAG_TX, NULL, 0);
	// A tag left at a number whose channel another tool replaced: kept as found, never live
	channel_t other = make("Plain", RADIO_MODE_DMR, true);
	codeplug_tag_t t = { .kind = TEST_KIND, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, "Gone", &t, NULL, 0));
	CHECK(codeplug_set_channel(10, &other));
	codeplug_tag_t r;
	CHECK(codeplug_tag_read(10, &r) && r.check == codeplug_name_check("Gone"));
	CHECK(get(10).kind == CODEPLUG_KIND_NONE);
	// A plain channel written with the stale tag's name would make it live: it goes
	channel_t gone = make("Gone", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(10, &gone));
	CHECK(!codeplug_tag_read(10, &r) && get(10).kind == CODEPLUG_KIND_NONE);

	// A delete takes the tag; a new channel at that number starts plain
	tagged(10, "T Ops", CODEPLUG_TAG_TX, NULL, 0);
	CHECK(codeplug_delete_channel(10));
	CHECK(!codeplug_tag_read(10, &r));
	// A tag at a free number (another tool's leftover) goes with the first write there
	CHECK(codeplug_tag_write(10, "T Ops", &t, NULL, 0));
	channel_t c = make("T Ops", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(10, &c));
	CHECK(!codeplug_tag_read(10, &r) && get(10).kind == CODEPLUG_KIND_NONE);

	// A later firmware's area is never rewritten: a delete with no tag to take still succeeds
	CHECK(codeplug_kv_set_u32(0x7000u, 1));
	sim_flash_data[EE + CODEPLUG_KV_ADDR + 4] = CODEPLUG_KV_VERSION + 1;
	CHECK(codeplug_delete_channel(10));
	CHECK(!codeplug_channel_used(10));
}

static void test_unknown_kind(void)
{
	fresh();
	channel_t c = make("Future", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(10, &c));
	// A kind from a later firmware with a block longer than this one writes: read, kept by a rename
	uint8_t v[5 + 100];
	memset(v, 0x77, sizeof v);
	v[0] = 0x99;
	v[1] = CODEPLUG_SYSTEM_NONE;
	v[2] = (uint8_t)codeplug_name_check("Future");
	v[3] = (uint8_t)(codeplug_name_check("Future") >> 8);
	v[4] = CODEPLUG_TAG_TX | 0x08u | 0x30u;	// the reserved bit and the kind's bits, kept as found
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_CH_TAG + 9u), v, sizeof v));
	c = get(10);
	CHECK(c.kind == 0x99 && !codeplug_kind_built(0x99) && strcmp(codeplug_kind_name(0x99), "?") == 0);
	strcpy(c.name, "Future 2");
	CHECK(codeplug_set_channel(10, &c));
	uint8_t w[sizeof v];
	CHECK(tag_bytes(10, w, sizeof w) == (int)sizeof v);
	CHECK(w[0] == 0x99 && w[4] == v[4] && memcmp(&w[5], &v[5], sizeof v - 5u) == 0);
	CHECK(get(10).kind == 0x99);

	// Too short to be a tag, kind 0 or 0xFF: no tag
	uint8_t bad[5] = { 0, 0, 0, 0, 0 };
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_CH_TAG + 10u), bad, 5));
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_CH_TAG + 11u), v, 4));
	codeplug_tag_t r;
	CHECK(!codeplug_tag_read(11, &r) && !codeplug_tag_read(12, &r));
	// A parameter block longer than this firmware writes is refused, not cut
	uint8_t big[TAG_PARAMS_MAX + 1];
	codeplug_tag_t t = { .kind = TEST_KIND };
	CHECK(!codeplug_tag_write(10, "Future 2", &t, big, sizeof big));
}

static void test_full_area(void)
{
	fresh();
	channel_t c = make("T Ops", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(10, &c));
	// Fill the area, then a tag can't be written; the channel itself still can
	uint8_t blob[200];
	memset(blob, 0x5C, sizeof blob);
	uint16_t k = 0x7000u;
	while (codeplug_kv_set(k, blob, sizeof blob)) {
		k++;
	}
	while (codeplug_kv_set(k, blob, 8)) {
		k++;
	}
	codeplug_tag_t t = { .kind = TEST_KIND, .flags = CODEPLUG_TAG_TX };
	CHECK(!codeplug_tag_write(10, "T Ops", &t, blob, 40));
	CHECK(get(10).kind == CODEPLUG_KIND_NONE);
	CHECK(codeplug_kv_remove(0x7000u));
	CHECK(codeplug_tag_write(10, "T Ops", &t, blob, 40));
	CHECK(get(10).kind == TEST_KIND);
}

static void put_rec(uint16_t key, uint8_t kind, uint8_t flags, const char *name, const uint8_t *rest, uint16_t len)
{
	uint8_t v[CODEPLUG_REC_HDR + 64];
	v[0] = kind;
	v[1] = flags;
	memset(&v[2], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&v[2], name, strlen(name));
	memcpy(&v[CODEPLUG_REC_HDR], rest, len);
	CHECK(codeplug_kv_set(key, v, (uint16_t)(CODEPLUG_REC_HDR + len)));
}

static void test_records(void)
{
	fresh();
	// A kind's radio-wide record: identity and options, the kind's bytes
	uint8_t ms[] = { 'N', '0', 'C', 'A', 'L', 'L', 0x01 };
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + TEST_KIND), ms, sizeof ms));
	uint8_t b[64];
	CHECK(codeplug_mode_settings(TEST_KIND, b, sizeof b) == (int)sizeof ms && memcmp(b, ms, sizeof ms) == 0);
	CHECK(codeplug_mode_settings(0x81, b, sizeof b) == -1 && codeplug_mode_settings(0, b, sizeof b) == -1);

	// Targets: a call sign address and a wide numeric one
	const uint8_t call[] = { 'C', 'Q', 'C', 'Q', 'C', 'Q' };
	const uint8_t wide[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
	put_rec(CODEPLUG_KEY_TARGET + 0, TEST_KIND, CODEPLUG_TARGET_ALL, "Everyone", call, sizeof call);
	put_rec(CODEPLUG_KEY_TARGET + 1, TEST_KIND, CODEPLUG_TARGET_GROUP | 0x40u, "Group 40-bit", wide, sizeof wide);
	put_rec(CODEPLUG_KEY_TARGET + 2, 0x81, CODEPLUG_TARGET_INDIVIDUAL, "Other kind", wide, sizeof wide);
	codeplug_rec_head_t h;
	uint8_t addr[16 + CODEPLUG_REC_HDR];
	codeplug_ref_t r0 = { 1, codeplug_name_check("Everyone") };
	codeplug_ref_t r1 = { 2, codeplug_name_check("Group 40-bit") };
	codeplug_ref_t r2 = { 3, codeplug_name_check("Other kind") };
	CHECK(codeplug_target(r0, TEST_KIND, &h, addr, 16) == 6 && memcmp(addr, call, 6) == 0);
	CHECK(strcmp(h.name, "Everyone") == 0 && (h.flags & CODEPLUG_TARGET_TYPE) == CODEPLUG_TARGET_ALL);
	CHECK(codeplug_target(r1, TEST_KIND, &h, addr, 3) == 5 && addr[2] == 0x03 && h.flags == 0x41u);
	CHECK(codeplug_target(r2, TEST_KIND, &h, addr, 16) == -1);		// another kind's
	CHECK(codeplug_target((codeplug_ref_t){ 0, 0 }, TEST_KIND, &h, addr, 16) == -1);
	// Replaced under the reference: a new name doesn't resolve
	put_rec(CODEPLUG_KEY_TARGET + 0, TEST_KIND, CODEPLUG_TARGET_ALL, "Someone else", call, sizeof call);
	CHECK(codeplug_target(r0, TEST_KIND, &h, addr, 16) == -1);

	// A receive list of references, each resolving on its own
	uint8_t refs[3 * CODEPLUG_REF_LEN];
	const codeplug_ref_t in[3] = { r0, r1, { 9, 0x1234 } };
	for (unsigned i = 0; i < 3; i++) {
		refs[i * 4] = (uint8_t)in[i].index;
		refs[i * 4 + 1] = (uint8_t)(in[i].index >> 8);
		refs[i * 4 + 2] = (uint8_t)in[i].check;
		refs[i * 4 + 3] = (uint8_t)(in[i].check >> 8);
	}
	put_rec(CODEPLUG_KEY_TLIST + 4, TEST_KIND, 0, "Listen", refs, sizeof refs);
	codeplug_ref_t out[2];
	codeplug_ref_t lr = { 5, codeplug_name_check("Listen") };
	CHECK(codeplug_target_list(lr, TEST_KIND, &h, out, 2) == 3 && out[0].index == 1 && out[1].check == r1.check);
	CHECK(codeplug_target(out[0], TEST_KIND, &h, addr, 16) == -1 && codeplug_target(out[1], TEST_KIND, &h, addr, 16) == 5);
	CHECK(codeplug_target_list(lr, 0x81, &h, out, 2) == -1);

	// Traffic keys: several on one system, chosen by algorithm and key id
	uint8_t k1[5 + 4] = { 2, 0x84, 0x01, 0x00, 0, 0xA1, 0xA2, 0xA3, 0xA4 };
	uint8_t k2[5 + 4] = { 2, 0x84, 0x02, 0x00, 0, 0xB1, 0xB2, 0xB3, 0xB4 };
	uint8_t k3[5 + 2] = { 3, 0x84, 0x01, 0x00, 0, 0xC1, 0xC2 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 0, k1, sizeof k1));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 7, k2, sizeof k2));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 3, k3, sizeof k3));
	uint8_t key[8];
	CHECK(codeplug_traffic_key(2, 0x84, 2, key, sizeof key) == 4 && key[0] == 0xB1);
	CHECK(codeplug_traffic_key(2, 0x84, 1, key, sizeof key) == 4 && key[3] == 0xA4);
	CHECK(codeplug_traffic_key(3, 0x84, 1, key, sizeof key) == 2 && key[1] == 0xC2);
	CHECK(codeplug_traffic_key(2, 0x85, 1, key, sizeof key) == -1);
	CHECK(codeplug_traffic_key(4, 0x84, 1, key, sizeof key) == -1);
}

static void put_entry(uint8_t n, uint8_t sys, uint8_t cls, uint8_t group, uint8_t flags, const uint8_t *body, uint16_t len)
{
	uint8_t v[CODEPLUG_SYS_ENTRY_HDR + 32];
	v[0] = sys;
	v[1] = cls;
	v[2] = group;
	v[3] = flags;
	memcpy(&v[CODEPLUG_SYS_ENTRY_HDR], body, len);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), v, (uint16_t)(CODEPLUG_SYS_ENTRY_HDR + len)));
}

static void put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

// A system shaped like a wide-area trunked one: a network and system identity, two sites each with its own control
// channels, a channel plan table, its own unit ID, several keys chosen by key ID, a channel with a fallback
static void test_systems(void)
{
	fresh();
	uint8_t v[CODEPLUG_SYS_HDR + 16];
	memset(v, 0, sizeof v);
	v[0] = TEST_SYSTYPE;
	v[1] = CODEPLUG_SYS_NEEDS_KEY | CODEPLUG_SYS_TX | 0x10u;	// a type's bit kept as found
	v[2] = CODEPLUG_SHARE_IDENTITY;
	v[3] = 5;					// identity: network (3) and system (2)
	put_le32(&v[4], 0xABCDEFu);			// own ID on this system
	v[8] = 12;					// fallback channel 12
	v[10] = (uint8_t)codeplug_name_check("Fallback");
	v[11] = (uint8_t)(codeplug_name_check("Fallback") >> 8);
	memset(&v[12], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&v[12], "Wide area", 9);
	const uint8_t block[8] = { 0x01, 0x23, 0x0B, 0x45, 0x03, 0x77, 0x88, 0x99 };	// identity, then the type's
	memcpy(&v[CODEPLUG_SYS_HDR], block, sizeof block);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 3, v, CODEPLUG_SYS_HDR + sizeof block));
	codeplug_system_t sy;
	uint8_t b[16 + CODEPLUG_SYS_HDR];
	CHECK(codeplug_system(3, &sy, b, 16) == 8 && memcmp(b, block, 8) == 0);
	CHECK(sy.type == TEST_SYSTYPE && sy.id_len == 5 && sy.own_id == 0xABCDEFu && sy.fb_chan == 12);
	CHECK(sy.fb_check == codeplug_name_check("Fallback") && strcmp(sy.name, "Wide area") == 0);
	CHECK((sy.flags & CODEPLUG_SYS_TX) != 0 && sy.sharing == CODEPLUG_SHARE_IDENTITY);
	CHECK(codeplug_system(4, &sy, b, 16) == -1 && codeplug_system(CODEPLUG_SYSTEM_NONE, &sy, b, 16) == -1);
	// A zeroed header is no system; a header with only the type is a listen-only one, shared off
	memset(v, 0, CODEPLUG_SYS_HDR);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 4, v, CODEPLUG_SYS_HDR));
	CHECK(codeplug_system(4, &sy, b, 16) == -1);
	v[0] = TEST_SYSTYPE;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 4, v, CODEPLUG_SYS_HDR));
	CHECK(codeplug_system(4, &sy, b, 16) == 0 && (sy.flags & CODEPLUG_SYS_TX) == 0 && sy.sharing == CODEPLUG_SHARE_OFF);

	// Sites, their control channels (group = site), plan rows; entries stored out of order, another system's between
	const uint8_t site0[2] = { 1, 7 }, site1[2] = { 1, 9 };
	uint8_t ch[10];
	put_entry(9, 3, CODEPLUG_ENTRY_SITE, 1, 0, site1, 2);
	put_entry(2, 3, CODEPLUG_ENTRY_SITE, 0, 0, site0, 2);
	for (uint8_t i = 0; i < 5; i++) {
		memset(ch, 0, sizeof ch);
		put_le32(&ch[2], 851000000u + i * 12500u);
		put_le32(&ch[6], 806000000u + i * 12500u);
		put_entry((uint8_t)(20 - i), 3, CODEPLUG_ENTRY_CHANNEL, i < 3 ? 0 : 1, CODEPLUG_ENTRY_CONTROL, ch, 10);
	}
	put_entry(30, 4, CODEPLUG_ENTRY_CHANNEL, 0, CODEPLUG_ENTRY_CONTROL, ch, 10);
	const uint8_t plan[9] = { 0x10, 0x55, 0x55, 0x33, 0x00, 0x64, 0x00, 0x80, 0x02 };
	put_entry(40, 3, CODEPLUG_ENTRY_PLAN, 0, 0, plan, sizeof plan);
	put_entry(41, 3, CODEPLUG_ENTRY_PLAN, 2, 0x20, plan, sizeof plan);

	codeplug_sys_entry_t e;
	uint8_t body[16];
	int at = -1, n = 0, prev = -1, site1_ctl = 0;
	while (codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_CHANNEL, &e, body, sizeof body) >= 0) {
		CHECK(at > prev && e.len == 10 && (e.flags & CODEPLUG_ENTRY_CONTROL) != 0);
		prev = at;
		site1_ctl += e.group == 1;
		n++;
	}
	CHECK(n == 5 && site1_ctl == 2);
	at = -1;
	CHECK(codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_SITE, &e, body, sizeof body) == 2 && e.group == 0 && body[1] == 7);
	CHECK(codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_SITE, &e, body, sizeof body) == 9 && e.group == 1 && body[1] == 9);
	CHECK(codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_SITE, &e, body, sizeof body) == -1);
	at = -1;
	CHECK(codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_PLAN, &e, body, sizeof body) == 40 && e.len == 9 && body[8] == 2);
	CHECK(codeplug_sys_entry_next(&at, 3, CODEPLUG_ENTRY_PLAN, &e, body, sizeof body) == 41 && e.group == 2 && e.flags == 0x20);

	// The system's own secret and its traffic keys by key ID
	const uint8_t secret[4] = { 9, 8, 7, 6 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 3, secret, 4));
	uint8_t k[8];
	CHECK(codeplug_system_secret(3, k, sizeof k) == 4 && k[3] == 6 && codeplug_system_secret(4, k, sizeof k) == -1);
	uint8_t tk[CODEPLUG_TRAFFIC_HDR + 2] = { 3, 0x84, 0x34, 0x12, 0, 0xEE, 0xFF };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 1, tk, sizeof tk));
	CHECK(codeplug_traffic_key(3, 0x84, 0x1234, k, sizeof k) == 2 && k[0] == 0xEE);

	// A channel on the system with its own fallback
	channel_t c = make("Fallback", RADIO_MODE_FM, false);
	CHECK(codeplug_set_channel(12, &c));
	c = make("Talk", RADIO_MODE_DMR, true);
	CHECK(codeplug_set_channel(13, &c));
	codeplug_tag_t t = { .kind = TEST_KIND, .system = 3, .flags = CODEPLUG_TAG_FALLBACK, .fb_chan = 12,
			     .fb_check = codeplug_name_check("Fallback") };
	CHECK(codeplug_tag_write(13, "Talk", &t, NULL, 0));
	c = get(13);
	codeplug_tag_t r;
	CHECK(c.kind == TEST_KIND && c.system == 3 && codeplug_tag_read(13, &r) && r.fb_chan == 12);
	CHECK(r.fb_check == codeplug_name_check(get(12).name));
}

#if CONFIG_DMR_AES
static void test_aes_reference(void)
{
	fresh();
	channel_t c = make("AES", RADIO_MODE_DMR, false);
	c.kind = CODEPLUG_KIND_DMR_AES;
	c.system = 2;
	c.aes_ref = 256;
	CHECK(codeplug_set_channel(10, &c));
	c = get(10);
	CHECK(c.kind == CODEPLUG_KIND_DMR_AES && c.aes_ref == 256 && !c.rx_only);
	strcpy(c.name, "Renamed");
	c.aes_ref = 1;
	CHECK(codeplug_set_channel(10, &c));
	CHECK(get(10).aes_ref == 1);
	c.kind = CODEPLUG_KIND_NONE;
	CHECK(codeplug_set_channel(10, &c));
	CHECK(get(10).kind == CODEPLUG_KIND_NONE && !get(10).rx_only);
}
#endif

int main(void)
{
#if CONFIG_DMR_AES
	test_aes_reference();
#endif
	test_name_check();
	test_tag();
	test_live_rule();
	test_write_follows();
	test_rewrite();
	test_stale_and_delete();
	test_unknown_kind();
	test_full_area();
	test_records();
	test_systems();
	sim_flash_free();
	if (failures) {
		printf("codeplugTagTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("codeplugTagTest: all passed\n");
	return 0;
}
