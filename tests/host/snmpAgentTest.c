// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SNMP agent's firmware glue (core/snmp/snmp_agent.c) on fakes of the radio, the settings, the IP gateway and the
// USB network interface: requests from the host answered at once (and retried while the IN endpoint is busy), requests
// over the air through the one-request mailbox (size limit, busy, the answer gap, RF write access, the air side busy
// until the give-up time), settings saved on SET and from the debug link, authentication-failure notifications to the
// host, the agent switched off; then mutated requests from both sides (fuzz.h; RFOUNDRY_FUZZ_ITERS).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"
#include "hostlink/hostlink.h"
#include "hostlink/hostlink_net.h"
#include "dmr/ipGateway.h"
#include "dmr/dmr_data_api.h"
#include "dmr/dmrDataService.h"
#include "snmp/snmp_agent.h"
#include "snmp/snmpBer.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fakes ----

static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; }

static bool link_up = true;
static int ticks;
static bool f_up(void) { return link_up; }
static void f_tick(uint32_t t) { (void)t; ticks++; }
static const hal_netif_t netif = { .up = f_up, .tick = f_tick };
static const char *const powers[] = { "Low", "High" };
const board_t board = { .name = "testboard", .power_count = 2, .power_names = powers, .netif = &netif };

static radio_view_t fake_view;
void radio_get_view(radio_view_t *v) { *v = fake_view; }

static settings_t stored, staged;
static int saves;
void settings_read(settings_t *out) { *out = stored; }
settings_t *settings_begin(void) { staged = stored; return &staged; }
void settings_end(uint32_t t) { (void)t; stored = staged; saves++; }

void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1]) { strcpy(out, "CPCALL"); }
void dmrDataServiceGetStats(dmrDataStats_t *s) { memset(s, 0, sizeof(*s)); }
int sms_inbox_count(void) { return 0; }
int sms_inbox_unread(void) { return 0; }
bool sms_inbox_get(int i, dmrDataMessage_t *out) { (void)i; (void)out; return false; }
dmrArsState_t dmr_data_ars_state(void) { return DMR_ARS_STATE_OFF; }
dmrDataEvent_t dmr_data_last_event(void) { dmrDataEvent_t e; memset(&e, 0, sizeof(e)); return e; }
static bool net_mode = true;
bool hostlink_network_mode(void) { return net_mode; }
void hostlink_net_stats(hostlink_net_stats_t *s) { memset(s, 0, sizeof(*s)); }

// The gateway: the bound handler, what went to the host, what went to the air
static ipGatewayUdpHandler handler;
static uint16_t bound_port;
bool ipGatewayUdpBindAir(uint16_t port, ipGatewayUdpHandler h) { bound_port = port; handler = h; return true; }
static int usb_busy;			// ipGatewayUdpSend refuses this many times
static int usb_sent, usb_len;
static uint32_t usb_ip;
static uint16_t usb_sport, usb_dport;
static uint8_t usb_buf[1600];
bool ipGatewayUdpSend(uint32_t ip, uint16_t sport, uint16_t dport, const uint8_t *p, int n)
{
	if (usb_busy > 0) {
		usb_busy--;
		return false;
	}
	usb_sent++;
	usb_ip = ip;
	usb_sport = sport;
	usb_dport = dport;
	usb_len = n;
	memcpy(usb_buf, p, (size_t)n);
	return true;
}
static uint32_t host_ip = 0x0A000001u;
uint32_t ipGatewayHostIP(void) { return host_ip; }
static bool air_busy;
static int air_sent, air_len;
static uint32_t air_dst;
static uint8_t air_buf[1600];
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t proto, const uint8_t *l4, int n)
{
	if (air_busy) {
		return false;
	}
	air_sent++;
	air_dst = dst;
	air_len = n;
	memcpy(air_buf, l4, (size_t)n);
	(void)group;
	(void)proto;
	return true;
}

// ---- requests (an independent encoder, short and 82 lengths) ----

typedef struct {
	uint8_t b[700];
	int n;
} buf_t;

static void tlv(buf_t *o, uint8_t tag, const void *p, int n)
{
	o->b[o->n++] = tag;
	if (n >= 0x80) {
		o->b[o->n++] = 0x82;
		o->b[o->n++] = (uint8_t)(n >> 8);
	}
	o->b[o->n++] = (uint8_t)n;
	if (n > 0) {
		memcpy(o->b + o->n, p, (size_t)n);
		o->n += n;
	}
}

static void intv(buf_t *o, int32_t v)
{
	uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };

	tlv(o, 0x02, t, 4);
}

// One binding per oid; value: NULL, or (tag, val, vlen) for every binding of a SET
static int request(uint8_t *out, const char *community, uint8_t pdu, int f2, int f3, const uint8_t *oid, int olen,
		   int count, uint8_t vtag, const void *val, int vlen)
{
	buf_t list = { .n = 0 }, p = { .n = 0 }, m = { .n = 0 }, msg = { .n = 0 };

	for (int i = 0; i < count; i++) {
		buf_t vb = { .n = 0 };

		tlv(&vb, 0x06, oid, olen);
		tlv(&vb, vtag, val, vlen);
		tlv(&list, 0x30, vb.b, vb.n);
	}
	intv(&p, 0x4242);
	intv(&p, f2);
	intv(&p, f3);
	tlv(&p, 0x30, list.b, list.n);
	intv(&m, 1);
	tlv(&m, 0x04, community, (int)strlen(community));
	tlv(&m, pdu, p.b, p.n);
	tlv(&msg, 0x30, m.b, m.n);
	memcpy(out, msg.b, (size_t)msg.n);
	return msg.n;
}

static const uint8_t SYS_NAME[] = { 0x2B, 6, 1, 2, 1, 1, 5, 0 };
static const uint8_t SYS_DESCR[] = { 0x2B, 6, 1, 2, 1, 1, 1, 0 };
static const uint8_t START[] = { 0x2B };

static bool any_id;	// the fuzz loop's requests have any ID

// The answer's error status and its first binding's value (as an OCTET STRING), -1 if it doesn't parse
static int answer(const uint8_t *p, int n, char *text, int *bindings)
{
	berReader_t top, msg, pdu, list, vb;
	int32_t v, id, status, index;
	uint8_t tag;
	const uint8_t *val;
	int len;

	berReaderInit(&top, p, n);
	if (!berEnter(&top, 0x30, &msg) || !berGetInt(&msg, &v) || !berGet(&msg, &tag, &val, &len) ||
	    !berEnter(&msg, 0xA2, &pdu) || !berGetInt(&pdu, &id) || (!any_id && (id != 0x4242)) || !berGetInt(&pdu, &status) ||
	    !berGetInt(&pdu, &index) || !berEnter(&pdu, 0x30, &list)) {
		return -1;
	}
	int count = 0;

	if (text != NULL) {
		text[0] = '\0';
	}
	while (!berAtEnd(&list)) {
		if (!berEnter(&list, 0x30, &vb) || !berGet(&vb, &tag, &val, &len) || !berGet(&vb, &tag, &val, &len)) {
			return -1;
		}
		if ((count == 0) && (text != NULL) && (tag == 0x04) && (len < 64)) {
			memcpy(text, val, (size_t)len);
			text[len] = '\0';
		}
		count++;
	}
	if (bindings != NULL) {
		*bindings = count;
	}
	return list.err ? -1 : status;
}

static void from_host(const uint8_t *p, int n)
{
	handler(false, host_ip, 40000, 0x0A000002u, 161, p, n);
}

// The host link task's passes: the first answers a waiting request into the staging buffer, the next hands it to the air
static void air_poll(void)
{
	snmp_agent_poll(now_ms);
	snmp_agent_poll(now_ms);
}

static void from_air(uint32_t src, const uint8_t *p, int n)
{
	handler(true, src, 41000, 3100001, 161, p, n);
}

static void reset_io(void)
{
	usb_sent = air_sent = 0;
	usb_busy = 0;
	air_busy = false;
	link_up = true;
}

// ---- tests ----

static void test_usb(void)
{
	uint8_t q[700];
	char text[64];
	int nb;

	CHECK(bound_port == 161 && handler != NULL);
	// sysName: the codeplug's callsign while the settings have none, then the settings' name
	int qn = request(q, "public", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	reset_io();
	from_host(q, qn);
	CHECK(usb_sent == 1 && usb_ip == host_ip && usb_sport == 161 && usb_dport == 40000);
	CHECK(answer(usb_buf, usb_len, text, &nb) == 0 && nb == 1 && strcmp(text, "CPCALL") == 0);
	memcpy(stored.radio_name, "RADIO01", 7);
	from_host(q, qn);
	CHECK(usb_sent == 2 && answer(usb_buf, usb_len, text, NULL) == 0 && strcmp(text, "RADIO01") == 0);
	// a full SETTINGS_NAME_LEN name with no NUL in settings is cut at the MIB's limit, never read past
	memset(stored.radio_name, 'Z', sizeof(stored.radio_name));
	from_host(q, qn);
	CHECK(usb_sent == 3 && answer(usb_buf, usb_len, text, NULL) == 0 && strlen(text) == SETTINGS_NAME_LEN);
	memset(stored.radio_name, 0, sizeof(stored.radio_name));

	// The IN endpoint busy: retried (the link ticked) and sent; busy past USB_WAIT_MS or the link down: dropped
	reset_io();
	usb_busy = 5;
	ticks = 0;
	from_host(q, qn);
	CHECK(usb_sent == 1 && ticks == 5);
	reset_io();
	usb_busy = 1000;
	uint32_t t0 = now_ms;
	from_host(q, qn);
	CHECK(usb_sent == 0 && (now_ms - t0) >= 50 && (now_ms - t0) <= 52);
	reset_io();
	usb_busy = 1000;
	link_up = false;
	t0 = now_ms;
	from_host(q, qn);
	CHECK(usb_sent == 0 && now_ms == t0);
	usb_busy = 0;
	link_up = true;

	// The USB answer limit: a GetBulk of 32 repetitions fits SNMP_USB_MAX, whole bindings
	qn = request(q, "public", 0xA5, 0, 32, START, 1, 1, 0x05, NULL, 0);
	reset_io();
	from_host(q, qn);
	CHECK(usb_sent == 1 && usb_len <= SNMP_USB_MAX && answer(usb_buf, usb_len, NULL, &nb) == 0 && nb > 5 && nb < 32);
	// no answer to garbage, to a wrong community, to a response
	reset_io();
	from_host(q, 5);
	q[0] = 0x31;
	from_host(q, qn);
	qn = request(q, "nope", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	from_host(q, qn);
	qn = request(q, "public", 0xA2, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	from_host(q, qn);
	CHECK(usb_sent == 0);
}

static void test_set_and_config(void)
{
	uint8_t q[700];
	char text[64];

	// SET sysName with the write community: answered, saved to the settings NUL padded
	snmp_agent_set_community(true, "private", 7);
	CHECK(stored.snmp_write_hash == snmpCommunityHash((const uint8_t *)"private", 7));
	int qn = request(q, "private", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "W1AW", 4);
	reset_io();
	saves = 0;
	memset(stored.radio_name, 'x', sizeof(stored.radio_name));
	from_host(q, qn);
	CHECK(usb_sent == 1 && answer(usb_buf, usb_len, text, NULL) == 0 && strcmp(text, "W1AW") == 0);
	CHECK(saves == 1 && memcmp(stored.radio_name, "W1AW", 4) == 0 && stored.radio_name[4] == 0 &&
	      stored.radio_name[SETTINGS_NAME_LEN - 1] == 0);
	// The longest name: all SETTINGS_NAME_LEN characters, no terminator in settings (radio_name is NUL padded)
	qn = request(q, "private", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "ABCDEFGHIJKLMNOP", SETTINGS_NAME_LEN);
	from_host(q, qn);
	CHECK(answer(usb_buf, usb_len, NULL, NULL) == 0 && memcmp(stored.radio_name, "ABCDEFGHIJKLMNOP", SETTINGS_NAME_LEN) == 0);
	// One too long: wrongLength, nothing saved
	saves = 0;
	qn = request(q, "private", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "ABCDEFGHIJKLMNOPQ", SETTINGS_NAME_LEN + 1);
	from_host(q, qn);
	CHECK(answer(usb_buf, usb_len, NULL, NULL) == 8 && saves == 0);	// wrongLength (RFC 3416)
	// The read community can't SET; the debug link's config round trip; a read community replaces "public"
	qn = request(q, "public", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "K1", 2);
	from_host(q, qn);
	CHECK(answer(usb_buf, usb_len, NULL, NULL) == 6 && saves == 0);	// noAccess
	snmpConfig_t c;
	snmp_agent_get_config(&c);
	c.rfManager = 0x123456;
	c.rfTrapGapS = 90;
	snmp_agent_set_config(&c);
	CHECK(stored.snmp_rf_manager[0] == 0x56 && stored.snmp_rf_manager[1] == 0x34 && stored.snmp_rf_manager[2] == 0x12 &&
	      stored.snmp_rf_gap_s == 90);
	snmp_agent_set_community(false, "s3cret", 6);
	qn = request(q, "public", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	reset_io();
	from_host(q, qn);
	CHECK(usb_sent == 0);
	snmp_agent_set_community(false, "", 0);	// back to "public"
	from_host(q, qn);
	CHECK(usb_sent == 1 && stored.snmp_read_hash == 0);
	snmp_agent_set_trap_port(0);
	CHECK(snmp_agent_trap_port() == 162);
	c.rfManager = 0;
	snmp_agent_set_config(&c);
}

static void test_rf(void)
{
	uint8_t q[700];
	char text[64];
	snmp_agent_stats_t s0, s1;
	int nb;

	int qn = request(q, "public", 0xA0, 0, 0, SYS_DESCR, sizeof(SYS_DESCR), 1, 0x05, NULL, 0);
	reset_io();
	now_ms += 5000;
	snmp_agent_stats(&s0);
	from_air(3100009, q, qn);
	CHECK(air_sent == 0);	// waits for the host link task
	from_air(3100010, q, qn);	// the mailbox is full: dropped
	air_poll();
	CHECK(air_sent == 1 && air_dst == 3100009);
	// the UDP header: 161 -> the requester's port, its length; the message after it within SNMP_RF_MAX
	CHECK(air_len > 8 && air_len <= 8 + SNMP_RF_MAX && air_buf[0] == 0 && air_buf[1] == 161 && air_buf[2] == (41000 >> 8) &&
	      air_buf[3] == (41000 & 0xFF) && ((air_buf[4] << 8) | air_buf[5]) == air_len);
	CHECK(answer(air_buf + 8, air_len - 8, text, NULL) == 0 && strncmp(text, "RadioFoundry testboard", 22) == 0);
	snmp_agent_stats(&s1);
	CHECK(s1.rf_requests == s0.rf_requests + 2 && s1.rf_answers == s0.rf_answers + 1 && s1.rf_dropped == s0.rf_dropped + 1);

	// Within the answer gap: dropped; after it: answered
	from_air(3100009, q, qn);
	now_ms += 500;
	air_poll();
	CHECK(air_sent == 1);
	from_air(3100009, q, qn);
	now_ms += 600;
	air_poll();
	CHECK(air_sent == 2);

	// Sizes: empty and longer than SNMP_RF_REQ_MAX are dropped at once; exactly SNMP_RF_REQ_MAX is taken (and,
	// being padding after the message, answered)
	snmp_agent_stats(&s0);
	from_air(3100009, q, 0);
	uint8_t big[SNMP_RF_REQ_MAX + 1];
	memset(big, 0, sizeof(big));
	memcpy(big, q, (size_t)qn);
	from_air(3100009, big, sizeof(big));
	snmp_agent_stats(&s1);
	CHECK(s1.rf_dropped == s0.rf_dropped + 2);
	now_ms += 1100;
	from_air(3100009, big, SNMP_RF_REQ_MAX);
	air_poll();
	CHECK(air_sent == 3);

	// A GetBulk of 32 over RF: capped at 8 repetitions and SNMP_RF_MAX
	now_ms += 1100;
	qn = request(q, "public", 0xA5, 0, 32, START, 1, 1, 0x05, NULL, 0);
	from_air(3100009, q, qn);
	air_poll();
	CHECK(air_sent == 4 && air_len <= 8 + SNMP_RF_MAX && answer(air_buf + 8, air_len - 8, NULL, &nb) == 0 && nb <= 8 && nb > 2);

	// SET over RF with the write community: noAccess unless RF writes are on
	now_ms += 1100;
	snmp_agent_set_community(true, "private", 7);
	qn = request(q, "private", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "RFNAME", 6);
	from_air(3100009, q, qn);
	air_poll();
	CHECK(air_sent == 5 && answer(air_buf + 8, air_len - 8, NULL, NULL) == 6 && memcmp(stored.radio_name, "RFNAME", 6) != 0);
	snmpConfig_t c;
	snmp_agent_get_config(&c);
	c.flags |= SNMP_CFG_RF_WRITE;
	snmp_agent_set_config(&c);
	now_ms += 1100;
	from_air(3100009, q, qn);
	air_poll();
	CHECK(air_sent == 6 && answer(air_buf + 8, air_len - 8, NULL, NULL) == 0 && memcmp(stored.radio_name, "RFNAME", 7) == 0);
	c.flags &= (uint8_t)~SNMP_CFG_RF_WRITE;
	snmp_agent_set_config(&c);

	// The air side busy: the answer waits, then is given up after SNMP_RF_GIVE_UP_MS (and the next request waits
	// behind it until then)
	now_ms += 1100;
	qn = request(q, "public", 0xA0, 0, 0, SYS_DESCR, sizeof(SYS_DESCR), 1, 0x05, NULL, 0);
	air_busy = true;
	from_air(3100009, q, qn);
	snmp_agent_poll(now_ms);	// answered into the staging buffer
	snmp_agent_poll(now_ms);	// not taken
	snmp_agent_stats(&s0);
	from_air(3100011, q, qn);	// in the mailbox behind it
	now_ms += SNMP_RF_GIVE_UP_MS - 1;
	snmp_agent_poll(now_ms);
	snmp_agent_stats(&s1);
	CHECK(air_sent == 6 && s1.rf_dropped == s0.rf_dropped);
	now_ms += 1;
	snmp_agent_poll(now_ms);
	snmp_agent_stats(&s1);
	CHECK(air_sent == 6 && s1.rf_dropped == s0.rf_dropped + 1);
	air_busy = false;
	air_poll();	// the waiting request (the gap since the last answer has passed)
	CHECK(air_sent == 7 && air_dst == 3100011);

	// RF answers off: requests over the air dropped; the host still answered
	snmp_agent_get_config(&c);
	c.flags |= SNMP_CFG_RF_OFF;
	snmp_agent_set_config(&c);
	now_ms += 1100;
	from_air(3100009, q, qn);
	air_poll();
	CHECK(air_sent == 7);
	reset_io();
	from_host(q, qn);
	CHECK(usb_sent == 1);
	c.flags &= (uint8_t)~SNMP_CFG_RF_OFF;
	snmp_agent_set_config(&c);
}

static void test_traps_and_off(void)
{
	uint8_t q[700];
	snmpConfig_t c;

	// Authentication failures notify the host when enabled (snmpEnableAuthenTraps), on the trap port
	snmp_agent_get_config(&c);
	c.flags |= SNMP_CFG_AUTH_TRAPS;
	c.rfManager = 0;
	snmp_agent_set_config(&c);
	snmp_agent_set_trap_port(10162);
	// drain what the first polls queue (coldStart)
	for (int i = 0; i < 10; i++) {
		now_ms += SNMP_EVENT_POLL_MS;
		snmp_agent_poll(now_ms);
	}
	reset_io();
	int qn = request(q, "wrong", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	from_host(q, qn);
	CHECK(usb_sent == 0);
	now_ms += SNMP_EVENT_POLL_MS;
	snmp_agent_poll(now_ms);
	CHECK(usb_sent == 1 && usb_dport == 10162 && usb_ip == host_ip && usb_buf[0] == 0x30);
	// The test trap; with the host's link down it isn't kept
	reset_io();
	link_up = false;
	snmp_agent_test_trap();
	now_ms += SNMP_EVENT_POLL_MS;
	snmp_agent_poll(now_ms);
	link_up = true;
	now_ms += SNMP_EVENT_POLL_MS;
	snmp_agent_poll(now_ms);
	CHECK(usb_sent == 0);
	snmp_agent_set_trap_port(162);

	// Switched off: no answers from either side, nothing polled
	snmp_agent_get_config(&c);
	c.flags |= SNMP_CFG_OFF;
	snmp_agent_set_config(&c);
	qn = request(q, "public", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x05, NULL, 0);
	reset_io();
	from_host(q, qn);
	now_ms += 1100;
	from_air(3100009, q, qn);
	snmp_agent_poll(now_ms);
	CHECK(usb_sent == 0 && air_sent == 0);
	c.flags &= (uint8_t)~SNMP_CFG_OFF;
	snmp_agent_set_config(&c);
}

// Mutated requests from the host and the air in exact-size buffers: every answer within its limit and well formed
static void test_fuzz(void)
{
	uint8_t seeds[4][700];
	int seedLen[4];

	seedLen[0] = request(seeds[0], "public", 0xA0, 0, 0, SYS_NAME, sizeof(SYS_NAME), 2, 0x05, NULL, 0);
	seedLen[1] = request(seeds[1], "public", 0xA1, 0, 0, START, 1, 3, 0x05, NULL, 0);
	seedLen[2] = request(seeds[2], "public", 0xA5, 1, 12, START, 1, 2, 0x05, NULL, 0);
	seedLen[3] = request(seeds[3], "private", 0xA3, 0, 0, SYS_NAME, sizeof(SYS_NAME), 1, 0x04, "FUZZ", 4);

	fuzz_t f;
	fuzz_init(&f, 0xA6E7u);
	long iters = fuzz_iters(5000), bad = 0;

	any_id = true;
	uint8_t buf[700];

	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 4);

		memcpy(buf, seeds[s], (size_t)seedLen[s]);
		int n = fuzz_mutate(&f, buf, seedLen[s], (int)sizeof(buf));
		uint8_t *p = fuzz_exact(buf, n);

		reset_io();
		if (fuzz_below(&f, 2) == 0) {
			from_host(p, n);
			if ((usb_sent > 0) && ((usb_len > SNMP_USB_MAX) || (answer(usb_buf, usb_len, NULL, NULL) < 0))) {
				bad++;
			}
		} else {
			now_ms += 1100;
			from_air(3100009, p, n);
			air_poll();
			if ((air_sent > 0) && ((air_len > 8 + SNMP_RF_MAX) || (((air_buf[4] << 8) | air_buf[5]) != air_len) ||
					       (answer(air_buf + 8, air_len - 8, NULL, NULL) < 0))) {
				bad++;
			}
		}
		free(p);
	}
	CHECK(bad == 0);
}

int main(void)
{
	fake_view.dmr.own_id = 3100001;
	strcpy(fake_view.zone_name, "Zone");
	strcpy(fake_view.ch.name, "Chan");
	fake_view.power = 1;
	snmp_agent_init();
	snmp_agent_poll(now_ms);
	test_usb();
	test_set_and_config();
	test_rf();
	test_traps_and_off();
	test_fuzz();
	printf("snmpAgentTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
