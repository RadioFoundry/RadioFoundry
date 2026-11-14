// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tier III systems and trunked channels from the codeplug (core/radio/radio_trunk_cp.c, docs/spec/dmr-trunking.md 12)
// on a simulated NOR flash: a system record and its channels in the settings area, channels in key order whatever the
// area's order, options a codeplug may not set, absent, short and longer records; the key and when a system is usable;
// a channel's tag and its name check, its talkgroups; the fallback channel; and the debug link's DL_CMD_CPKV
// (core/debuglink/debuglink_cpkv.c) that writes them
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "storage/storage.h"
#include "radio/radio_trunk.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
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

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

#define SYS_LEN		64
#define AUTHENTICATES	0x0800
#define CHAN_LEN	13

static void contact(uint16_t i, const char *name, uint32_t id, uint8_t type)
{
	contact_t c;
	memset(&c, 0, sizeof c);
	strcpy(c.name, name);
	c.id = id;
	c.type = type;
	CHECK(codeplug_set_contact(i, &c));
}

// A system record as a codeplug tool writes it (docs/spec/dmr-trunking.md 12.1)
static void sys_rec(uint8_t *v, const char *name, uint16_t opts, uint16_t fb_chan, uint16_t fb_check)
{
	memset(v, 0, SYS_LEN);
	strncpy((char *)v, name, 16);
	v[16] = 1;		// small
	put16(&v[17], 5);
	v[19] = 1;		// A
	v[20] = 2;		// color code
	put16(&v[21], opts);
	put32(&v[23], 0);
	v[27] = 2;		// sharing: whole system
	v[28] = 45;		// fallback hold
	put16(&v[29], fb_chan);
	v[43] = (uint8_t)(int8_t)-95;
	v[44] = 3;
	v[45] = 2;
	put16(&v[46], 1);
	put16(&v[48], 2);
	put16(&v[62], fb_check);
}

// A codeplug channel; its tag (12.4) when sys is not 0xFF
static void ch_rec(uint16_t number, const char *name, uint16_t contact, uint16_t list, uint8_t sys, uint8_t flags,
		   uint16_t fb_chan, uint16_t fb_check)
{
	channel_t ch;
	memset(&ch, 0, sizeof ch);
	strcpy(ch.name, name);
	ch.rx_hz = ch.tx_hz = 461462500;
	ch.mode = RADIO_MODE_DMR;
	ch.rx_only = true;
	ch.color_code = 2;
	ch.timeslot = 1;
	ch.contact = contact;
	ch.listen_list = list;
	CHECK(codeplug_set_channel(number, &ch));
	if (sys != 0xFF) {
		uint8_t t[8] = { sys, flags };
		put16(&t[2], radio_t3_name_check(name));
		put16(&t[4], fb_chan);
		put16(&t[6], fb_check);
		CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_TAG + number - 1u), t, sizeof t));
	}
}

static void chan(uint16_t key, uint8_t sys, bool ctrl, uint16_t lcn, uint32_t rx, uint32_t tx)
{
	uint8_t r[CHAN_LEN];
	r[0] = sys;
	r[1] = ctrl ? 1 : 0;
	put16(&r[2], lcn);
	put32(&r[4], rx);
	put32(&r[8], tx);
	r[12] = 0xFF;
	CHECK(codeplug_kv_set(key, r, sizeof r));
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	contact(1, "TG 91", 91, CONTACT_GROUP);
	contact(2, "TG 92", 92, CONTACT_GROUP);
	contact(3, "Joe", 3100001, CONTACT_PRIVATE);
	contact(4, "All", 0, CONTACT_ALL_CALL);
	listen_list_t l;
	memset(&l, 0, sizeof l);
	strcpy(l.name, "Metro");
	l.count = 3;
	l.contacts[0] = 1;
	l.contacts[1] = 2;
	l.contacts[2] = 4;
	CHECK(codeplug_set_listen_list(1, &l));
}

static void test_load(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN];
	char name[CODEPLUG_NAME_LEN + 1];

	fresh();
	// T3_OPT_MONITOR (0x0008) is the debug link's: a codeplug can't set it
	sys_rec(v, "Metro", T3_OPT_PRIVATE | T3_OPT_NO_ROAM | T3_OPT_STUN | T3_OPT_MONITOR, 9, 0x1234);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	// Written out of key order, with another system's channel among them
	chan(CODEPLUG_KEY_T3_CHAN + 5, 0, true, 1, 461462500, 461087500);
	chan(CODEPLUG_KEY_T3_CHAN + 1, 0, false, 2, 461337500, 461237500);
	chan(CODEPLUG_KEY_T3_CHAN + 3, 1, true, 7, 451000000, 456000000);
	chan(CODEPLUG_KEY_T3_CHAN + 2, 0, true, 3, 461287500, 461137500);

	CHECK(radio_t3_name(0, name) && strcmp(name, "Metro") == 0);
	CHECK(radio_t3_load(0, &s));
	CHECK(s.cfg.auth.model == 1 && s.cfg.auth.net == 5 && s.cfg.auth.category == T3_PAR_A && s.cc == 2);
	CHECK(s.cfg.opts == (T3_OPT_PRIVATE | T3_OPT_NO_ROAM | T3_OPT_STUN) && s.sys == 0);
	CHECK(s.fb_hold_s == 45 && s.fb_chan == 9 && s.fb_check == 0x1234 && !s.cfg.auth_key_set);
	CHECK(s.cfg.own_id == 0 && s.cfg.leave_dbm == -95 && s.cfg.ps_rq == 3);
	CHECK(s.cfg.auth.nsites == 2 && s.cfg.auth.sites == s.cfg.sites && s.cfg.sites[0] == 1 && s.cfg.sites[1] == 2);
	CHECK(s.cfg.plan.learn && s.cfg.plan.fixed.base_hz == 0);
	// Key order: 0x0201 (LCN 2), 0x0202 (LCN 3), 0x0205 (LCN 1); the control channels 3 then 1
	CHECK(s.nchans == 3 && s.chans[0].lcn == 2 && s.chans[1].lcn == 3 && s.chans[2].lcn == 1);
	CHECK(s.cfg.plan.table == s.chans && s.cfg.plan.ntable == 3);
	CHECK(s.cfg.nctrl == 2 && s.cfg.ctrl[0].lcn == 3 && s.cfg.ctrl[1].lcn == 1);
	CHECK(s.cfg.ctrl[1].rx_hz == 461462500 && s.cfg.ctrl[1].tx_hz == 461087500 && s.cfg.ctrl[1].cc == T3_CC_ANY);
	CHECK(s.cfg.ntg == 0 && s.talk_id == 0);	// the talkgroups are a channel's
	CHECK(radio_t3_usable(0) && !radio_t3_usable(2));

	// The other system: its one channel, no contacts
	sys_rec(v, "Other", 0, 0, 0);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM + 1, v, sizeof v));
	CHECK(radio_t3_load(1, &s) && s.nchans == 1 && s.cfg.nctrl == 1 && s.chans[0].lcn == 7 && s.cfg.ntg == 0);
	CHECK(s.cfg.opts == 0);
	CHECK(sim_flash_violations() == 0);
}

#if CONFIG_DMR_TRUNK_SUPL
// The supplementary user data a system's radios offer (12.8): its format and octets; none without a record, or with
// one that has no octets; the debug link's staging is not the codeplug's
static void test_supl(void)
{
	uint8_t v[SYS_LEN];
	radio_t3_setup_t s;
	fresh();
	sys_rec(v, "Metro", T3_OPT_PRIVATE, 0, 0);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	chan(CODEPLUG_KEY_T3_CHAN, 0, true, 1, 461462500, 461087500);
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_len == 0);

	const uint8_t rec[] = { T3_UDTF_ISO8, 'U', 'n', 'i', 't', ' ', '7' };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SUPL, rec, sizeof rec));
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_format == T3_UDTF_ISO8 && s.cfg.supl_len == 6);
	CHECK(s.cfg.supl_data == s.supl && memcmp(s.supl, "Unit 7", 6) == 0);
	// another system's record isn't this one's
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM + 1, v, sizeof v));
	chan((uint16_t)(CODEPLUG_KEY_T3_CHAN + 1), 1, true, 2, 451000000, 456000000);
	CHECK(radio_t3_load(1, &s) && s.cfg.supl_len == 0);

	// at most 46 octets; a longer value is cut to them
	uint8_t big[1 + T3_UDT_MAX + 4];
	memset(big, 'x', sizeof big);
	big[0] = 0x10u | T3_UDTF_BINARY;		// the format's low four bits
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SUPL, big, sizeof big));
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_format == T3_UDTF_BINARY && s.cfg.supl_len == T3_UDT_MAX);
	// the format alone: none offered
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SUPL, rec, 1));
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_len == 0);
	CHECK(sim_flash_violations() == 0);
}
#endif

// A short data poll record longer than its buffer (12.7) is cut to it: the reply is never past T3_UDT_MAX octets,
// whatever length the record stores
static void test_poll_long(void)
{
	uint8_t v[SYS_LEN];
	radio_t3_setup_t s;
	fresh();
	sys_rec(v, "Metro", T3_OPT_PRIVATE, 0, 0);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	chan(CODEPLUG_KEY_T3_CHAN, 0, true, 1, 461462500, 461087500);
	uint8_t big[2 + T3_UDT_MAX + 20];
	memset(big, 'y', sizeof big);
	big[0] = T3_UDTF_ISO8;
	big[1] = 4;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_POLL, big, sizeof big));
	CHECK(radio_t3_load(0, &s) && s.cfg.poll_len == T3_UDT_MAX && s.poll_reply[T3_UDT_MAX - 1] == 'y');
}

// A trunked channel's talk target and talkgroups (12.4): its contact and RX group list
static void test_talk(void)
{
	radio_t3_setup_t s;
	channel_t ch;
	memset(&ch, 0, sizeof ch);

	fresh();
	radio_t3_reset(&s);
	ch.contact = 2;
	ch.listen_list = 1;
	radio_t3_talk(&ch, &s);
	CHECK(s.cfg.ntg == 2 && s.cfg.tg[0].id == 91 && s.cfg.tg[0].flags == T3_TG_LISTEN);
	CHECK(s.cfg.tg[1].id == 92 && s.cfg.tg[1].flags == (T3_TG_LISTEN | T3_TG_TALK));
	CHECK(s.talk_id == 0 && (s.cfg.opts & T3_OPT_ALLCALL) != 0);	// all-call from the list's contact
	ch.contact = 3;			// PTT calls a private contact
	radio_t3_talk(&ch, &s);
	CHECK(s.talk_id == 3100001 && s.talk_private);
	CHECK(s.cfg.ntg == 2 && s.cfg.tg[0].flags == T3_TG_LISTEN && s.cfg.tg[1].flags == T3_TG_LISTEN);
	ch.contact = 4;			// the all-call contact, no list
	ch.listen_list = 0;
	radio_t3_talk(&ch, &s);
	CHECK(s.talk_id == 0xFFFFFF && !s.talk_private && s.cfg.ntg == 0);
	ch.contact = 0;			// no contact: the list's first talkgroup
	ch.listen_list = 1;
	radio_t3_talk(&ch, &s);
	CHECK(s.cfg.tg[0].id == 91 && (s.cfg.tg[0].flags & T3_TG_TALK) != 0 && (s.cfg.tg[1].flags & T3_TG_TALK) == 0);
	ch.contact = 9;			// a contact and a list that aren't there
	ch.listen_list = 7;
	radio_t3_talk(&ch, &s);
	CHECK(s.cfg.ntg == 0 && s.talk_id == 0);
}

// The key (12.3) and when a system is usable: a control channel, and a key when it authenticates
static void test_key(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN], k[19];

	fresh();
	sys_rec(v, "Auth", AUTHENTICATES, 0, 0);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	chan(CODEPLUG_KEY_T3_CHAN, 0, true, 1, 461462500, 461087500);
	CHECK(!radio_t3_usable(0) && !radio_t3_load(0, &s));		// it authenticates: no key, not usable
	for (int i = 0; i < 16; i++) {
		k[i] = (uint8_t)(0xA0 + i);
	}
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_KEY, k, 16));		// K, no PSN
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s) && s.cfg.auth_key_set && s.cfg.auth_key[0] == 0xA0 &&
	      s.cfg.auth_key[15] == 0xAF && s.cfg.psn == 0);
	CHECK((s.cfg.opts & AUTHENTICATES) == 0);			// the codeplug's flag, not an engine option
	k[16] = 0x12;
	k[17] = 0x34;
	k[18] = 0x56;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_KEY, k, sizeof k));
	CHECK(radio_t3_load(0, &s) && s.cfg.psn == 0x123456);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_KEY, k, 15));		// too short: no key
	CHECK(!radio_t3_usable(0) && !radio_t3_load(0, &s));
	sys_rec(v, "Open", 0, 0, 0);					// doesn't authenticate: usable, a key still taken
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s) && !s.cfg.auth_key_set);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_KEY, k, sizeof k));
	CHECK(radio_t3_load(0, &s) && s.cfg.auth_key_set);
}

// A channel's tag (12.4): read when its name check matches; a renamed or renumbered channel loses it
static void test_tag(void)
{
	radio_t3_tag_t t;

	fresh();
	CHECK(radio_t3_name_check("Ops") != radio_t3_name_check("Ops2") &&
	      radio_t3_name_check("Ops") == radio_t3_name_check("Ops"));
	ch_rec(5, "Ops", 2, 1, 3, 0x01, 7, 0xBEEF);
	ch_rec(6, "Plain", 0, 0, 0xFF, 0, 0, 0);
	CHECK(radio_t3_tag(5, "Ops", &t) && t.sys == 3 && t.flags == 0x01 && t.fb_chan == 7 && t.fb_check == 0xBEEF);
	CHECK(!radio_t3_tag(5, "Ops2", &t));				// renamed, or another channel at 5
	CHECK(!radio_t3_tag(6, "Plain", &t));				// no tag
	CHECK(!radio_t3_tag(0, "", &t) && !radio_t3_tag(CODEPLUG_CHANNELS_MAX + 1, "Ops", &t));
	uint8_t bad[8] = { RADIO_T3_SYSTEMS };				// a system past those read
	put16(&bad[2], radio_t3_name_check("Plain"));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_TAG + 5, bad, sizeof bad));
	CHECK(!radio_t3_tag(6, "Plain", &t));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_TAG + 5, bad, 7));	// too short
	CHECK(!radio_t3_tag(6, "Plain", &t));
	// The last channel number has a key
	ch_rec(CODEPLUG_CHANNELS_MAX, "Last", 0, 0, 1, 0, 0, 0);
	CHECK(radio_t3_tag(CODEPLUG_CHANNELS_MAX, "Last", &t) && t.sys == 1);
}

// The fallback channel (12.5): the channel's own, else the system's; none when set so, gone, renamed or trunked
static void test_fallback(void)
{
	radio_t3_setup_t s;
	radio_t3_tag_t t;
	channel_t out;

	fresh();
	radio_t3_reset(&s);
	ch_rec(10, "Sys FB", 0, 0, 0xFF, 0, 0, 0);
	ch_rec(11, "Own FB", 0, 0, 0xFF, 0, 0, 0);
	ch_rec(12, "Trunked", 0, 0, 0, 0, 0, 0);
	s.fb_chan = 10;
	s.fb_check = radio_t3_name_check("Sys FB");
	t = (radio_t3_tag_t){ 0, 0, 0, 0 };
	CHECK(radio_t3_fallback(&t, &s, &out) == 10 && strcmp(out.name, "Sys FB") == 0);
	t.fb_chan = 11;
	t.fb_check = radio_t3_name_check("Own FB");
	CHECK(radio_t3_fallback(&t, &s, &out) == 11);			// the channel's own first
	t.flags = RADIO_T3_TAG_NO_FALLBACK;
	CHECK(radio_t3_fallback(&t, &s, &out) == 0);
	t.flags = 0;
	t.fb_check ^= 1;						// renamed or renumbered
	CHECK(radio_t3_fallback(&t, &s, &out) == 0);
	t.fb_chan = 12;							// a trunked channel isn't a fallback
	t.fb_check = radio_t3_name_check("Trunked");
	CHECK(radio_t3_fallback(&t, &s, &out) == 0);
	t.fb_chan = 40;							// gone
	CHECK(radio_t3_fallback(&t, &s, &out) == 0);
	t.fb_chan = 0;
	s.fb_chan = 0;
	CHECK(radio_t3_fallback(&t, &s, &out) == 0);			// none set
}

static void test_refused(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN + 8];
	char name[CODEPLUG_NAME_LEN + 1];

	fresh();
	CHECK(!radio_t3_load(0, &s) && s.nchans == 0 && !radio_t3_name(0, name) && name[0] == '\0');
	sys_rec(v, "NoCtrl", 0, 0, 0);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, SYS_LEN));
	chan(CODEPLUG_KEY_T3_CHAN, 0, false, 2, 461337500, 461237500);
	CHECK(!radio_t3_load(0, &s));			// no control channel
	CHECK(radio_t3_name(0, name) && strcmp(name, "NoCtrl") == 0);
	chan(CODEPLUG_KEY_T3_CHAN + 1, 0, true, 1, 461462500, 461087500);
	CHECK(radio_t3_load(0, &s));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, SYS_LEN - 1));
	CHECK(!radio_t3_load(0, &s) && !radio_t3_name(0, name));	// shorter than the fields read
	memset(v + SYS_LEN, 0xA5, 8);			// a later writer's appended fields
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM, v, sizeof v));
	CHECK(radio_t3_load(0, &s) && s.cfg.auth.net == 5);
	uint8_t shortc[CHAN_LEN - 1] = { 0, 1, 9, 0 };	// a channel record too short: skipped
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_CHAN + 0x10, shortc, sizeof shortc));
	CHECK(radio_t3_load(0, &s) && s.nchans == 2);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SYSTEM + RADIO_T3_SYSTEMS, v, sizeof v));
	CHECK(!radio_t3_load(RADIO_T3_SYSTEMS, &s));	// past the systems the radio reads
	// More channels than the table holds: the first RADIO_T3_CHANS by key, control channels to T3_CTRL_MAX
	for (uint16_t i = 0; i < 12; i++) {
		chan((uint16_t)(CODEPLUG_KEY_T3_CHAN + 0x20 + i), 0, true, (uint16_t)(10 + i), 461000000 + i, 462000000 + i);
	}
	CHECK(radio_t3_load(0, &s) && s.nchans == RADIO_T3_CHANS && s.cfg.nctrl == T3_CTRL_MAX);
	CHECK(s.chans[0].lcn == 2 && s.chans[1].lcn == 1 && s.chans[2].lcn == 10 && s.cfg.ctrl[1].lcn == 10);
}

static size_t cpkv(uint8_t op, uint16_t key, uint16_t off, const void *data, uint8_t n, uint8_t *out)
{
	uint8_t req[64];
	req[0] = op;
	put16(&req[1], key);
	put16(&req[3], off);
	memcpy(&req[5], data, n);
	return debuglink_cmd_cpkv(req, (uint16_t)(5u + n), out, 256);
}

static void test_debuglink(void)
{
	uint8_t out[256], v[SYS_LEN], back[SYS_LEN];
	radio_t3_setup_t s;

	fresh();
	sys_rec(v, "Linked", 0, 0, 0);
	// A record longer than one request goes in two: the first part, then a patch from where it ended
	CHECK(cpkv(DL_CPKV_SET, CODEPLUG_KEY_T3_SYSTEM, 0, v, 40, out) == 1 && out[0] == DL_ST_OK);
	CHECK(cpkv(DL_CPKV_SET, CODEPLUG_KEY_T3_SYSTEM, 40, v + 40, SYS_LEN - 40, out) == 1 && out[0] == DL_ST_OK);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_T3_SYSTEM, back, sizeof back) == SYS_LEN && memcmp(back, v, SYS_LEN) == 0);
	CHECK(cpkv(DL_CPKV_GET, CODEPLUG_KEY_T3_SYSTEM, 16, NULL, 0, out) == 3u + SYS_LEN - 16u && out[0] == DL_ST_OK &&
	      out[1] == SYS_LEN && out[2] == 0 && memcmp(out + 3, v + 16, SYS_LEN - 16) == 0);
	// A patch inside the value keeps the rest; one past its end is refused
	uint8_t cc = 7;
	CHECK(cpkv(DL_CPKV_SET, CODEPLUG_KEY_T3_SYSTEM, 20, &cc, 1, out) == 1 && out[0] == DL_ST_OK);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_T3_SYSTEM, back, sizeof back) == SYS_LEN && back[20] == 7 && back[45] == 2);
	CHECK(cpkv(DL_CPKV_SET, CODEPLUG_KEY_T3_SYSTEM, SYS_LEN + 1, &cc, 1, out) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cpkv(DL_CPKV_SET, 0x7F00, 4, &cc, 1, out) == 1 && out[0] == DL_ST_BAD_ARG);	// a patch of no value
	// The channel and the load over the link (DL_T3_LOAD's call)
	uint8_t r[CHAN_LEN] = { 0, 1 };
	put16(&r[2], 1);
	put32(&r[4], 461462500);
	put32(&r[8], 461087500);
	r[12] = 0xFF;
	CHECK(cpkv(DL_CPKV_SET, CODEPLUG_KEY_T3_CHAN, 0, r, sizeof r, out) == 1 && out[0] == DL_ST_OK);
	CHECK(radio_t3_load(0, &s) && s.cc == 7 && s.cfg.nctrl == 1);
	CHECK(cpkv(DL_CPKV_REMOVE, CODEPLUG_KEY_T3_CHAN, 0, NULL, 0, out) == 1 && out[0] == DL_ST_OK);
	CHECK(!radio_t3_load(0, &s));
	CHECK(cpkv(DL_CPKV_GET, CODEPLUG_KEY_T3_CHAN, 0, NULL, 0, out) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(cpkv(9, CODEPLUG_KEY_T3_CHAN, 0, NULL, 0, out) == 1 && out[0] == DL_ST_BAD_ARG);
}

int main(void)
{
	test_load();
#if CONFIG_DMR_TRUNK_SUPL
	test_supl();
#endif
	test_poll_long();
	test_talk();
	test_key();
	test_tag();
	test_fallback();
	test_refused();
	test_debuglink();
	if (failures) {
		printf("t3CodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("t3CodeplugTest: all passed\n");
	return 0;
}
