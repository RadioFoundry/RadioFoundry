// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tier III systems and trunked channels from the codeplug (core/radio/radio_trunk_cp.c,
// docs/spec/dmr-trunking-codeplug.md) on a simulated NOR flash: a system record (type 1) and its channel entries in the
// settings area, entries in number order whatever the area's order, options a codeplug may not set, the header's
// transmit and needs-key flags, absent, short and longer records and records of another type; the key and when a
// system is usable; a channel's tag (kind 1), its talkgroups; the fallback channel; and the debug link's DL_CMD_CPKV
// (core/debuglink/debuglink_cpkv.c) that writes them
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
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

#define BLOCK		CODEPLUG_SYS_HDR	// where the type's block starts in a system record
#define BLOCK_MIN	23			// the block up to the site count
#define SYS_LEN		(BLOCK + BLOCK_MIN + 16)
#define CHAN_LEN	(CODEPLUG_SYS_ENTRY_HDR + 11)
#define TX		CODEPLUG_SYS_TX
#define NEEDS_KEY	CODEPLUG_SYS_NEEDS_KEY
#define SYS(n)		((uint16_t)(CODEPLUG_KEY_SYSTEM + (n)))
#define ENTRY(n)	((uint16_t)(CODEPLUG_KEY_SYS_CHAN + (n)))

static void contact(uint16_t i, const char *name, uint32_t id, uint8_t type)
{
	contact_t c;
	memset(&c, 0, sizeof c);
	strcpy(c.name, name);
	c.id = id;
	c.type = type;
	CHECK(codeplug_set_contact(i, &c));
}

// A system record as a codeplug tool writes it (dmr-trunking-codeplug.md 1): the generic header with these flags, then
// the type's block
static void sys_rec(uint8_t *v, const char *name, uint8_t flags, uint16_t opts, uint16_t fb_chan, uint16_t fb_check)
{
	uint8_t *b = &v[BLOCK];
	memset(v, 0, SYS_LEN);
	v[0] = CODEPLUG_SYSTYPE_DMR_TRUNK;
	v[1] = flags;
	v[2] = CODEPLUG_SHARE_WHOLE;
	v[3] = 4;		// the identity: model, NET, category
	put32(&v[4], 0);	// own ID: the radio's
	put16(&v[8], fb_chan);
	put16(&v[10], fb_check);
	strncpy((char *)&v[12], name, 16);
	b[0] = 1;		// small
	put16(&b[1], 5);
	b[3] = 1;		// A
	b[4] = 2;		// color code
	put16(&b[5], opts);
	b[7] = 45;		// fallback hold
	b[20] = (uint8_t)(int8_t)-95;
	b[21] = 3;
	b[22] = 2;
	put16(&b[23], 1);
	put16(&b[25], 2);
}

// A codeplug channel: the base record (DMR, receive only) and, when sys >= 0, its tag (4) with these flags and, with
// CODEPLUG_TAG_FALLBACK, this fallback
static void ch_rec(uint16_t number, const char *name, uint16_t contact, uint16_t list, int sys, uint8_t flags,
		   uint16_t fb_chan, uint16_t fb_check)
{
	channel_t ch;
	memset(&ch, 0, sizeof ch);
	strcpy(ch.name, name);
	ch.rx_hz = ch.tx_hz = 450012500;
	ch.mode = RADIO_MODE_DMR;
	ch.rx_only = true;
	ch.color_code = 2;
	ch.timeslot = 1;
	ch.contact = contact;
	ch.listen_list = list;
	CHECK(codeplug_set_channel(number, &ch));
	if (sys >= 0) {
		codeplug_tag_t t = { .kind = CODEPLUG_KIND_DMR_TRUNK, .system = (uint8_t)sys, .flags = flags,
				     .fb_chan = fb_chan, .fb_check = fb_check };
		CHECK(codeplug_tag_write(number, name, &t, NULL, 0));
	}
}

// A system channel (2): an entry of class 0 for the whole system
static void chan(uint16_t key, uint8_t sys, bool ctrl, uint16_t lcn, uint32_t rx, uint32_t tx)
{
	uint8_t r[CHAN_LEN];
	r[0] = sys;
	r[1] = CODEPLUG_ENTRY_CHANNEL;
	r[2] = CODEPLUG_GROUP_NONE;
	r[3] = ctrl ? CODEPLUG_ENTRY_CONTROL : 0;
	put16(&r[4], lcn);
	put32(&r[6], rx);
	put32(&r[10], tx);
	r[14] = 0xFF;
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
	// T3_OPT_MONITOR (0x0008) is the debug link's and 0x0001 is reserved (listen only is the header's flag): a
	// codeplug's options set neither
	sys_rec(v, "Metro", TX, T3_OPT_PRIVATE | T3_OPT_NO_ROAM | T3_OPT_STUN | T3_OPT_MONITOR | 0x0001, 9, 0x1234);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	// Written out of entry order, with another system's channel among them
	chan(ENTRY(5), 0, true, 1, 450012500, 455012500);
	chan(ENTRY(1), 0, false, 2, 461337500, 461237500);
	chan(ENTRY(3), 1, true, 7, 451000000, 456000000);
	chan(ENTRY(2), 0, true, 3, 461287500, 461137500);

	CHECK(radio_t3_name(0, name) && strcmp(name, "Metro") == 0);
	CHECK(radio_t3_load(0, &s));
	CHECK(s.cfg.auth.model == 1 && s.cfg.auth.net == 5 && s.cfg.auth.category == T3_PAR_A && s.cc == 2);
	CHECK(s.cfg.opts == (T3_OPT_PRIVATE | T3_OPT_NO_ROAM | T3_OPT_STUN) && s.sys == 0 && !s.ip_gw);
	CHECK(s.fb_hold_s == 45 && s.fb_chan == 9 && s.fb_check == 0x1234 && !s.cfg.auth_key_set);
	CHECK(s.cfg.own_id == 0 && s.cfg.leave_dbm == -95 && s.cfg.ps_rq == 3);
	CHECK(s.cfg.auth.nsites == 2 && s.cfg.auth.sites == s.cfg.sites && s.cfg.sites[0] == 1 && s.cfg.sites[1] == 2);
	CHECK(s.cfg.plan.learn && s.cfg.plan.fixed.base_hz == 0);
	// Entry order: 1 (LCN 2), 2 (LCN 3), 5 (LCN 1); the control channels 3 then 1
	CHECK(s.nchans == 3 && s.chans[0].lcn == 2 && s.chans[1].lcn == 3 && s.chans[2].lcn == 1);
	CHECK(s.cfg.plan.table == s.chans && s.cfg.plan.ntable == 3);
	CHECK(s.cfg.nctrl == 2 && s.cfg.ctrl[0].lcn == 3 && s.cfg.ctrl[1].lcn == 1);
	CHECK(s.cfg.ctrl[1].rx_hz == 450012500 && s.cfg.ctrl[1].tx_hz == 455012500 && s.cfg.ctrl[1].cc == T3_CC_ANY);
	CHECK(s.cfg.ntg == 0 && s.talk_id == 0);	// the talkgroups are a channel's
	CHECK(radio_t3_usable(0) && !radio_t3_usable(2));

	// The other system: its one channel, no contacts. No transmit flag in its header: listen only, whatever its
	// options say; its own ID and the IP gateway option
	sys_rec(v, "Other", 0, 0x1000, 0, 0);
	put32(&v[4], 3100077);
	CHECK(codeplug_kv_set(SYS(1), v, sizeof v));
	CHECK(radio_t3_load(1, &s) && s.nchans == 1 && s.cfg.nctrl == 1 && s.chans[0].lcn == 7 && s.cfg.ntg == 0);
	CHECK(s.cfg.opts == T3_OPT_LISTEN_ONLY && s.cfg.own_id == 3100077 && s.ip_gw);
	// An ID DMR can't carry reads as the radio's
	put32(&v[4], 0x01000000);
	CHECK(codeplug_kv_set(SYS(1), v, sizeof v));
	CHECK(radio_t3_load(1, &s) && s.cfg.own_id == 0);
	CHECK(sim_flash_violations() == 0);
}

#if CONFIG_DMR_TRUNK_SUPL
// The supplementary user data a system's radios offer (5): its format and octets; none without a record, or with
// one that has no octets; the debug link's staging is not the codeplug's
static void test_supl(void)
{
	uint8_t v[SYS_LEN];
	radio_t3_setup_t s;
	fresh();
	sys_rec(v, "Metro", TX, T3_OPT_PRIVATE, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	chan(ENTRY(0), 0, true, 1, 450012500, 455012500);
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_len == 0);

	const uint8_t rec[] = { T3_UDTF_ISO8, 'U', 'n', 'i', 't', ' ', '7' };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_SUPL, rec, sizeof rec));
	CHECK(radio_t3_load(0, &s) && s.cfg.supl_format == T3_UDTF_ISO8 && s.cfg.supl_len == 6);
	CHECK(s.cfg.supl_data == s.supl && memcmp(s.supl, "Unit 7", 6) == 0);
	// another system's record isn't this one's
	CHECK(codeplug_kv_set(SYS(1), v, sizeof v));
	chan(ENTRY(1), 1, true, 2, 451000000, 456000000);
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

// A short data poll record longer than its buffer (5) is cut to it: the reply is never past T3_UDT_MAX octets,
// whatever length the record stores
static void test_poll_long(void)
{
	uint8_t v[SYS_LEN];
	radio_t3_setup_t s;
	fresh();
	sys_rec(v, "Metro", TX, T3_OPT_PRIVATE, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	chan(ENTRY(0), 0, true, 1, 450012500, 455012500);
	uint8_t big[2 + T3_UDT_MAX + 20];
	memset(big, 'y', sizeof big);
	big[0] = T3_UDTF_ISO8;
	big[1] = 4;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_T3_POLL, big, sizeof big));
	CHECK(radio_t3_load(0, &s) && s.cfg.poll_len == T3_UDT_MAX && s.poll_reply[T3_UDT_MAX - 1] == 'y');
}

// A trunked channel's talk target and talkgroups (4): its contact and RX group list
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

	// All-call from a channel's RX list goes with the channel; the system record's own stays
	ch.contact = 1;
	ch.listen_list = 1;
	radio_t3_talk(&ch, &s);
	CHECK((s.cfg.opts & T3_OPT_ALLCALL) != 0);
	ch.listen_list = 0;
	radio_t3_talk(&ch, &s);
	CHECK((s.cfg.opts & T3_OPT_ALLCALL) == 0);
	s.allcall = true;
	radio_t3_talk(&ch, &s);
	CHECK((s.cfg.opts & T3_OPT_ALLCALL) != 0);
	s.allcall = false;

	// A full RX list: the talkgroup PTT calls still goes in, and talks
	listen_list_t l;
	memset(&l, 0, sizeof l);
	strcpy(l.name, "Full");
	l.count = CODEPLUG_LIST_LEN;
	for (uint16_t i = 0; i < CODEPLUG_LIST_LEN; i++) {
		contact((uint16_t)(10 + i), "G", 1000u + i, CONTACT_GROUP);
		l.contacts[i] = (uint16_t)(10 + i);
	}
	CHECK(codeplug_set_listen_list(2, &l));
	contact(10 + CODEPLUG_LIST_LEN, "Talk", 5000, CONTACT_GROUP);
	ch.contact = 10 + CODEPLUG_LIST_LEN;
	ch.listen_list = 2;
	radio_t3_talk(&ch, &s);
	CHECK(s.cfg.ntg == T3_TG_MAX && s.cfg.tg[T3_TG_MAX - 1].id == 5000 &&
	      (s.cfg.tg[T3_TG_MAX - 1].flags & T3_TG_TALK) != 0 && (s.cfg.tg[0].flags & T3_TG_TALK) == 0);
}

// A control channel past the channels the radio takes (RADIO_T3_CHANS) is never hunted on: not usable, as the load
static void test_chan_limit(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN];

	fresh();
	sys_rec(v, "Many", TX, 0, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	for (uint16_t i = 0; i < RADIO_T3_CHANS; i++) {
		chan(ENTRY(i), 0, false, (uint16_t)(10 + i), 461000000 + i, 462000000 + i);
	}
	chan(ENTRY(RADIO_T3_CHANS), 0, true, 1, 450012500, 455012500);
	CHECK(!radio_t3_usable(0) && !radio_t3_load(0, &s));
	chan(ENTRY(0), 0, true, 10, 461000000, 462000000);	// one among them
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s));
}

// The key (3) and when a system is usable: a control channel, and a key when its header says it needs one
static void test_key(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN], k[19];

	fresh();
	sys_rec(v, "Auth", TX | NEEDS_KEY, 0, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	chan(ENTRY(0), 0, true, 1, 450012500, 455012500);
	CHECK(!radio_t3_usable(0) && !radio_t3_load(0, &s));		// it authenticates: no key, not usable
	for (int i = 0; i < 16; i++) {
		k[i] = (uint8_t)(0xA0 + i);
	}
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY, k, 16));		// K, no PSN
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s) && s.cfg.auth_key_set && s.cfg.auth_key[0] == 0xA0 &&
	      s.cfg.auth_key[15] == 0xAF && s.cfg.psn == 0);
	CHECK(s.cfg.opts == 0);						// the header's flags are no engine options
	k[16] = 0x12;
	k[17] = 0x34;
	k[18] = 0x56;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY, k, sizeof k));
	CHECK(radio_t3_load(0, &s) && s.cfg.psn == 0x123456);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY, k, 15));		// too short: no key
	CHECK(!radio_t3_usable(0) && !radio_t3_load(0, &s));
	sys_rec(v, "Open", TX, 0, 0, 0);				// needs none: usable, a key still taken
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s) && !s.cfg.auth_key_set);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY, k, sizeof k));
	CHECK(radio_t3_load(0, &s) && s.cfg.auth_key_set);
	// The old place of the flag, option 0x0800, is reserved: it asks for no key
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYS_KEY));
	sys_rec(v, "Open", TX, 0x0800, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	CHECK(radio_t3_usable(0) && radio_t3_load(0, &s) && s.cfg.opts == 0);
}

// A channel's tag (4): codeplug_channel gives the kind, the system and transmit allowed; a channel that names no system
// or one the radio doesn't run is a trunked channel still, on a system never usable
static void test_tag(void)
{
	channel_t ch;

	fresh();
	ch_rec(5, "Ops", 2, 1, 3, CODEPLUG_TAG_TX, 0, 0);
	ch_rec(6, "Plain", 0, 0, -1, 0, 0, 0);
	ch_rec(7, "Listen", 2, 1, 0, 0, 0, 0);				// a zeroed flags byte: receive only
	ch_rec(8, "Nowhere", 2, 1, CODEPLUG_SYSTEM_NONE, CODEPLUG_TAG_TX, 0, 0);
	CHECK(codeplug_channel(5, &ch) && ch.kind == CODEPLUG_KIND_DMR_TRUNK && ch.system == 3 && !ch.rx_only &&
	      ch.contact == 2 && ch.listen_list == 1);
	CHECK(codeplug_channel(6, &ch) && ch.kind == CODEPLUG_KIND_NONE && ch.rx_only);
	CHECK(codeplug_channel(7, &ch) && ch.kind == CODEPLUG_KIND_DMR_TRUNK && ch.system == 0 && ch.rx_only);
	CHECK(codeplug_channel(8, &ch) && ch.kind == CODEPLUG_KIND_DMR_TRUNK && ch.system == CODEPLUG_SYSTEM_NONE);
	CHECK(!radio_t3_usable(3) && !radio_t3_usable(CODEPLUG_SYSTEM_NONE));
	// The last channel number has a key
	ch_rec(CODEPLUG_CHANNELS_MAX, "Last", 0, 0, 1, CODEPLUG_TAG_TX, 0, 0);
	CHECK(codeplug_channel(CODEPLUG_CHANNELS_MAX, &ch) && ch.kind == CODEPLUG_KIND_DMR_TRUNK && ch.system == 1);
}

// The fallback channel (dmr-trunking.md 12): the channel's own, else the system's; none when the tag says so, when it
// is gone, renamed or a channel of a kind
static void test_fallback(void)
{
	radio_t3_setup_t s;
	channel_t out;
	const uint8_t own = CODEPLUG_TAG_TX | CODEPLUG_TAG_FALLBACK;

	fresh();
	radio_t3_reset(&s);
	ch_rec(10, "Sys FB", 0, 0, -1, 0, 0, 0);
	ch_rec(11, "Own FB", 0, 0, -1, 0, 0, 0);
	ch_rec(12, "Trunked", 0, 0, 0, CODEPLUG_TAG_TX, 0, 0);
	s.fb_chan = 10;
	s.fb_check = codeplug_name_check("Sys FB");
	ch_rec(20, "T sys", 0, 0, 0, CODEPLUG_TAG_TX, 0, 0);
	CHECK(radio_t3_fallback(20, &s, &out) == 10 && strcmp(out.name, "Sys FB") == 0);
	ch_rec(21, "T own", 0, 0, 0, own, 11, codeplug_name_check("Own FB"));
	CHECK(radio_t3_fallback(21, &s, &out) == 11);			// the channel's own first
	ch_rec(22, "T never", 0, 0, 0, own | RADIO_T3_TAG_NO_FALLBACK, 11, codeplug_name_check("Own FB"));
	CHECK(radio_t3_fallback(22, &s, &out) == 0);
	ch_rec(23, "T none", 0, 0, 0, own, 0, 0);			// its own, set to none: not the system's
	CHECK(radio_t3_fallback(23, &s, &out) == 0);
	ch_rec(24, "T stale", 0, 0, 0, own, 11, (uint16_t)(codeplug_name_check("Own FB") ^ 1u));
	CHECK(radio_t3_fallback(24, &s, &out) == 0);			// renamed or renumbered
	ch_rec(25, "T trunk", 0, 0, 0, own, 12, codeplug_name_check("Trunked"));
	CHECK(radio_t3_fallback(25, &s, &out) == 0);			// a trunked channel isn't a fallback
	ch_rec(26, "T gone", 0, 0, 0, own, 40, 0x1111);
	CHECK(radio_t3_fallback(26, &s, &out) == 0);
	s.fb_check ^= 1u;						// the system's, renamed
	CHECK(radio_t3_fallback(20, &s, &out) == 0);
	s.fb_chan = 0;
	CHECK(radio_t3_fallback(20, &s, &out) == 0);			// none set
}

static void test_refused(void)
{
	radio_t3_setup_t s;
	uint8_t v[SYS_LEN + 8];
	char name[CODEPLUG_NAME_LEN + 1];

	fresh();
	CHECK(!radio_t3_load(0, &s) && s.nchans == 0 && !radio_t3_name(0, name) && name[0] == '\0');
	sys_rec(v, "NoCtrl", TX, 0, 0, 0);
	CHECK(codeplug_kv_set(SYS(0), v, SYS_LEN));
	chan(ENTRY(0), 0, false, 2, 461337500, 461237500);
	CHECK(!radio_t3_load(0, &s) && !radio_t3_usable(0));	// no control channel
	CHECK(radio_t3_name(0, name) && strcmp(name, "NoCtrl") == 0);
	chan(ENTRY(1), 0, true, 1, 450012500, 455012500);
	CHECK(radio_t3_load(0, &s) && radio_t3_usable(0));
	// A block shorter than the fields read: named, not usable. One with no site values names no sites
	CHECK(codeplug_kv_set(SYS(0), v, BLOCK + BLOCK_MIN - 1));
	CHECK(!radio_t3_load(0, &s) && !radio_t3_usable(0) && radio_t3_name(0, name));
	CHECK(codeplug_kv_set(SYS(0), v, BLOCK + BLOCK_MIN + 2));
	CHECK(radio_t3_load(0, &s) && s.cfg.auth.nsites == 1 && s.cfg.sites[0] == 1);
	CHECK(codeplug_kv_set(SYS(0), v, BLOCK - 1));		// no whole header: no system
	CHECK(!radio_t3_load(0, &s) && !radio_t3_name(0, name));
	memset(v + SYS_LEN, 0xA5, 8);			// a later writer's appended fields
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	CHECK(radio_t3_load(0, &s) && s.cfg.auth.net == 5 && s.cfg.auth.nsites == 2);
	// A system of another type at the number is no Tier III system
	v[0] = CODEPLUG_SYSTYPE_P25_TRUNK;
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	CHECK(!radio_t3_load(0, &s) && !radio_t3_usable(0) && !radio_t3_name(0, name));
	v[0] = CODEPLUG_SYSTYPE_DMR_TRUNK;
	CHECK(codeplug_kv_set(SYS(0), v, sizeof v));
	uint8_t shortc[CHAN_LEN - 1] = { 0, CODEPLUG_ENTRY_CHANNEL, CODEPLUG_GROUP_NONE, CODEPLUG_ENTRY_CONTROL, 9, 0 };
	CHECK(codeplug_kv_set(ENTRY(0x10), shortc, sizeof shortc));	// a channel entry too short: skipped
	// An entry of another class isn't a channel
	uint8_t site[CHAN_LEN] = { 0, CODEPLUG_ENTRY_SITE, 0, CODEPLUG_ENTRY_CONTROL, 8, 0 };
	CHECK(codeplug_kv_set(ENTRY(0x11), site, sizeof site));
	CHECK(radio_t3_load(0, &s) && s.nchans == 2);
	CHECK(codeplug_kv_set(SYS(RADIO_T3_SYSTEMS), v, sizeof v));
	CHECK(!radio_t3_load(RADIO_T3_SYSTEMS, &s) && !radio_t3_usable(RADIO_T3_SYSTEMS));	// past the systems the radio runs
	// More channels than the table holds: the first RADIO_T3_CHANS by entry, control channels to T3_CTRL_MAX
	for (uint16_t i = 0; i < 12; i++) {
		chan(ENTRY(0x20 + i), 0, true, (uint16_t)(10 + i), 461000000 + i, 462000000 + i);
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
	if (n != 0) {
		memcpy(&req[5], data, n);	// data may be NULL with n 0 (a read)
	}
	return debuglink_cmd_cpkv(req, (uint16_t)(5u + n), out, 256);
}

static void test_debuglink(void)
{
	uint8_t out[256], v[SYS_LEN], back[SYS_LEN];
	radio_t3_setup_t s;

	fresh();
	sys_rec(v, "Linked", TX, 0, 0, 0);
	// A record longer than one request goes in two: the first part, then a patch from where it ended
	CHECK(cpkv(DL_CPKV_SET, SYS(0), 0, v, 40, out) == 1 && out[0] == DL_ST_OK);
	CHECK(cpkv(DL_CPKV_SET, SYS(0), 40, v + 40, SYS_LEN - 40, out) == 1 && out[0] == DL_ST_OK);
	CHECK(codeplug_kv_get(SYS(0), back, sizeof back) == SYS_LEN && memcmp(back, v, SYS_LEN) == 0);
	CHECK(cpkv(DL_CPKV_GET, SYS(0), 16, NULL, 0, out) == 3u + SYS_LEN - 16u && out[0] == DL_ST_OK &&
	      out[1] == SYS_LEN && out[2] == 0 && memcmp(out + 3, v + 16, SYS_LEN - 16) == 0);
	// A patch inside the value keeps the rest; one past its end is refused
	uint8_t cc = 7;
	CHECK(cpkv(DL_CPKV_SET, SYS(0), BLOCK + 4, &cc, 1, out) == 1 && out[0] == DL_ST_OK);
	CHECK(codeplug_kv_get(SYS(0), back, sizeof back) == SYS_LEN && back[BLOCK + 4] == 7 && back[BLOCK + 22] == 2);
	CHECK(cpkv(DL_CPKV_SET, SYS(0), SYS_LEN + 1, &cc, 1, out) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cpkv(DL_CPKV_SET, 0x7F00, 4, &cc, 1, out) == 1 && out[0] == DL_ST_BAD_ARG);	// a patch of no value
	// The channel and the load over the link (DL_T3_LOAD's call)
	uint8_t r[CHAN_LEN] = { 0, CODEPLUG_ENTRY_CHANNEL, CODEPLUG_GROUP_NONE, CODEPLUG_ENTRY_CONTROL };
	put16(&r[4], 1);
	put32(&r[6], 450012500);
	put32(&r[10], 455012500);
	r[14] = 0xFF;
	CHECK(cpkv(DL_CPKV_SET, ENTRY(0), 0, r, sizeof r, out) == 1 && out[0] == DL_ST_OK);
	CHECK(radio_t3_load(0, &s) && s.cc == 7 && s.cfg.nctrl == 1);
	CHECK(cpkv(DL_CPKV_REMOVE, ENTRY(0), 0, NULL, 0, out) == 1 && out[0] == DL_ST_OK);
	CHECK(!radio_t3_load(0, &s));
	CHECK(cpkv(DL_CPKV_GET, ENTRY(0), 0, NULL, 0, out) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(cpkv(9, ENTRY(0), 0, NULL, 0, out) == 1 && out[0] == DL_ST_BAD_ARG);
}

int main(void)
{
	test_load();
#if CONFIG_DMR_TRUNK_SUPL
	test_supl();
#endif
	test_poll_long();
	test_talk();
	test_chan_limit();
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
