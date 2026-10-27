// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_DMR_DATA's IP operations (core/debuglink/debuglink_dmr_data.c, docs/spec/debuglink.md 3.12): IP_SETTINGS with
// and without the appended gateway ID, and IP_SEND's parsing with and without an IPv4 destination. The data service
// and the IP gateway are faked; dmrData.c gives the CAI addresses
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_frame.h"
#include "dmr/dmr_data_api.h"
#include "dmr/dmrData.h"
#include "dmr/ipGateway.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fakes ---

static dmrDataSettings_t settings;
static struct {
	int calls;
	bool group;
	uint32_t dst, src_ip, dst_ip;
	uint8_t ttl, protocol;
	uint8_t l4[8 + DL_REQ_MAX];
	int length;
} air;

void dmr_data_get_settings(dmrDataSettings_t *out) { *out = settings; }
void dmr_data_set_settings(const dmrDataSettings_t *in) { settings = *in; }
uint32_t dmr_data_serial(void) { return 0; }
dmrDataEvent_t dmr_data_last_event(void) { dmrDataEvent_t e; memset(&e, 0, sizeof e); return e; }
dmrArsState_t dmr_data_ars_state(void) { return DMR_ARS_STATE_OFF; }
bool dmr_signal_call_alert(uint32_t dst) { (void)dst; return false; }
bool dmr_signal_radio_check(uint32_t dst) { (void)dst; return false; }
dmr_req_state_t dmr_signal_state(uint8_t *kind, uint32_t *dst) { *kind = 0; *dst = 0; return DMR_REQ_IDLE; }
int sms_inbox_count(void) { return 0; }
int sms_inbox_unread(void) { return 0; }
bool sms_inbox_get(int i, dmrDataMessage_t *out) { (void)i; (void)out; return false; }
void sms_inbox_mark_read(int i) { (void)i; }
bool sms_send(uint32_t dst, bool group, const char *text, bool ack, dmr_sms_format_t f)
{
	(void)dst; (void)group; (void)text; (void)ack; (void)f;
	return false;
}
dmr_req_state_t sms_send_state(void) { return DMR_REQ_IDLE; }
void dmrDataServiceGetStats(dmrDataStats_t *s) { memset(s, 0, sizeof *s); }
void dmrDataServiceResetStats(void) { }
bool dmrDataServiceIsBusy(void) { return false; }
bool dmrDataServiceIpBusy(void) { return false; }
uint8_t dmrDataServiceIpResult(void) { return DMR_REQ_OK; }
int dmrDataServiceLrrpTriggerCount(void) { return 0; }
const char *dmrDataServiceTxKindName(uint8_t kind) { (void)kind; return "other"; }
void dmrDataServiceSetTxSpacing(uint16_t guardMs, bool chain) { (void)guardMs; (void)chain; }

bool ipGatewayIPToAir(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl, uint8_t protocol,
		      const uint8_t *l4, int length)
{
	air.calls++;
	air.group = group;
	air.dst = dst;
	air.src_ip = srcIp;
	air.dst_ip = dstIp;
	air.ttl = ttl;
	air.protocol = protocol;
	air.length = length;
	memcpy(air.l4, l4, (size_t)length);
	return true;
}

// --- helpers ---

static uint8_t out[256];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return debuglink_cmd_dmr_data(req, len, out, sizeof out);
}

// IP_SETTINGS: an older request (11 bytes) leaves the gateway ID, the longer one sets its 24 bits
static void test_ip_settings(void)
{
	uint8_t r[1 + 15];

	memset(&settings, 0, sizeof settings);
	settings.ipGatewayId = 3100500;
	r[0] = DL_DATA_OP_IP_SETTINGS;
	r[1] = DMR_UDPC_AUTO;
	r[2] = DMR_IPC_RULES;
	r[3] = 0;
	put32(r + 4, 1234);
	put32(r + 8, 0);
	CHECK(cmd(r, 1 + 11) == 1 && out[0] == DL_ST_OK);
	CHECK(settings.ipGatewayId == 3100500 && settings.ipConfirmedRule[0] == 1234 &&
	      settings.ipConfirmedTries == DMR_IPC_TRIES_DEFAULT);
	put32(r + 12, 0xFF000000u | 3101276u);
	CHECK(cmd(r, 1 + 15) == 1 && out[0] == DL_ST_OK && settings.ipGatewayId == 3101276u);
	put32(r + 12, 0);
	CHECK(cmd(r, 1 + 15) == 1 && out[0] == DL_ST_OK && settings.ipGatewayId == 0);
	// Between the two lengths: the old request, the gateway ID left alone
	settings.ipGatewayId = 77;
	CHECK(cmd(r, 1 + 13) == 1 && out[0] == DL_ST_OK && settings.ipGatewayId == 77);
	CHECK(cmd(r, 1 + 10) == 1 && out[0] == DL_ST_BAD_ARG);
}

// IP_SEND: the ports little endian in, big endian in the UDP header; flags bit 1 takes a u32 IPv4 destination after
// the ports, else the destination ID's CAI address
static void test_ip_send(void)
{
	uint8_t r[1 + DL_REQ_MAX];
	const uint8_t text[] = { 'h', 'e', 'l', 'l', 'o' };

	memset(&air, 0, sizeof air);
	r[0] = DL_DATA_OP_IP_SEND;
	put32(r + 1, 0xAB000000u | 235u);
	r[5] = 0;
	r[6] = 5016 & 0xFF;
	r[7] = 5016 >> 8;
	r[8] = 161;
	r[9] = 0;
	memcpy(r + 10, text, sizeof text);
	CHECK(cmd(r, 10 + sizeof text) == 1 && out[0] == DL_ST_OK);
	CHECK(air.calls == 1 && !air.group && air.dst == 235 && air.dst_ip == dmrDataIdAddress(false, 235) &&
	      air.src_ip == IPGW_AIR_SRC_OURS && air.ttl == DMR_IP_TTL_DEFAULT && air.protocol == DMR_IP_PROTO_UDP);
	CHECK(air.length == 8 + (int)sizeof text && air.l4[0] == (5016 >> 8) && air.l4[1] == (5016 & 0xFF) &&
	      air.l4[2] == 0 && air.l4[3] == 161 && air.l4[4] == 0 && air.l4[5] == 8 + sizeof text &&
	      memcmp(air.l4 + 8, text, sizeof text) == 0);
	// A talkgroup: its 225.x.y.z
	r[5] = 1;
	put32(r + 1, 9);
	CHECK(cmd(r, 10 + sizeof text) == 1 && out[0] == DL_ST_OK);
	CHECK(air.calls == 2 && air.group && air.dst == 9 && air.dst_ip == dmrDataIdAddress(true, 9));
	// With an IPv4 destination (192.168.10.5, little endian like every u32 here): the payload after it
	put32(r + 1, 3100500);
	r[5] = 2;
	put32(r + 10, 0xC0A80A05u);
	memcpy(r + 14, text, sizeof text);
	CHECK(cmd(r, 14 + sizeof text) == 1 && out[0] == DL_ST_OK);
	CHECK(air.calls == 3 && !air.group && air.dst == 3100500 && air.dst_ip == 0xC0A80A05u &&
	      air.length == 8 + (int)sizeof text && air.l4[5] == 8 + sizeof text &&
	      memcmp(air.l4 + 8, text, sizeof text) == 0);
	// An empty payload is a datagram of 8; flag bit 1 without the address is malformed, as is a short header
	CHECK(cmd(r, 14) == 1 && out[0] == DL_ST_OK && air.calls == 4 && air.length == 8 && air.l4[5] == 8);
	CHECK(cmd(r, 13) == 1 && out[0] == DL_ST_BAD_ARG && air.calls == 4);
	CHECK(cmd(r, 10) == 1 && out[0] == DL_ST_BAD_ARG && air.calls == 4);
	r[5] = 0;
	CHECK(cmd(r, 9) == 1 && out[0] == DL_ST_BAD_ARG && air.calls == 4);
	CHECK(cmd(r, 10) == 1 && out[0] == DL_ST_OK && air.calls == 5 && air.length == 8);
	// The longest payload fits
	memset(r + 14, 'x', DL_REQ_MAX - 14);
	r[5] = 2;
	CHECK(cmd(r, DL_REQ_MAX) == 1 && out[0] == DL_ST_OK && air.calls == 6 && air.length == 8 + DL_REQ_MAX - 14);
}

int main(void)
{
	test_ip_settings();
	test_ip_send();
	printf("# debuglinkDmrDataTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
