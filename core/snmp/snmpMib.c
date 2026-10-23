// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's MIB and its notifications (see snmpMib.h). Object names and numbers: RFC 3418 (system, snmp),
// docs/spec/ODR-MIB.txt (the enterprise subtree).
#include <stddef.h>
#include <string.h>
#include "snmp/snmpMib.h"
#include "dmr/dmrDataService.h"

// The enterprise number as OID contents (base 128, X.690 8.19)
#if ODR_SNMP_PEN >= 2097152u
#error "ODR_SNMP_PEN: at most 21 bits here"
#elif ODR_SNMP_PEN >= 16384u
#define PEN_BER		(0x80u | ((ODR_SNMP_PEN >> 14) & 0x7Fu)), (0x80u | ((ODR_SNMP_PEN >> 7) & 0x7Fu)), (ODR_SNMP_PEN & 0x7Fu)
#define PEN_LEN		3
#elif ODR_SNMP_PEN >= 128u
#define PEN_BER		(0x80u | ((ODR_SNMP_PEN >> 7) & 0x7Fu)), (ODR_SNMP_PEN & 0x7Fu)
#define PEN_LEN		2
#else
#define PEN_BER		ODR_SNMP_PEN
#define PEN_LEN		1
#endif

#define TRUTH(b)	((b) ? 1u : 2u)		// TruthValue (RFC 2579); also up(1) / down(2)
#define SYS_SERVICES	(4u + 64u)		// internet (routes IP over DMR) and applications (RFC 3418)

// The prefixes (each led by its length): system, snmp (RFC 3418) and odrObjects. The interfaces group is left out
// for the flash it would cost (the smallest images); odrNet has the link's counters
enum { P_SYSTEM, P_SNMP, P_ODR };
static const uint8_t PFX_SYSTEM[] = { 6, 0x2B, 6, 1, 2, 1, 1 };
static const uint8_t PFX_SNMP[] = { 6, 0x2B, 6, 1, 2, 1, 11 };
static const uint8_t PFX_ODR[] = { 7 + PEN_LEN, 0x2B, 6, 1, 4, 1, PEN_BER, 2, 1 };
static const uint8_t *const prefixes[] = { PFX_SYSTEM, PFX_SNMP, PFX_ODR };

// The instances in OID order; the enum is their index (the id the callbacks get)
enum {
	SYS_DESCR, SYS_OBJECT_ID, SYS_UPTIME, SYS_CONTACT, SYS_NAME, SYS_LOCATION, SYS_SERVICES_ID,
	SN_IN_PKTS, SN_BAD_VERSIONS, SN_BAD_NAMES, SN_BAD_USES, SN_ASN_ERRS, SN_AUTH_TRAPS, SN_SILENT_DROPS, SN_PROXY_DROPS,
	ID_DMR_ID, ID_CALLSIGN, ID_BOARD, ID_FIRMWARE,
	PW_MV, PW_PCT, PW_STATE,
	RD_STATE, RD_RSSI, RD_MODE, RD_ZONE, RD_CH_NUMBER, RD_CH_NAME, RD_RX_HZ, RD_TX_HZ, RD_POWER, RD_POWER_NAME, RD_CC,
	RD_TS, RD_TARGET, RD_TARGET_TYPE, RD_ROLE,
	DM_STATE, DM_SRC, DM_DST, DM_LAST_SRC, DM_LAST_DST, DM_LAST_GROUP, DM_LAST_AGE,
	DA_RX_PACKETS, DA_RX_FOR_US, DA_SMS_RX, DA_SMS_TX, DA_SMS_FAILED, DA_INBOX, DA_UNREAD, DA_ARS,
	NT_USB, NT_TO_AIR, NT_PACKETS_UP,
	AG_RF_MANAGER, AG_RF_GAP, AG_RF_ANSWERS, AG_RF_WRITE, AG_USB_TRAPS, AG_RF_CALL_TRAPS, AG_TRAPS_SENT, AG_TRAPS_DROPPED,
	OBJECT_COUNT
};

#define R		0
#define W		SNMP_OBJ_WRITE
#define INT		BER_INTEGER
#define STR		BER_OCTETS
#define U32		BER_GAUGE32
#define CNT		BER_COUNTER32
#define SYS(n, t, f)	{ P_SYSTEM, { n, 0 }, 2, t, f }
#define SNMPG(n, t, f)	{ P_SNMP, { n, 0 }, 2, t, f }
#define ODR(g, n, t, f)	{ P_ODR, { g, n, 0 }, 3, t, f }

// Sorted by OID (tests/host/snmpTest.c checks it)
static const snmpObject_t objects[OBJECT_COUNT] = {
	[SYS_DESCR] = SYS(1, STR, R),
	[SYS_OBJECT_ID] = SYS(2, BER_OID, R),
	[SYS_UPTIME] = SYS(3, BER_TIMETICKS, R),
	[SYS_CONTACT] = SYS(4, STR, R),
	[SYS_NAME] = SYS(5, STR, W),
	[SYS_LOCATION] = SYS(6, STR, R),
	[SYS_SERVICES_ID] = SYS(7, INT, R),
	[SN_IN_PKTS] = SNMPG(1, CNT, R),
	[SN_BAD_VERSIONS] = SNMPG(3, CNT, R),
	[SN_BAD_NAMES] = SNMPG(4, CNT, R),
	[SN_BAD_USES] = SNMPG(5, CNT, R),
	[SN_ASN_ERRS] = SNMPG(6, CNT, R),
	[SN_AUTH_TRAPS] = SNMPG(30, INT, W),
	[SN_SILENT_DROPS] = SNMPG(31, CNT, R),
	[SN_PROXY_DROPS] = SNMPG(32, CNT, R),
	[ID_DMR_ID] = ODR(1, 1, U32, R),
	[ID_CALLSIGN] = ODR(1, 2, STR, R),
	[ID_BOARD] = ODR(1, 3, STR, R),
	[ID_FIRMWARE] = ODR(1, 4, STR, R),
	[PW_MV] = ODR(2, 1, U32, R),
	[PW_PCT] = ODR(2, 2, U32, R),
	[PW_STATE] = ODR(2, 3, INT, R),
	[RD_STATE] = ODR(3, 1, INT, R),
	[RD_RSSI] = ODR(3, 2, INT, R),
	[RD_MODE] = ODR(3, 3, INT, R),
	[RD_ZONE] = ODR(3, 4, STR, R),
	[RD_CH_NUMBER] = ODR(3, 5, U32, R),
	[RD_CH_NAME] = ODR(3, 6, STR, R),
	[RD_RX_HZ] = ODR(3, 7, U32, R),
	[RD_TX_HZ] = ODR(3, 8, U32, R),
	[RD_POWER] = ODR(3, 9, U32, R),
	[RD_POWER_NAME] = ODR(3, 10, STR, R),
	[RD_CC] = ODR(3, 11, U32, R),
	[RD_TS] = ODR(3, 12, U32, R),
	[RD_TARGET] = ODR(3, 13, U32, R),
	[RD_TARGET_TYPE] = ODR(3, 14, INT, R),
	[RD_ROLE] = ODR(3, 15, INT, R),
	[DM_STATE] = ODR(4, 1, INT, R),
	[DM_SRC] = ODR(4, 2, U32, R),
	[DM_DST] = ODR(4, 3, U32, R),
	[DM_LAST_SRC] = ODR(4, 4, U32, R),
	[DM_LAST_DST] = ODR(4, 5, U32, R),
	[DM_LAST_GROUP] = ODR(4, 6, INT, R),
	[DM_LAST_AGE] = ODR(4, 7, BER_TIMETICKS, R),
	[DA_RX_PACKETS] = ODR(5, 1, CNT, R),
	[DA_RX_FOR_US] = ODR(5, 2, CNT, R),
	[DA_SMS_RX] = ODR(5, 3, CNT, R),
	[DA_SMS_TX] = ODR(5, 4, CNT, R),
	[DA_SMS_FAILED] = ODR(5, 5, CNT, R),
	[DA_INBOX] = ODR(5, 6, U32, R),
	[DA_UNREAD] = ODR(5, 7, U32, R),
	[DA_ARS] = ODR(5, 8, INT, R),
	[NT_USB] = ODR(6, 1, INT, R),
	[NT_TO_AIR] = ODR(6, 3, CNT, R),
	[NT_PACKETS_UP] = ODR(6, 6, CNT, R),
	[AG_RF_MANAGER] = ODR(7, 1, U32, W),
	[AG_RF_GAP] = ODR(7, 2, U32, W),
	[AG_RF_ANSWERS] = ODR(7, 3, INT, W),
	[AG_RF_WRITE] = ODR(7, 4, INT, W),
	[AG_USB_TRAPS] = ODR(7, 5, INT, W),
	[AG_RF_CALL_TRAPS] = ODR(7, 6, INT, W),
	[AG_TRAPS_SENT] = ODR(7, 7, CNT, R),
	[AG_TRAPS_DROPPED] = ODR(7, 8, CNT, R),
};

// Where each value comes from: a kind and an offset (into snmpData_t, snmpStats_t, or a small constant)
enum { K_SPECIAL, K_CONST, K_U8, K_U16, K_U32, K_I16, K_ENUM, K_TRUTH, K_STR, K_PSTR, K_STAT };
#define K_DMR		0x80u	// no instance unless the channel is DMR
#define D(k, f)		{ k, offsetof(snmpData_t, f) }
#define ST(f)		{ K_STAT, offsetof(snmpStats_t, f) }
#define C(v)		{ K_CONST, v }
_Static_assert(offsetof(snmpData_t, smsText) <= 255, "snmpData_t: the MIB's fields must be within 255 octets");

static const uint8_t fields[OBJECT_COUNT][2] = {
	[SYS_SERVICES_ID] = C(SYS_SERVICES), [SYS_NAME] = D(K_STR, name),
	[SN_IN_PKTS] = ST(inPkts), [SN_BAD_VERSIONS] = ST(inBadVersions), [SN_BAD_NAMES] = ST(inBadCommunityNames),
	[SN_BAD_USES] = ST(inBadCommunityUses), [SN_ASN_ERRS] = ST(inASNParseErrs), [SN_SILENT_DROPS] = ST(silentDrops),
	[SN_PROXY_DROPS] = C(0),
	[ID_DMR_ID] = D(K_U32, dmrId), [ID_CALLSIGN] = D(K_STR, name), [ID_BOARD] = D(K_PSTR, board),
	[ID_FIRMWARE] = D(K_PSTR, firmware),
	[PW_MV] = D(K_U16, batteryMv), [PW_PCT] = D(K_U8, batteryPct), [PW_STATE] = D(K_ENUM, batteryState),
	[RD_STATE] = D(K_ENUM, radioState), [RD_RSSI] = D(K_I16, rssiDbm), [RD_MODE] = D(K_ENUM, mode),
	[RD_ZONE] = D(K_STR, zoneName), [RD_CH_NUMBER] = D(K_U16, channelNumber), [RD_CH_NAME] = D(K_STR, channelName),
	[RD_RX_HZ] = D(K_U32, rxHz), [RD_TX_HZ] = D(K_U32, txHz), [RD_POWER] = D(K_U8, power),
	[RD_POWER_NAME] = D(K_PSTR, powerName), [RD_CC] = D(K_DMR | K_U8, cc), [RD_TS] = D(K_DMR | K_U8, ts),
	[RD_TARGET] = D(K_DMR | K_U32, talkTarget), [RD_ROLE] = D(K_ENUM, role),
	[DM_STATE] = D(K_ENUM, dmrState), [DM_LAST_SRC] = D(K_U32, lastSrc), [DM_LAST_DST] = D(K_U32, lastDst),
	[DM_LAST_GROUP] = D(K_TRUTH, lastGroup),
	[DA_RX_PACKETS] = D(K_U32, dataRxPackets), [DA_RX_FOR_US] = D(K_U32, dataRxForUs),
	[DA_SMS_RX] = D(K_U32, smsReceived), [DA_SMS_TX] = D(K_U32, smsSent), [DA_SMS_FAILED] = D(K_U32, smsFailed),
	[DA_INBOX] = D(K_U8, smsInbox), [DA_UNREAD] = D(K_U8, smsUnread), [DA_ARS] = D(K_ENUM, arsState),
	[NT_USB] = D(K_TRUTH, usbNetwork), [NT_TO_AIR] = D(K_U32, netToAir), [NT_PACKETS_UP] = D(K_U32, netPacketsUp),
};

// sysObjectID: odrProducts.1, an ODR radio
static const uint8_t PRODUCT_OID[] = { 0x2B, 6, 1, 4, 1, PEN_BER, 1, 1 };

static void str(snmpValue_t *v, const char *s)
{
	v->ptr = (const uint8_t *)((s != NULL) ? s : "");
	v->len = (uint16_t)strlen((const char *)v->ptr);
}

static char *append(char *p, const char *end, const char *s)
{
	while ((s != NULL) && (*s != '\0') && (p < end)) {
		*p++ = *s++;
	}
	return p;
}

static uint32_t load(const uint8_t *p, int size)
{
	uint32_t u = 0;
	uint16_t h;

	if (size == 4) {
		memcpy(&u, p, 4);
	} else {
		memcpy(&h, p, 2);
		u = h;
	}
	return u;
}

static bool get(const snmpMib_t *mib, uint8_t id, snmpValue_t *v)
{
	snmpMibCtx_t *c = mib->ctx;
	const snmpData_t *d = c->data;
	const snmpConfig_t *cfg = c->cfg;
	const uint8_t *base = (const uint8_t *)d;
	uint8_t kind = fields[id][0], off = fields[id][1];
	uint32_t n;

	if ((kind & K_DMR) && (d->mode != 1)) {
		return false;	// DMR only
	}
	switch (kind & 0x7Fu) {
	case K_CONST:
		n = off;
		break;
	case K_U8:
		n = base[off];
		break;
	case K_U16:
		n = load(base + off, 2);
		break;
	case K_U32:
		n = load(base + off, 4);
		break;
	case K_I16:
		n = (uint32_t)(int32_t)(int16_t)load(base + off, 2);
		break;
	case K_ENUM:
		n = base[off] + 1u;	// SMIv2 enumerations start at 1
		break;
	case K_TRUTH:
		n = TRUTH(base[off]);
		break;
	case K_STR:
		str(v, (const char *)base + off);
		return true;
	case K_PSTR: {
		const char *s;

		memcpy(&s, base + off, sizeof(s));
		str(v, s);
		return true;
	}
	case K_STAT:
		n = load((const uint8_t *)c->stats + off, 4);
		break;
	default:
		switch (id) {
		case SYS_DESCR: {
			char *p = c->text, *end = c->text + sizeof(c->text) - 1;

			p = append(p, end, "OpenDigitalRadio ");
			p = append(p, end, d->board);
			p = append(p, end, " ");
			p = append(p, end, d->firmware);
			*p = '\0';
			str(v, c->text);
			return true;
		}
		case SYS_OBJECT_ID:
			v->ptr = PRODUCT_OID;
			v->len = sizeof(PRODUCT_OID);
			return true;
		case SYS_UPTIME:
			n = d->uptimeMs / 10u;
			break;
		case SN_AUTH_TRAPS:
			n = (cfg->flags & SNMP_CFG_AUTH_TRAPS) ? 1u : 2u;	// enabled, disabled
			break;
		case RD_TARGET_TYPE:
			if (d->mode != 1) {
				return false;
			}
			n = d->talkPrivate ? 2u : 1u;
			break;
		case DM_SRC:
			n = (d->dmrState != 0) ? d->callSrc : 0;
			break;
		case DM_DST:
			n = (d->dmrState != 0) ? d->callDst : 0;
			break;
		case DM_LAST_AGE:
			if (d->lastMs == 0) {
				return false;	// no call heard yet
			}
			n = (d->uptimeMs - d->lastMs) / 10u;
			break;
		case AG_RF_MANAGER:
			n = cfg->rfManager;
			break;
		case AG_RF_GAP:
			n = (cfg->rfTrapGapS != 0) ? cfg->rfTrapGapS : SNMP_RF_TRAP_GAP_DEFAULT;
			break;
		case AG_RF_ANSWERS:
			n = TRUTH(!(cfg->flags & SNMP_CFG_RF_OFF));
			break;
		case AG_RF_WRITE:
			n = TRUTH(cfg->flags & SNMP_CFG_RF_WRITE);
			break;
		case AG_USB_TRAPS:
			n = TRUTH(!(cfg->flags & SNMP_CFG_USB_TRAPS_OFF));
			break;
		case AG_RF_CALL_TRAPS:
			n = TRUTH(cfg->flags & SNMP_CFG_RF_CALL_TRAPS);
			break;
		case AG_TRAPS_SENT:
			n = c->trapsSent;
			break;
		case AG_TRAPS_DROPPED:
			n = c->trapsDropped;
			break;
		default:	// sysContact, sysLocation
			str(v, "");
			return true;
		}
		break;
	}
	v->num = n;
	return true;
}

static uint8_t test(const snmpMib_t *mib, uint8_t id, const snmpValue_t *v)
{
	(void)mib;
	switch (id) {
	case SYS_NAME:
		if (v->len > SNMP_NAME_MAX) {
			return SNMP_ERR_WRONG_LENGTH;
		}
		for (int i = 0; i < v->len; i++) {
			if ((v->ptr[i] < 0x20) || (v->ptr[i] > 0x7E)) {
				return SNMP_ERR_WRONG_VALUE;	// printable ASCII: the radio's fonts and the talker alias
			}
		}
		return SNMP_ERR_NONE;
	case AG_RF_MANAGER:
		return (v->num <= 0xFFFFFFu) ? SNMP_ERR_NONE : SNMP_ERR_WRONG_VALUE;
	case AG_RF_GAP:
		return ((v->num >= 10u) && (v->num <= 3600u)) ? SNMP_ERR_NONE : SNMP_ERR_WRONG_VALUE;
	default:	// TruthValue, snmpEnableAuthenTraps: 1 or 2
		return ((v->num == 1u) || (v->num == 2u)) ? SNMP_ERR_NONE : SNMP_ERR_WRONG_VALUE;
	}
}

static void flag(snmpConfig_t *cfg, uint8_t f, bool on)
{
	cfg->flags = on ? (uint8_t)(cfg->flags | f) : (uint8_t)(cfg->flags & ~f);
}

static void commit(const snmpMib_t *mib, uint8_t id, const snmpValue_t *v)
{
	snmpMibCtx_t *c = mib->ctx;
	snmpConfig_t *cfg = c->cfg;
	bool one = v->num == 1u;

	if (id == SYS_NAME) {
		memcpy(c->newName, v->ptr, v->len);
		c->newName[v->len] = '\0';
		memcpy(c->data->name, c->newName, sizeof(c->newName));
		c->nameChanged = true;
		return;
	}
	switch (id) {
	case AG_RF_MANAGER:
		cfg->rfManager = v->num;
		break;
	case AG_RF_GAP:
		cfg->rfTrapGapS = (uint16_t)v->num;
		break;
	case SN_AUTH_TRAPS:
		flag(cfg, SNMP_CFG_AUTH_TRAPS, one);
		break;
	case AG_RF_ANSWERS:
		flag(cfg, SNMP_CFG_RF_OFF, !one);
		break;
	case AG_RF_WRITE:
		flag(cfg, SNMP_CFG_RF_WRITE, one);
		break;
	case AG_USB_TRAPS:
		flag(cfg, SNMP_CFG_USB_TRAPS_OFF, !one);
		break;
	case AG_RF_CALL_TRAPS:
		flag(cfg, SNMP_CFG_RF_CALL_TRAPS, one);
		break;
	default:
		return;
	}
	c->cfgChanged = true;
}

void snmpMibInit(snmpMib_t *mib, snmpMibCtx_t *ctx)
{
	mib->objects = objects;
	mib->count = OBJECT_COUNT;
	mib->prefixes = prefixes;
	mib->get = get;
	mib->test = test;
	mib->commit = commit;
	mib->ctx = ctx;
}

// ---- Notifications ----

static bool inCall(uint8_t s)
{
	return (s == 1) || (s == 2) || (s == 4) || (s == 5);	// RX, RX of another target, TX, TX ending
}

void snmpEventsInit(snmpEvents_t *e)
{
	memset(e, 0, sizeof(*e));
}

void snmpEventsPush(snmpEvents_t *e, const snmpEvent_t *ev, const snmpConfig_t *cfg)
{
	uint8_t t = 0;
	bool call = (ev->kind == SNMP_EV_CALL_START) || (ev->kind == SNMP_EV_CALL_END);

	if (cfg->flags & SNMP_CFG_OFF) {
		return;
	}
	if (!(cfg->flags & SNMP_CFG_USB_TRAPS_OFF)) {
		t |= SNMP_TARGET_USB;
	}
	if ((cfg->rfManager != 0) && (ev->kind != SNMP_EV_AUTH_FAIL) && (!call || (cfg->flags & SNMP_CFG_RF_CALL_TRAPS))) {
		t |= SNMP_TARGET_RF;
	}
	if (t == 0) {
		return;
	}
	if (e->count == SNMP_EVENT_QUEUE) {
		memmove(&e->q[0], &e->q[1], sizeof(e->q[0]) * (SNMP_EVENT_QUEUE - 1));	// the oldest goes
		e->count--;
		e->dropped++;
	}
	e->q[e->count] = *ev;
	e->q[e->count].targets = t;
	e->q[e->count].rfSinceMs = 0;
	e->count++;
}

snmpEvent_t *snmpEventsNext(snmpEvents_t *e, uint8_t target)
{
	for (int i = 0; i < e->count; i++) {
		if (e->q[i].targets & target) {
			return &e->q[i];
		}
	}
	return NULL;
}

void snmpEventsDone(snmpEvents_t *e, snmpEvent_t *ev, uint8_t target, bool dropped)
{
	int i = (int)(ev - e->q);

	if ((i < 0) || (i >= e->count)) {
		return;
	}
	ev->targets &= (uint8_t)~target;
	if (dropped) {
		e->dropped++;
	}
	if (ev->targets == 0) {
		memmove(&e->q[i], &e->q[i + 1], sizeof(e->q[0]) * (size_t)(e->count - i - 1));
		e->count--;
	}
}

bool snmpEventsRfAllowed(const snmpEvents_t *e, const snmpEvent_t *ev, const snmpConfig_t *cfg, uint32_t nowMs)
{
	uint32_t gap = (cfg->rfTrapGapS != 0) ? cfg->rfTrapGapS : SNMP_RF_TRAP_GAP_DEFAULT;

	return (ev->kind == SNMP_EV_EMERGENCY) || (e->lastRfMs == 0) || ((nowMs - e->lastRfMs) >= (gap * 1000u));
}

void snmpEventsRfSent(snmpEvents_t *e, uint32_t nowMs)
{
	e->lastRfMs = (nowMs != 0) ? nowMs : 1;
}

void snmpEventsDetect(snmpEvents_t *e, const snmpData_t *d, const snmpConfig_t *cfg)
{
	snmpEvent_t ev;

	memset(&ev, 0, sizeof(ev));
	ev.atMs = d->uptimeMs;
	if (!e->started) {
		e->started = true;
		e->batteryState = d->batteryState;
		e->arsState = d->arsState;
		e->dataEventSerial = d->dataEventSerial;
		ev.kind = SNMP_EV_COLD_START;
		snmpEventsPush(e, &ev, cfg);
	}

	// Calls: a start when one begins (or another follows straight on), an end with its duration
	bool was = inCall(e->dmrState), is = inCall(d->dmrState);
	bool other = was && is && ((d->callSrc != e->callSrc) || (d->callDst != e->callDst) || (d->callGroup != e->callGroup));

	if (was && (!is || other)) {
		ev.kind = SNMP_EV_CALL_END;
		ev.src = e->callSrc;
		ev.dst = e->callDst;
		ev.group = e->callGroup;
		ev.slot = e->callSlot;
		ev.durationMs = d->uptimeMs - e->callStartMs;
		snmpEventsPush(e, &ev, cfg);
	}
	if (is && (!was || other)) {
		e->callSrc = d->callSrc;
		e->callDst = d->callDst;
		e->callGroup = d->callGroup;
		e->callSlot = d->ts;
		e->callStartMs = d->uptimeMs;
		ev.kind = SNMP_EV_CALL_START;
		ev.src = d->callSrc;
		ev.dst = d->callDst;
		ev.group = d->callGroup;
		ev.slot = d->ts;
		ev.durationMs = 0;
		snmpEventsPush(e, &ev, cfg);
		if (d->callEmergency) {
			ev.kind = SNMP_EV_EMERGENCY;
			snmpEventsPush(e, &ev, cfg);
		}
	}
	e->dmrState = d->dmrState;
	memset(&ev, 0, sizeof(ev));
	ev.atMs = d->uptimeMs;

	if ((d->batteryState > e->batteryState) && (d->batteryState != 0)) {
		ev.kind = SNMP_EV_LOW_BATTERY;
		ev.value = d->batteryMv;
		snmpEventsPush(e, &ev, cfg);
	}
	e->batteryState = d->batteryState;

	if (d->arsState != e->arsState) {
		ev.kind = SNMP_EV_ARS;
		ev.value = d->arsState + 1;
		snmpEventsPush(e, &ev, cfg);
		e->arsState = d->arsState;
	}

	if (d->dataEventSerial != e->dataEventSerial) {
		e->dataEventSerial = d->dataEventSerial;
		ev.value = 0;
		ev.src = d->dataEventId;
		ev.dst = d->dmrId;
		if (d->dataEventKind == DMR_DATA_EV_SMS_RECEIVED) {
			ev.kind = SNMP_EV_SMS;
			memcpy(ev.text, d->smsText, sizeof(ev.text));
			snmpEventsPush(e, &ev, cfg);
		} else if (d->dataEventKind == DMR_DATA_EV_EMERGENCY_ALARM) {
			ev.kind = SNMP_EV_EMERGENCY;
			ev.dst = 0;
			snmpEventsPush(e, &ev, cfg);
		}
	}
}

// Notification OIDs: SNMPv2-MIB's coldStart and authenticationFailure (odrNotifications.n for the others)
static const uint8_t TRAP_COLD_START[] = { 0x2B, 6, 1, 6, 3, 1, 1, 5, 1 };
static const uint8_t TRAP_AUTH_FAIL[] = { 0x2B, 6, 1, 6, 3, 1, 1, 5, 5 };

// Per kind: the odrNotifications number (0: an SNMPv2-MIB one), how many objects, and which (odrEvent.n: 1 source,
// 2 destination, 3 group, 4 slot, 5 duration, 6 text, 7 value)
static const uint8_t EVENT_OBJECTS[][7] = {
	[SNMP_EV_COLD_START] = { 0, 0 },
	[SNMP_EV_AUTH_FAIL] = { 0, 0 },
	[SNMP_EV_CALL_START] = { 1, 4, 1, 2, 3, 4 },
	[SNMP_EV_CALL_END] = { 2, 5, 1, 2, 3, 4, 5 },
	[SNMP_EV_SMS] = { 3, 3, 1, 2, 6 },
	[SNMP_EV_EMERGENCY] = { 4, 3, 1, 2, 3 },
	[SNMP_EV_ARS] = { 5, 1, 7 },
	[SNMP_EV_LOW_BATTERY] = { 6, 1, 7 },
};

int snmpEventEncode(const snmpEvent_t *ev, int32_t requestId, uint8_t *out, int cap)
{
	uint8_t oids[5][SNMP_OID_MAX];
	uint8_t trap[] = { 0x2B, 6, 1, 4, 1, PEN_BER, 2, 0, 0 };
	snmpVarbind_t vb[5];
	const uint8_t *row = EVENT_OBJECTS[ev->kind];
	const uint8_t *oid = trap;
	int oidLen = sizeof(trap);
	int base = PFX_ODR[0];

	if (row[0] == 0) {
		oid = (ev->kind == SNMP_EV_COLD_START) ? TRAP_COLD_START : TRAP_AUTH_FAIL;
		oidLen = sizeof(TRAP_COLD_START);
	}
	trap[oidLen - 1] = row[0];
	memset(vb, 0, sizeof(vb));
	for (int i = 0; i < row[1]; i++) {
		uint8_t k = row[2 + i];
		snmpValue_t *v = &vb[i].v;

		memcpy(oids[i], PFX_ODR + 1, (size_t)base);
		oids[i][base] = 8;	// odrEvent.k.0
		oids[i][base + 1] = k;
		oids[i][base + 2] = 0;
		vb[i].oid = oids[i];
		vb[i].oidLen = (uint8_t)(base + 3);
		v->type = BER_GAUGE32;
		switch (k) {
		case 1:
			v->num = ev->src;
			break;
		case 2:
			v->num = ev->dst;
			break;
		case 3:
			v->type = BER_INTEGER;
			v->num = TRUTH(ev->group);
			break;
		case 4:
			v->num = ev->slot;
			break;
		case 5:
			v->type = BER_TIMETICKS;
			v->num = ev->durationMs / 10u;
			break;
		case 6:
			v->type = BER_OCTETS;
			str(v, ev->text);
			break;
		default:
			v->type = BER_INTEGER;
			v->num = (uint32_t)ev->value;
			break;
		}
	}
	return snmpBuildTrap(out, cap, SNMP_TRAP_COMMUNITY, requestId, ev->atMs / 10u, oid, oidLen, vb, row[1]);
}
