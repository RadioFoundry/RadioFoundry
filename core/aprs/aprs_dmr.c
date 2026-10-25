// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Digital APRS (aprs_dmr.h)
#include <string.h>
#include "config.h"
#include "aprs/aprs_dmr.h"
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrData.h"
#include "dmr/dmrLrrp.h"
#include "dmr/dmrDataService.h"

#define UDT_BURSTS_MAX	(DMR_DATA_SMS_PREAMBLES + 4)

// The LRRP request ID our unsolicited reports carry (no request came: any fixed ID; docs/spec/aprs.md 8.1)
static const uint8_t report_id[] = { 0x00, 0x00, 0x00, 0x01 };

static void location_of(const aprs_dmr_pos_t *p, dmrDataLocation_t *l)
{
	memset(l, 0, sizeof(*l));
	uint32_t la = (uint32_t)(p->lat < 0 ? -(int64_t)p->lat : p->lat);
	uint32_t lo = (uint32_t)(p->lon < 0 ? -(int64_t)p->lon : p->lon);
	l->fixValid = true;
	l->north = p->lat >= 0;
	l->east = p->lon >= 0;
	l->latDegrees = (uint8_t)(la / 10000000u);
	l->lonDegrees = (uint8_t)(lo / 10000000u);
	// 1e-7 degree -> 1e-4 minute: x 60 / 1000
	l->latMinutes10000 = (uint32_t)(((uint64_t)(la % 10000000u) * 60u) / 1000u);
	l->lonMinutes10000 = (uint32_t)(((uint64_t)(lo % 10000000u) * 60u) / 1000u);
	l->speedKnots = p->speed_kn < DMR_LOCATION_SPEED_UNKNOWN ? p->speed_kn : DMR_LOCATION_SPEED_UNKNOWN;
	if (p->utc_hms != 0xFFFFFFFFu) {
		l->utcHours = (uint8_t)(p->utc_hms / 10000u);
		l->utcMinutes = (uint8_t)(p->utc_hms / 100u % 100u);
		l->utcSeconds = (uint8_t)(p->utc_hms % 100u);
	}
}

bool aprs_dmr_send(bool udt, uint32_t dst, uint32_t own_id, const aprs_dmr_pos_t *pos)
{
	if (dst == 0 || dst > 0xFFFFFFu || dmrDataServiceIsBusy()) {
		return false;
	}
	if (udt) {
		dmrDataLocation_t l;
		dmrBurst_t b[UDT_BURSTS_MAX];
		location_of(pos, &l);
		int n = dmrDataBuildUDTLocation(false, dst, own_id, &l, DMR_DATA_SMS_PREAMBLES, b, UDT_BURSTS_MAX);
		return n > 0 && dmrDataServiceSendBursts(b, n);
	}
	lrrpPosition_t p;
	memset(&p, 0, sizeof(p));
	location_of(pos, &p.location);
	if (pos->date != 0 && pos->utc_hms != 0xFFFFFFFFu) {
		p.utcDay = (uint8_t)(pos->date / 10000u);
		p.utcMonth = (uint8_t)(pos->date / 100u % 100u);
		p.utcYear = (uint16_t)(2000u + pos->date % 100u);
	}
	if (pos->course != 0xFFFFu) {
		p.hasCourse = true;
		p.courseDegrees = pos->course;
	}
	uint8_t doc[LRRP_MAX_DOCUMENT + 16];
	uint8_t wanted = LRRP_WANT_SPEED | LRRP_WANT_DIRECTION | (p.utcYear != 0 ? LRRP_WANT_TIME : 0);
	int n = lrrpBuildTriggeredReport(report_id, (int)sizeof(report_id), wanted, &p, doc);
	return n > 0 && dmrDataServiceSendUDP(false, dst, DMR_UDP_PORT_LRRP, DMR_UDP_PORT_LRRP, doc, n);
}

#else

bool aprs_dmr_send(bool udt, uint32_t dst, uint32_t own_id, const aprs_dmr_pos_t *pos)
{
	(void)udt;
	(void)dst;
	(void)own_id;
	(void)pos;
	return false;
}

#endif
