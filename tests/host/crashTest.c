// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash log's record (core/crash/crash.c, docs/spec/crash-log.md): CRC, sealing and the count of crashes in a row,
// the build ID, the start-up side on a fake store, the packed report and the summary line; the UI's side
// (core/ui/ui_svc_crash.c): the record for the screen, seen and clear, the report sent and its progress; the debug
// link's (core/debuglink/debuglink_crash.c): the view section, DL_CMD_CRASH's checks and the deferred crash test
#include <stdio.h>
#include <string.h>
#include "crash/crash.h"
#include "ui/ui_svc.h"
#include "codeplug/codeplug.h"
#include "dmr/dmr_data_api.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The platform's side, faked: a store in RAM that a test can take away
static crash_record_t store;
static bool have_store = true;

crash_record_t *hal_crash_store(void) { return have_store ? &store : NULL; }
uint8_t hal_crash_arch(void) { return CRASH_ARCH_CM4; }
bool hal_crash_ram_addr(uint32_t a) { return a >= 0x20000000u && a < 0x20020000u; }
bool hal_crash_code_addr(uint32_t a) { return a >= 0x08000000u && a < 0x08100000u; }
void hal_crash_restart(void) { for (;;) { } }

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static crash_record_t fault(void)
{
	crash_record_t r;
	memset(&r, 0, sizeof r);
	r.arch = CRASH_ARCH_CM4;
	r.kind = CRASH_KIND_FAULT;
	r.cause = 3;
	r.pc = 0x0802C4A9u;
	r.lr = 0x0803082Du;
	r.sp = 0x20004F00u;
	r.psr = 0x21000000u;
	for (int k = 0; k < 14; k++)
		r.r[k] = 0x1000u + (uint32_t)k;
	r.status[0] = 0x00008200u;	// CFSR: precise bus error, BFAR valid
	r.status[3] = 0xDEADBEEFu;
	r.uptime_ms = 123456;
	memcpy(r.task, "radio", 5);
	return r;
}

static void test_crc(void)
{
	CHECK(crash_crc32("123456789", 9) == 0xCBF43926u);	// the standard check value
	CHECK(crash_crc32("", 0) == 0);
}

static void test_layout(void)
{
	CHECK(sizeof(crash_record_t) == 188);	// tools/crash_decode.py and the spec say so
	CHECK(offsetof(crash_record_t, crc) == 184);
	CHECK(offsetof(crash_record_t, stack) == 120);
}

static void test_build_id(void)
{
	bool dirty = true;
	CHECK(crash_build_id("34c825a6", &dirty) == 0x34C825A6u && !dirty);
	CHECK(crash_build_id("34c825a6-dirty", &dirty) == 0x34C825A6u && dirty);
	CHECK(crash_build_id("abc", &dirty) == 0xABCu && !dirty);
	CHECK(crash_build_id("host", &dirty) == 0 && !dirty);
	CHECK(crash_build_id("unknown", &dirty) == 0);
	CHECK(crash_build_id("12345678x", &dirty) == 0);	// not a hash followed by "-..."
}

static void test_seal(void)
{
	crash_record_t r = fault();
	crash_seal(&r, NULL);
	CHECK(crash_valid(&r));
	CHECK(r.magic == CRASH_MAGIC && r.version == CRASH_VERSION && r.size == sizeof r && r.count == 1);
	CHECK(!crash_valid(NULL));

	crash_record_t bad = r;
	bad.pc ^= 4;
	CHECK(!crash_valid(&bad));	// the CRC sees a changed field
	bad = r;
	bad.version = 2;
	crash_reseal(&bad);
	CHECK(!crash_valid(&bad));	// another layout
	bad = r;
	bad.size = 100;
	crash_reseal(&bad);
	CHECK(!crash_valid(&bad));
	bad = r;
	bad.magic = 0;
	CHECK(!crash_valid(&bad));

	// In a row: the same build, nobody looked: counts up, saturating
	crash_record_t r2 = fault();
	crash_seal(&r2, &r);
	CHECK(r2.count == 2 && crash_valid(&r2));
	r2.count = 0xFF;
	crash_reseal(&r2);
	crash_record_t r3 = fault();
	crash_seal(&r3, &r2);
	CHECK(r3.count == 0xFF);
	// Seen on the screen: the next one starts over
	r.flags |= CRASH_F_SEEN;
	crash_reseal(&r);
	crash_seal(&r3, &r);
	CHECK(r3.count == 1);
	// Another build's record, or garbage, starts over too
	crash_record_t other = fault();
	crash_seal(&other, NULL);
	other.build = 0x12345678u;
	crash_reseal(&other);
	crash_seal(&r3, &other);
	CHECK(r3.count == 1);
	memset(&other, 0x5A, sizeof other);
	crash_seal(&r3, &other);
	CHECK(r3.count == 1);

	// The host build's version is "host": no build ID, not dirty
	CHECK(r.build == 0 && !(r.flags & CRASH_F_DIRTY));
}

static void test_store(void)
{
	// Nothing kept: garbage in the store (power-on contents)
	memset(&store, 0xA5, sizeof store);
	crash_init();
	CHECK(crash_last() == NULL);
	crash_mark(CRASH_F_SEEN);	// nothing to mark: harmless
	CHECK(crash_last() == NULL);

	// A record from before this start
	crash_record_t r = fault();
	crash_seal(&r, NULL);
	store = r;
	crash_init();
	const crash_record_t *l = crash_last();
	CHECK(l != NULL && l->pc == r.pc && l->count == 1 && !(l->flags & CRASH_F_SEEN));

	// Marked: here and in the store, which stays valid
	crash_mark(CRASH_F_SEEN);
	CHECK(crash_last()->flags & CRASH_F_SEEN);
	CHECK(crash_valid(&store) && (store.flags & CRASH_F_SEEN));
	crash_mark(CRASH_F_SENT);
	CHECK((store.flags & (CRASH_F_SEEN | CRASH_F_SENT)) == (CRASH_F_SEEN | CRASH_F_SENT) && crash_valid(&store));
	crash_init();
	CHECK(crash_last() != NULL && (crash_last()->flags & CRASH_F_SENT));	// kept across another start

	// Cleared: gone here and at the next start
	crash_clear();
	CHECK(crash_last() == NULL && !crash_valid(&store));
	crash_init();
	CHECK(crash_last() == NULL);

	// A platform without a store
	have_store = false;
	crash_init();
	CHECK(crash_last() == NULL);
	crash_clear();
	have_store = true;
}

static void test_pack(void)
{
	crash_record_t r = fault();
	r.flags |= CRASH_F_STACK;
	for (int k = 0; k < CRASH_STACK_WORDS; k++)
		r.stack[k] = 0x20001000u + 4u * (uint32_t)k;	// data
	r.stack[2] = 0x0801234Bu;	// three return addresses (Thumb: bit 0 set)
	r.stack[7] = 0x08055501u;
	r.stack[15] = 0x080FFFFFu;
	crash_seal(&r, NULL);

	uint8_t out[CRASH_PACK_MAX];
	size_t n = crash_pack(&r, out, sizeof out, hal_crash_code_addr);
	CHECK(n == CRASH_PACK_FIXED + 3 * 4);
	CHECK(out[0] == 'O' && out[1] == 'C' && out[2] == CRASH_VERSION && out[3] == CRASH_ARCH_CM4);
	CHECK(out[4] == CRASH_KIND_FAULT && out[5] == r.flags && out[6] == 1 && out[7] == 3);
	CHECK(get32(out + 8) == r.build && get32(out + 12) == 123456u && get32(out + 16) == 3);
	CHECK(get32(out + 20) == r.pc && get32(out + 24) == r.lr && get32(out + 28) == r.sp && get32(out + 32) == r.psr);
	CHECK(get32(out + 36) == 0x8200u && get32(out + 48) == 0xDEADBEEFu);
	CHECK(memcmp(out + 52, "radio\0\0\0", 8) == 0);
	CHECK(get32(out + 60) == 0x1000u && get32(out + 72) == 0x1003u);
	CHECK(get32(out + 76) == 0x0801234Bu && get32(out + 80) == 0x08055501u && get32(out + 84) == 0x080FFFFFu);

	// Room for the fixed part and one word: the first return address only
	n = crash_pack(&r, out, CRASH_PACK_FIXED + 5, hal_crash_code_addr);
	CHECK(n == CRASH_PACK_FIXED + 4 && out[7] == 1);
	// No room for the fixed part
	CHECK(crash_pack(&r, out, CRASH_PACK_FIXED - 1, hal_crash_code_addr) == 0);
	// No stack copy (sp wasn't in RAM), or no code check: no return addresses
	r.flags &= (uint8_t)~CRASH_F_STACK;
	CHECK(crash_pack(&r, out, sizeof out, hal_crash_code_addr) == CRASH_PACK_FIXED && out[7] == 0);
	r.flags |= CRASH_F_STACK;
	CHECK(crash_pack(&r, out, sizeof out, NULL) == CRASH_PACK_FIXED);

	// Every stack word a return address: CRASH_PACK_RET_MAX of them, CRASH_PACK_MAX bytes
	for (int k = 0; k < CRASH_STACK_WORDS; k++)
		r.stack[k] = 0x08000001u + 4u * (uint32_t)k;
	n = crash_pack(&r, out, sizeof out, hal_crash_code_addr);
	CHECK(n == CRASH_PACK_MAX && out[7] == CRASH_PACK_RET_MAX);
}

static void test_summary(void)
{
	char s[64];
	crash_record_t r = fault();
	crash_summary(&r, s, sizeof s);
	CHECK(strcmp(s, "Fault 3 at 0802C4A9 in radio") == 0);
	r.flags |= CRASH_F_ISR;
	crash_summary(&r, s, sizeof s);
	CHECK(strcmp(s, "Fault 3 at 0802C4A9 in an interrupt") == 0);

	r = fault();
	r.kind = CRASH_KIND_ASSERT;
	r.cause = 712;
	memset(r.task, 0, sizeof r.task);
	crash_summary(&r, s, sizeof s);
	CHECK(strcmp(s, "Assert line 712 at 0802C4A9") == 0);

	r = fault();
	r.kind = CRASH_KIND_STACK;
	memcpy(r.task, "audio_ta", 8);	// a full-length name has no terminator in the record
	crash_summary(&r, s, sizeof s);
	CHECK(strcmp(s, "Stack overflow in audio_ta") == 0);

	r.kind = CRASH_KIND_HEAP;
	memset(r.task, 0, sizeof r.task);
	crash_summary(&r, s, sizeof s);
	CHECK(strcmp(s, "Out of memory") == 0);
	CHECK(strcmp(crash_kind_name(0), "Crash") == 0);

	// Cut to the buffer, always terminated
	r = fault();
	crash_summary(&r, s, 9);
	CHECK(strcmp(s, "Fault 3 ") == 0);
	s[0] = 'x';
	crash_summary(&r, s, 0);
	CHECK(s[0] == 'x');
}

// --- The UI's side (core/ui/ui_svc_crash.c) on fakes of the settings, the codeplug and the DMR data service ---

static int32_t fake_contact_num;	// UI_SET_CRASH_CONTACT
static contact_t fake_contact;
static bool fake_contact_ok;
static bool fake_udp_ok = true;		// dmr_data_send_udp's answer
static dmr_req_state_t fake_udp_state;
static struct {
	unsigned calls;
	uint32_t dst;
	bool group;
	uint16_t src_port, dst_port;
	uint8_t data[DMR_DATA_UDP_MAX];
	size_t len;
} udp;

int32_t ui_svc_setting_get(uint8_t id)
{
	return id == UI_SET_CRASH_CONTACT ? fake_contact_num : 0;
}

bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (!fake_contact_ok || index != fake_contact_num)
		return false;
	*out = fake_contact;
	return true;
}

bool dmr_data_send_udp(uint32_t dst, bool group, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
	udp.calls++;
	if (!fake_udp_ok)
		return false;
	udp.dst = dst;
	udp.group = group;
	udp.src_port = src_port;
	udp.dst_port = dst_port;
	udp.len = len <= sizeof udp.data ? len : sizeof udp.data;
	memcpy(udp.data, payload, udp.len);
	fake_udp_state = DMR_REQ_QUEUED;
	return true;
}

dmr_req_state_t dmr_data_udp_state(void)
{
	return fake_udp_state;
}

static void put_store(const crash_record_t *r)
{
	store = *r;
	crash_init();
}

static void test_ui_svc(void)
{
	ui_crash_t c;
	memset(&store, 0, sizeof store);
	crash_init();
	CHECK(!ui_svc_crash_get(&c));
	CHECK(!ui_svc_crash_send());		// nothing to send
	CHECK(udp.calls == 0);
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_IDLE);	// nothing sent yet: idle, whatever the service says
	ui_svc_crash_seen();			// harmless without a record
	ui_svc_crash_clear();

	// A record: the fields the screen shows
	crash_record_t r = fault();
	r.flags |= CRASH_F_STACK;
	for (int k = 0; k < CRASH_STACK_WORDS; k++)
		r.stack[k] = 0x20001000u;
	r.stack[3] = 0x08012345u;		// one return address
	crash_seal(&r, NULL);
	r.count = 3;
	r.build = 0x34C825A6u;
	crash_reseal(&r);
	put_store(&r);
	CHECK(ui_svc_crash_get(&c));
	CHECK(!c.seen && !c.sent && c.count == 3 && c.uptime_ms == 123456u && c.build == 0x34C825A6u);
	CHECK(strcmp(c.summary, "Fault 3 at 0802C4A9 in radio") == 0);

	// Seen: in the record and the store
	ui_svc_crash_seen();
	CHECK(ui_svc_crash_get(&c) && c.seen && !c.sent);
	CHECK(crash_valid(&store) && (store.flags & CRASH_F_SEEN));

	// No target, a target the codeplug doesn't have, a contact without an ID: nothing goes
	fake_contact_num = 0;
	CHECK(!ui_svc_crash_send());
	fake_contact_num = 5;
	fake_contact_ok = false;
	CHECK(!ui_svc_crash_send());
	fake_contact_ok = true;
	memset(&fake_contact, 0, sizeof fake_contact);
	fake_contact.type = CONTACT_PRIVATE;
	CHECK(!ui_svc_crash_send());
	CHECK(udp.calls == 0);

	// The data service refuses it (one still going): false, and the state stays idle
	fake_contact.id = 3101234;
	fake_udp_ok = false;
	CHECK(!ui_svc_crash_send());
	CHECK(udp.calls == 1 && ui_svc_crash_tx_state() == UI_CRASH_TX_IDLE);
	fake_udp_ok = true;

	// A private contact: the packed report to its ID, port 4090 both ends
	CHECK(ui_svc_crash_send());
	CHECK(udp.calls == 2 && udp.dst == 3101234u && !udp.group);
	CHECK(udp.src_port == CRASH_UDP_PORT && udp.dst_port == CRASH_UDP_PORT);
	uint8_t want[CRASH_PACK_MAX];
	size_t n = crash_pack(&store, want, sizeof want, hal_crash_code_addr);
	CHECK(n == CRASH_PACK_FIXED + 4 && udp.len == n && memcmp(udp.data, want, n) == 0);

	// Its progress, as the data service has it; OK marks the record sent (once)
	fake_udp_state = DMR_REQ_QUEUED;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_QUEUED);
	fake_udp_state = DMR_REQ_SENDING;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_SENDING);
	CHECK(ui_svc_crash_get(&c) && !c.sent);
	fake_udp_state = DMR_REQ_OK;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_OK);
	CHECK(ui_svc_crash_get(&c) && c.sent && c.seen);
	CHECK(crash_valid(&store) && (store.flags & (CRASH_F_SENT | CRASH_F_SEEN)) == (CRASH_F_SENT | CRASH_F_SEEN));
	store.flags &= (uint8_t)~CRASH_F_SENT;	// the mark is made once: a second OK leaves the store alone
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_OK);
	CHECK(!(store.flags & CRASH_F_SENT));
	fake_udp_state = DMR_REQ_IDLE;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_IDLE);

	// A group contact: to the talkgroup, as a group call; a failed one says so and marks nothing
	put_store(&r);
	fake_contact.type = CONTACT_GROUP;
	fake_contact.id = 91;
	CHECK(ui_svc_crash_send());
	CHECK(udp.calls == 3 && udp.dst == 91u && udp.group);
	fake_udp_state = DMR_REQ_FAILED;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_FAILED);
	CHECK(ui_svc_crash_get(&c) && !c.sent && !(store.flags & CRASH_F_SENT));
	// Sent again after the failure: a new OK marks it
	CHECK(ui_svc_crash_send());
	fake_udp_state = DMR_REQ_OK;
	CHECK(ui_svc_crash_tx_state() == UI_CRASH_TX_OK && (store.flags & CRASH_F_SENT));

	// Cleared: gone, nothing to send
	ui_svc_crash_clear();
	CHECK(!ui_svc_crash_get(&c) && !crash_valid(&store));
	CHECK(!ui_svc_crash_send() && udp.calls == 4);
}

// --- The debug link's side (core/debuglink/debuglink_crash.c): the crash view section and DL_CMD_CRASH ---

static uint8_t fake_radio_state = RS_RX;
static unsigned asserts, delays;
static unsigned assert_line;

void radio_get_view(radio_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = fake_radio_state;
}

void hal_delay_ms(uint32_t ms) { (void)ms; delays++; }
void dlog(const char *fmt, ...) { (void)fmt; }
// The host build has no noreturn (CFLAGS): the fake comes back, so the service's path after it can be checked
void crash_assert(unsigned line) { asserts++; assert_line = line; }

static const char *view_text(char *buf, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, buf, cap - 1);
	debuglink_view_crash(&t);
	buf[t.len] = 0;
	return buf;
}

static size_t crash_cmd(const uint8_t *req, uint16_t len, uint8_t *st)
{
	uint8_t out[8] = { 0xEE };
	size_t n = debuglink_cmd_crash(req, len, out, sizeof out);
	*st = out[0];
	return n;
}

static void test_debuglink(void)
{
	static char text[2048];

	// No record: the section says so and nothing else
	memset(&store, 0, sizeof store);
	crash_init();
	CHECK(strcmp(view_text(text, sizeof text), "crash.built=1\ncrash.valid=0\n") == 0);

	// A record: every field; the stack words only with CRASH_F_STACK
	crash_record_t r = fault();
	memcpy(r.task, "audio_ta", 8);		// a full-length name: terminated in the text
	crash_seal(&r, NULL);
	put_store(&r);
	view_text(text, sizeof text);
	CHECK(strstr(text, "crash.valid=1\n") != NULL && strstr(text, "crash.arch=1\ncrash.kind=1\ncrash.cause=3\n"));
	CHECK(strstr(text, "crash.pc=0x0802c4a9\ncrash.lr=0x0803082d\ncrash.sp=0x20004f00\ncrash.psr=0x21000000\n"));
	CHECK(strstr(text, "crash.status=0x00008200,0x00000000,0x00000000,0xdeadbeef\n"));
	CHECK(strstr(text, "crash.task=audio_ta\ncrash.uptime_ms=123456\ncrash.build=00000000\n"));
	CHECK(strstr(text, "crash.r0=0x00001000\n") && strstr(text, "crash.r13=0x0000100d\n"));
	CHECK(strstr(text, "crash.stack=") == NULL);
	r.flags |= CRASH_F_STACK;
	for (int k = 0; k < CRASH_STACK_WORDS; k++)
		r.stack[k] = 0x20000000u + (uint32_t)k;
	crash_reseal(&r);
	put_store(&r);
	view_text(text, sizeof text);
	CHECK(strstr(text, "crash.flags=0x4\n") != NULL);
	CHECK(strstr(text, "crash.stack=20000000,20000001,") && strstr(text, ",2000000f\n"));

	// DL_CMD_CRASH: wrong lengths and operations
	uint8_t st;
	CHECK(crash_cmd(NULL, 0, &st) == 1 && st == DL_ST_BAD_ARG);
	const uint8_t clear_long[] = { DL_CRASH_OP_CLEAR, 0 };
	CHECK(crash_cmd(clear_long, 2, &st) == 1 && st == DL_ST_BAD_ARG);
	const uint8_t unknown[] = { 9 };
	CHECK(crash_cmd(unknown, 1, &st) == 1 && st == DL_ST_BAD_ARG);
	uint8_t test[6] = { DL_CRASH_OP_TEST, DL_CRASH_TEST_ASSERT, 0x43, 0x52, 0x53, 0x48 };	// "CRSH"
	CHECK(crash_cmd(test, 5, &st) == 1 && st == DL_ST_BAD_ARG);
	test[1] = 0;
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_BAD_ARG);		// no such test
	test[1] = DL_CRASH_TEST_ASSERT + 1;
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_BAD_ARG);
	test[1] = DL_CRASH_TEST_ASSERT;
	test[5] = 0;
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_REFUSED);		// a wrong token
	test[5] = 0x48;
	fake_radio_state = RS_TX;
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_REFUSED);		// transmitting
	fake_radio_state = RS_RX;
	debuglink_crash_service();
	CHECK(asserts == 0 && delays == 0);					// nothing was accepted

	// Accepted: the crash comes from the service, after the delay, not in the command
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_OK);
	CHECK(asserts == 0);
	debuglink_crash_service();
	CHECK(asserts == 1 && delays == 1 && assert_line > 0);
	debuglink_crash_service();						// once
	CHECK(asserts == 1 && delays == 1);

	// A PTT during the delay: no crash, and it doesn't stay pending
	CHECK(crash_cmd(test, 6, &st) == 1 && st == DL_ST_OK);
	fake_radio_state = RS_TX;
	debuglink_crash_service();
	CHECK(asserts == 1 && delays == 2);
	fake_radio_state = RS_RX;
	debuglink_crash_service();
	CHECK(asserts == 1 && delays == 2);

	// Clear: the record goes
	const uint8_t clear[] = { DL_CRASH_OP_CLEAR };
	CHECK(crash_cmd(clear, 1, &st) == 1 && st == DL_ST_OK);
	CHECK(crash_last() == NULL && !crash_valid(&store));
	CHECK(strcmp(view_text(text, sizeof text), "crash.built=1\ncrash.valid=0\n") == 0);
}

// The fault trace line (crash_trace.c): exactly what tools/odr_fault_trace.py's selftest decodes (its GOLDEN)
static char trace_buf[512];
static size_t trace_len;

static void trace_out(const char *s)
{
	size_t n = strlen(s);
	if (trace_len + n < sizeof trace_buf) {
		memcpy(trace_buf + trace_len, s, n + 1);
		trace_len += n;
	}
}

static void test_trace(void)
{
	static const uint32_t regs[4] = { 0, 1, 2, 3 };
	crash_trace_t t = { .vector = 2, .pc = 0x0300C1A4u, .psr = 0x80000140u, .lr = 0x0300C180u, .sp = 0x00012340u,
			    .r = regs, .nr = 4 };
	static const char body[] = "#ODRFAULT1 n=01 v=00000002 pc=0300c1a4 psr=80000140 lr=0300c180 sp=00012340 "
				   "r0=00000000 r1=00000001 r2=00000002 r3=00000003 ";
	trace_len = 0;
	crash_trace_line(&t, 1, trace_out);
	CHECK(strcmp(trace_buf, "\r\n#ODRFAULT1 n=01 v=00000002 pc=0300c1a4 psr=80000140 lr=0300c180 sp=00012340 "
			  "r0=00000000 r1=00000001 r2=00000002 r3=00000003 *e471\r\n") == 0);
	CHECK(crash_trace_crc(body) == 0xE471u);
	CHECK(crash_trace_crc("123456789") == 0x29B1u);	// CRC-16/CCITT-FALSE's check value
	// Sixteen registers at most, two-digit names from r10
	static const uint32_t many[20] = { 0 };
	crash_trace_t m = { .r = many, .nr = 20 };
	trace_len = 0;
	crash_trace_line(&m, 3, trace_out);
	CHECK(strstr(trace_buf, "n=03 ") != NULL && strstr(trace_buf, " r15=00000000 *") != NULL);
	CHECK(strstr(trace_buf, "r16=") == NULL);
}

int main(void)
{
	test_crc();
	test_trace();
	test_layout();
	test_build_id();
	test_seal();
	test_store();
	test_pack();
	test_summary();
	test_ui_svc();
	test_debuglink();
	if (failures) {
		printf("crashTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("crashTest: all passed\n");
	return 0;
}
