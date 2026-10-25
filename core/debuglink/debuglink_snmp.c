// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SNMP agent on the debug link (docs/spec/debuglink.md 3.17, docs/spec/snmp.md 7): the snmp view section (the
// flags and the transport counters; the protocol counters and the other settings are in the MIB itself) and the
// SNMP command (settings, communities, the host's notification port, a test notification). Both run in the host link
// task, as the agent does.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_SNMP
#include "snmp/snmp_agent.h"

void debuglink_view_snmp(dl_text_t *t)
{
	snmp_agent_stats_t s;
	snmpConfig_t c;

	snmp_agent_stats(&s);
	snmp_agent_get_config(&c);
	dl_text(t, "snmp.built=1\nsnmp.flags=0x%02x\nsnmp.write_set=%u\nsnmp.trap_port=%u\n", c.flags, c.writeHash != 0,
		snmp_agent_trap_port());
	dl_text(t, "snmp.rf_requests=%u\nsnmp.rf_answers=%u\nsnmp.rf_dropped=%u\nsnmp.traps_usb=%u\nsnmp.traps_rf=%u\n"
		"snmp.traps_dropped=%u\n", (unsigned)s.rf_requests, (unsigned)s.rf_answers, (unsigned)s.rf_dropped,
		(unsigned)s.traps_usb, (unsigned)s.traps_rf, (unsigned)s.traps_dropped);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t debuglink_cmd_snmp(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	uint8_t st = DL_ST_OK;
	snmpConfig_t c;

	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);

	snmp_agent_get_config(&c);
	switch (req[0]) {
	case DL_SNMP_OP_SET: {
		if (n != 5) {
			st = DL_ST_BAD_ARG;
			break;
		}
		uint32_t v = get32(a + 1);

		switch (a[0]) {
		case DL_SNMP_F_FLAGS:
			c.flags = (uint8_t)v;
			st = (v <= 0xFFu) ? DL_ST_OK : DL_ST_BAD_ARG;
			break;
		case DL_SNMP_F_RF_MANAGER:
			c.rfManager = v;
			st = (v <= 0xFFFFFFu) ? DL_ST_OK : DL_ST_BAD_ARG;
			break;
		case DL_SNMP_F_RF_GAP_S:
			c.rfTrapGapS = (uint16_t)v;
			st = ((v == 0) || ((v >= 10u) && (v <= 3600u))) ? DL_ST_OK : DL_ST_BAD_ARG;
			break;
		case DL_SNMP_F_TRAP_PORT:
			if (v > 0xFFFFu) {
				st = DL_ST_BAD_ARG;
				break;
			}
			snmp_agent_set_trap_port((uint16_t)v);
			out[0] = DL_ST_OK;
			return 1;
		default:
			st = DL_ST_BAD_ARG;
			break;
		}
		if (st == DL_ST_OK) {
			snmp_agent_set_config(&c);
		}
		break;
	}
	case DL_SNMP_OP_COMMUNITY:
		if ((n < 1) || (a[0] > 1) || ((n - 1u) > DL_SNMP_COMMUNITY_MAX)) {
			st = DL_ST_BAD_ARG;
			break;
		}
		snmp_agent_set_community(a[0] == 1, (const char *)(a + 1), n - 1);
		break;
	case DL_SNMP_OP_TEST_TRAP:
		snmp_agent_test_trap();
		break;
	default:
		st = DL_ST_BAD_ARG;
		break;
	}
	out[0] = st;
	return 1;
}

#else

void debuglink_view_snmp(dl_text_t *t)
{
	dl_text(t, "snmp.built=0\n");
}

size_t debuglink_cmd_snmp(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
