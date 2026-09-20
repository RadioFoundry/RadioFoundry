// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// APRS on the host port (aprs_host.h)
#include <string.h>
#include "config.h"
#include "aprs/aprs_host.h"
#include "aprs/aprs.h"
#include "aprs/aprs_nmea.h"
#include "aprs/kiss.h"
#if CONFIG_HOSTLINK
#include "hostlink/hostlink.h"
#endif

static kiss_dec_t kiss;
static char nmea[NMEA_LINE_MAX + 1];
static uint8_t nmea_len;
static bool nmea_on;		// collecting a '$' line

bool aprs_host_claims(uint8_t b)
{
	return aprs_host_enabled() && (b == KISS_FEND || b == '$');
}

bool aprs_host_in_frame(void)
{
	return nmea_on || !kiss_dec_idle(&kiss);
}

void aprs_host_kiss_frame(const uint8_t *f, size_t n, uint32_t now_ms)
{
	uint8_t type = f[0];
	if (type == KISS_CMD_RETURN || (type >> 4) != 0) {
		return;		// leave KISS mode (nothing to leave), or another port
	}
	switch (type & 0x0Fu) {
	case KISS_CMD_DATA:
		aprs_host_frame(f + 1, n - 1u, now_ms);
		break;
	case KISS_CMD_SETHW:
		if (n >= 2 && f[1] == KISS_TAG_IS_LINE) {
			aprs_host_is_line((const char *)f + 2, n - 2u, now_ms);
		} else if (n >= 2 && f[1] == KISS_TAG_NMEA) {
			aprs_nmea((const char *)f + 2, n - 2u, false, now_ms);
		}
		break;
	default:
		break;		// TXDELAY, persistence, slot time: the radio's own settings and CSMA apply (aprs.md 9)
	}
}

void aprs_host_feed(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t b = buf[i];
		if (!nmea_on && b == '$' && kiss.len == 0 && !kiss.esc) {
			nmea_on = true;
			nmea_len = 0;
		}
		if (nmea_on) {
			if (b == '\n' || b == '\r') {
				aprs_nmea(nmea, nmea_len, false, now_ms);
				nmea_on = false;
			} else if (nmea_len < NMEA_LINE_MAX) {
				nmea[nmea_len++] = (char)b;
			} else {
				nmea_on = false;	// too long: not NMEA
			}
			continue;
		}
		size_t n = kiss_dec_byte(&kiss, b);
		if (n != 0) {
			aprs_host_kiss_frame(kiss.buf, n, now_ms);
		}
	}
}

#if CONFIG_HOSTLINK
static const hostlink_tnc_t tnc = {
	.claims = aprs_host_claims,
	.feed = aprs_host_feed,
	.in_frame = aprs_host_in_frame,
	.output = aprs_host_output,
};

void aprs_host_start(void)
{
	kiss_dec_init(&kiss);
	hostlink_set_tnc(&tnc);
}
#else
void aprs_host_start(void)
{
	kiss_dec_init(&kiss);
}
#endif
