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

bool aprs_dmr_send(bool udt, uint32_t dst, uint32_t own_id, const aprs_dmr_pos_t *pos)
{
	if (dst == 0 || dst > 0xFFFFFFu || dmrDataServiceIsBusy()) {
		return false;
	}
	lrrpPosition_t p;
	lrrpPositionFromE7(pos->lat, pos->lon, pos->speed_kn, pos->course, pos->utc_hms, pos->date, &p);
	if (udt) {
		dmrBurst_t b[UDT_BURSTS_MAX];
		int n = dmrDataBuildUDTLocation(false, dst, own_id, &p.location, DMR_DATA_SMS_PREAMBLES, b, UDT_BURSTS_MAX);
		return n > 0 && dmrDataServiceSendBursts(b, n);
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
