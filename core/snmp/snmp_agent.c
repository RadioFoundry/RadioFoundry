// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The SNMP agent's firmware glue (see snmp_agent.h)
#include "config.h"
#if CONFIG_SNMP	// the whole file
#include <string.h>
#include "snmp/snmp_agent.h"
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
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#if CONFIG_HOTSPOT && CONFIG_SFR
#include "hotspot/hotspotSfr.h"
#endif
#if CONFIG_HOTSPOT && CONFIG_HOTSPOT_SPLIT
#include "hotspot/hotspotSplit.h"
#endif
#endif

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

#define USB_MAX_REPETITIONS	32
#define RF_MAX_REPETITIONS	8
#define USB_WAIT_MS		50	// an answer waits this long for the USB IN endpoint
#define UDP_PROTO		17

static snmpData_t data;
static snmpConfig_t cfg;
static snmpStats_t stats;
static snmpMibCtx_t ctx;
static snmpMib_t mib;
static snmpEvents_t events;
static radio_view_t view;		// static: too large for the host link task's stack
static settings_t st;			// the same
static dmrDataMessage_t msg;
static uint8_t out[SNMP_USB_MAX];
static bool started;
static uint16_t trap_port = SNMP_TRAP_PORT;
static uint32_t request_id;
static uint32_t last_event_poll, last_rf_answer, sms_serial;
static snmp_agent_stats_t counts;

// One request over the air, from the radio task to the host link task
static struct {
	volatile uint8_t full;
	uint32_t src;
	uint16_t port;
	uint16_t len;
	uint8_t data[SNMP_RF_REQ_MAX];
} rf_req;

// One datagram (UDP header + message) waiting for the gateway's air side
static struct {
	bool pending;
	uint32_t dst;
	uint32_t since;
	int len;
	uint8_t data[8 + SNMP_RF_MAX];
} rf_out;

static void name_copy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; (i < n) && (src[i] != '\0'); i++) {
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

// The snapshot the MIB and the event check read
static void fill(uint32_t now)
{
	radio_get_view(&view);
	settings_read(&st);
	data.uptimeMs = now;
	data.dmrId = view.dmr.own_id;
	if (st.radio_name[0] != '\0') {
		name_copy(data.name, st.radio_name, SETTINGS_NAME_LEN);
	} else {
		char cp[CODEPLUG_NAME_LEN + 1];

		codeplug_radio_name(cp);
		name_copy(data.name, cp, SNMP_NAME_MAX);
	}
	data.board = board.name;
	data.firmware = ODR_GIT;
	data.batteryMv = view.battery_mv;
	data.batteryPct = view.battery_pct;
	data.batteryState = view.battery;
	data.radioState = (uint8_t)view.state;
	data.rssiDbm = view.rssi_dbm;
	data.mode = (view.ch.mode == RADIO_MODE_DMR) ? 1 : 0;
	data.vfo = view.vfo;
	name_copy(data.zoneName, view.zone_name, SNMP_NAME_MAX);
	data.channelNumber = view.channel_number;
	name_copy(data.channelName, view.ch.name, SNMP_NAME_MAX);
	data.rxHz = view.ch.rx_hz;
	data.txHz = view.ch.tx_hz;
	data.power = view.power;
	data.powerName = ((view.power < board.power_count) && (board.power_names != NULL)) ? board.power_names[view.power]
											     : "";
	data.cc = view.dmr.cc;
	data.ts = view.dmr.ts;
	data.talkTarget = view.dmr.target_id;
	data.talkPrivate = view.dmr.target_private;
	data.role = view.hotspot ? SNMP_ROLE_HOTSPOT : SNMP_ROLE_RADIO;
#if CONFIG_HOTSPOT && CONFIG_SFR
	if (view.hotspot) {
		hotspot_sfr_view_t f;

		hotspot_sfr_get_view(&f);
		if (f.on) {
			data.role = SNMP_ROLE_SFR;
		}
	}
#endif
#if CONFIG_HOTSPOT && CONFIG_HOTSPOT_SPLIT
	if (view.hotspot) {
		hotspot_split_view_t sp;

		hotspot_split_get_view(&sp);
		if (sp.role == SPLIT_ROLE_TX) {
			data.role = SNMP_ROLE_SPLIT_TX;
		} else if (sp.role == SPLIT_ROLE_RX) {
			data.role = SNMP_ROLE_SPLIT_RX;
		}
	}
#endif
	data.dmrState = view.dmr.state;
	data.callSrc = view.dmr.call.src;
	data.callDst = view.dmr.call.dst;
	data.callGroup = view.dmr.call.group;
	data.callEmergency = view.dmr.call.emergency;
	data.lastSrc = view.dmr.last.src;
	data.lastDst = view.dmr.last.dst;
	data.lastGroup = view.dmr.last.group;
	data.lastMs = view.dmr.last_ms;

	dmrDataStats_t ds;

	dmrDataServiceGetStats(&ds);
	data.dataRxPackets = ds.rxPackets;
	data.dataRxForUs = ds.rxForUs;
	data.smsReceived = ds.smsReceived;
	data.smsSent = ds.smsSent;
	data.smsFailed = ds.smsFailed;
	data.smsInbox = (uint8_t)sms_inbox_count();
	data.smsUnread = (uint8_t)sms_inbox_unread();
	data.arsState = (uint8_t)dmr_data_ars_state();
	dmrDataEvent_t ev = dmr_data_last_event();

	data.dataEventSerial = ev.serial;
	data.dataEventKind = ev.kind;
	data.dataEventId = ev.id;
	if ((ev.serial != sms_serial) && (ev.kind == DMR_DATA_EV_SMS_RECEIVED)) {
		data.smsText[0] = '\0';
		if (sms_inbox_get(0, &msg)) {
			name_copy(data.smsText, msg.text, SNMP_TEXT_MAX);
		}
	}
	sms_serial = ev.serial;

	data.usbNetwork = hostlink_network_mode();
	hostlink_net_stats_t ns;

	hostlink_net_stats(&ns);
	data.netToAir = ns.to_air;
	data.netPacketsUp = ns.packets_up;
	ctx.trapsSent = counts.traps_usb + counts.traps_rf;
	ctx.trapsDropped = events.dropped;
}

static void load_config(void)
{
	settings_read(&st);
	cfg.readHash = st.snmp_read_hash;
	cfg.writeHash = st.snmp_write_hash;
	cfg.rfManager = (uint32_t)st.snmp_rf_manager[0] | ((uint32_t)st.snmp_rf_manager[1] << 8) |
			((uint32_t)st.snmp_rf_manager[2] << 16);
	cfg.rfTrapGapS = st.snmp_rf_gap_s;
	cfg.flags = st.snmp_flags;
}

static void save_config(void)
{
	settings_t *s = settings_begin();

	s->snmp_read_hash = cfg.readHash;
	s->snmp_write_hash = cfg.writeHash;
	s->snmp_rf_manager[0] = (uint8_t)cfg.rfManager;
	s->snmp_rf_manager[1] = (uint8_t)(cfg.rfManager >> 8);
	s->snmp_rf_manager[2] = (uint8_t)(cfg.rfManager >> 16);
	s->snmp_rf_gap_s = cfg.rfTrapGapS;
	s->snmp_flags = cfg.flags;
	if (ctx.nameChanged) {
		memset(s->radio_name, 0, sizeof(s->radio_name));
		memcpy(s->radio_name, ctx.newName, strlen(ctx.newName));
	}
	settings_end(hal_millis());
	ctx.cfgChanged = ctx.nameChanged = false;
}

static int process(bool rf, const uint8_t *req, int len, uint8_t *resp, int cap)
{
	snmpRequestCfg_t rc = { .mib = &mib, .readHash = cfg.readHash, .writeHash = cfg.writeHash,
				.writeAllowed = !rf || (cfg.flags & SNMP_CFG_RF_WRITE),
				.maxRepetitions = rf ? RF_MAX_REPETITIONS : USB_MAX_REPETITIONS, .stats = &stats };
	bool auth;

	fill(hal_millis());
	int n = snmpProcess(&rc, req, len, resp, cap, &auth);

	if (auth && (cfg.flags & SNMP_CFG_AUTH_TRAPS)) {
		snmpEvent_t ev = { .kind = SNMP_EV_AUTH_FAIL, .atMs = hal_millis() };

		snmpEventsPush(&events, &ev, &cfg);	// to the host only (never over RF)
	}
	if (ctx.cfgChanged || ctx.nameChanged) {
		save_config();
	}
	return n;
}

// To the host: retried while the IN endpoint is busy, up to USB_WAIT_MS
static bool usb_send(uint32_t ip, uint16_t dst_port, const uint8_t *p, int n)
{
	uint32_t t0 = hal_millis();

	while (!ipGatewayUdpSend(ip, SNMP_PORT, dst_port, p, n)) {
		if ((board.netif == NULL) || !board.netif->up() || ((hal_millis() - t0) >= USB_WAIT_MS)) {
			return false;
		}
		hal_delay_ms(1);
		board.netif->tick(hal_millis());
	}
	return true;
}

// The gateway's local port 161: from the host (host link task) or the air (radio task)
static void udp_in(bool from_air, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, const uint8_t *p, int len)
{
	(void)dst;
	(void)dst_port;
	if (cfg.flags & SNMP_CFG_OFF) {
		return;
	}
	if (from_air) {
		counts.rf_requests++;
		if ((cfg.flags & SNMP_CFG_RF_OFF) || rf_req.full || (len <= 0) || (len > SNMP_RF_REQ_MAX)) {
			counts.rf_dropped++;
			return;
		}
		rf_req.src = src;
		rf_req.port = src_port;
		rf_req.len = (uint16_t)len;
		memcpy(rf_req.data, p, (size_t)len);
		__sync_synchronize();	// complete before the flag says so
		rf_req.full = 1;
		return;
	}
	int n = process(false, p, len, out, sizeof(out));

	if (n > 0) {
		usb_send(src, src_port, out, n);
	}
}

// A message (already at rf_out.data + 8) to dst's port over the air
static void rf_stage(uint32_t dst, uint16_t dst_port, int n, uint32_t now)
{
	uint8_t *u = rf_out.data;

	u[0] = SNMP_PORT >> 8;
	u[1] = SNMP_PORT & 0xFF;
	u[2] = (uint8_t)(dst_port >> 8);
	u[3] = (uint8_t)dst_port;
	u[4] = (uint8_t)((8 + n) >> 8);
	u[5] = (uint8_t)(8 + n);
	u[6] = u[7] = 0;	// the checksum: worked out when the IP packet is built (dmrDataBuildIP)
	rf_out.dst = dst;
	rf_out.len = 8 + n;
	rf_out.since = now;
	rf_out.pending = true;
}

static void rf_poll(uint32_t now)
{
	if (rf_out.pending) {
		if (ipGatewayIPToAir(false, rf_out.dst, UDP_PROTO, rf_out.data, rf_out.len)) {
			rf_out.pending = false;
		} else if ((now - rf_out.since) >= SNMP_RF_GIVE_UP_MS) {
			rf_out.pending = false;
			counts.rf_dropped++;
		}
		return;	// one at a time
	}
	if (rf_req.full) {
		__sync_synchronize();
		bool gap = (last_rf_answer != 0) && ((now - last_rf_answer) < SNMP_RF_ANSWER_GAP_MS);
		int n = gap ? 0 : process(true, rf_req.data, rf_req.len, rf_out.data + 8, SNMP_RF_MAX);

		if (n > 0) {
			rf_stage(rf_req.src, rf_req.port, n, now);
			last_rf_answer = (now != 0) ? now : 1;
			counts.rf_answers++;
		} else if (gap) {
			counts.rf_dropped++;
		}
		rf_req.full = 0;
		return;
	}
	snmpEvent_t *ev = snmpEventsNext(&events, SNMP_TARGET_RF);

	if (ev == NULL) {
		return;
	}
	if ((cfg.rfManager == 0) || !snmpEventsRfAllowed(&events, ev, &cfg, now)) {
		snmpEventsDone(&events, ev, SNMP_TARGET_RF, true);	// the rate limit: dropped
		return;
	}
	int n = snmpEventEncode(ev, (int32_t)++request_id, rf_out.data + 8, SNMP_RF_MAX);

	if (n > 0) {
		rf_stage(cfg.rfManager, SNMP_TRAP_PORT, n, now);
		snmpEventsRfSent(&events, now);
		counts.traps_rf++;
	}
	snmpEventsDone(&events, ev, SNMP_TARGET_RF, n <= 0);
}

static void usb_traps(void)
{
	snmpEvent_t *ev = snmpEventsNext(&events, SNMP_TARGET_USB);

	if (ev == NULL) {
		return;
	}
	uint32_t host = ipGatewayHostIP();

	if (!hostlink_network_mode() || (board.netif == NULL) || !board.netif->up() || (host == 0)) {
		snmpEventsDone(&events, ev, SNMP_TARGET_USB, false);	// no host to tell: not kept
		return;
	}
	int n = snmpEventEncode(ev, (int32_t)++request_id, out, sizeof(out));

	if ((n > 0) && !usb_send(host, trap_port, out, n)) {
		return;	// busy: next pass
	}
	if (n > 0) {
		counts.traps_usb++;
	}
	snmpEventsDone(&events, ev, SNMP_TARGET_USB, n <= 0);
}

void snmp_agent_init(void)
{
	load_config();
	ctx.data = &data;
	ctx.cfg = &cfg;
	ctx.stats = &stats;
	snmpMibInit(&mib, &ctx);
	snmpEventsInit(&events);
	ipGatewayUdpBindAir(SNMP_PORT, udp_in);	// the link and the air (read-only, rate limited there)
	started = true;
}

void snmp_agent_poll(uint32_t now_ms)
{
	if (!started || (cfg.flags & SNMP_CFG_OFF)) {
		return;
	}
	if ((now_ms - last_event_poll) >= SNMP_EVENT_POLL_MS) {
		last_event_poll = now_ms;
		fill(now_ms);
		snmpEventsDetect(&events, &data, &cfg);
		usb_traps();
	}
	rf_poll(now_ms);
}

void snmp_agent_stats(snmp_agent_stats_t *s)
{
	counts.snmp = stats;
	counts.traps_dropped = events.dropped;
	*s = counts;
}

void snmp_agent_get_config(snmpConfig_t *c)
{
	*c = cfg;
}

void snmp_agent_set_config(const snmpConfig_t *c)
{
	cfg = *c;
	save_config();
}

void snmp_agent_set_community(bool write, const char *text, int len)
{
	uint32_t h = (len > 0) ? snmpCommunityHash((const uint8_t *)text, len) : 0;

	if (write) {
		cfg.writeHash = h;
	} else {
		cfg.readHash = h;
	}
	save_config();
}

void snmp_agent_set_trap_port(uint16_t port)
{
	trap_port = (port != 0) ? port : SNMP_TRAP_PORT;
}

uint16_t snmp_agent_trap_port(void)
{
	return trap_port;
}

void snmp_agent_test_trap(void)
{
	snmpEvent_t ev = { .kind = SNMP_EV_COLD_START, .atMs = hal_millis() };

	snmpEventsPush(&events, &ev, &cfg);
}

#endif	// CONFIG_SNMP
