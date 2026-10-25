// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KISS over TCP on port 8001 (see net_svc.h): the KISS framing of Chepponis and Karn ("The KISS TNC", 1987) on a TCP
// stream, as APRS programs (APRSdroid, Xastir, YAAC ...) speak it to network TNCs. The frames are the same as on the
// serial port's KISS TNC (core/aprs/aprs_host.c): data frames to RF after the APRS engine's checks, SetHardware 'G'
// and 'N' lines; what the engine has for the host goes to the client (net.c's TNC pump). One client at a time: a new
// connection replaces the old one (a client that vanished without closing would otherwise hold the port).
#include "config.h"
#if CONFIG_NET_KISS	// the whole file
#include "net/net_svc.h"
#include "net/tcp.h"
#include "aprs/kiss.h"
#include "aprs/aprs.h"
#include "aprs/aprs_host.h"
#include "hal_time.h"

#define CHUNK	32	// encoded bytes per tcp_write

static int client = -1;
static kiss_dec_t in;
static struct {
	uint32_t conns, frames_in, frames_out, dropped;
} st;

static void k_opened(int c)
{
	if (client >= 0) {
		tcp_abort(client);
	}
	client = c;
	kiss_dec_init(&in);
	st.conns++;
}

static void k_recv(int c, const uint8_t *d, int n)
{
	if (c != client) {
		return;
	}
	for (int i = 0; i < n; i++) {
		size_t m = kiss_dec_byte(&in, d[i]);

		if (m != 0) {
			st.frames_in++;
			aprs_host_kiss_frame(in.buf, m, hal_millis());
		}
	}
}

static void k_peer_closed(int c)
{
	tcp_close(c);
	if (c == client) {
		client = -1;
	}
}

static void k_closed(int c, bool error)
{
	(void)error;
	if (c == client) {
		client = -1;
	}
}

static const tcp_app_t app = {
	.opened = k_opened, .recv = k_recv, .peer_closed = k_peer_closed, .closed = k_closed,
};

void kiss_tcp_start(void)
{
	client = -1;
	tcp_listen(NET_KISS_PORT, &app);
}

bool kiss_tcp_active(void)
{
	return (client >= 0) && aprs_host_enabled();
}

bool kiss_tcp_room(size_t n)
{
	return (client >= 0) && ((size_t)tcp_space(client) >= n);
}

void kiss_tcp_frame(const uint8_t *f, size_t n)
{
	uint8_t buf[CHUNK + 2];
	size_t k = 0;

	if (!kiss_tcp_room(2u * n + 2u)) {
		st.dropped++;
		return;
	}
	buf[k++] = KISS_FEND;
	for (size_t i = 0; i < n; i++) {
		if ((f[i] == KISS_FEND) || (f[i] == KISS_FESC)) {
			buf[k++] = KISS_FESC;
			buf[k++] = (f[i] == KISS_FEND) ? KISS_TFEND : KISS_TFESC;
		} else {
			buf[k++] = f[i];
		}
		if (k >= CHUNK) {
			tcp_write(client, buf, (int)k);
			k = 0;
		}
	}
	buf[k++] = KISS_FEND;
	tcp_write(client, buf, (int)k);
	st.frames_out++;
}

void kiss_tcp_view(dl_text_t *t)
{
	dl_text(t, "kiss.client=%u\nkiss.conns=%u\nkiss.frames_in=%u\nkiss.frames_out=%u\nkiss.dropped=%u\n",
		client >= 0, (unsigned)st.conns, (unsigned)st.frames_in, (unsigned)st.frames_out, (unsigned)st.dropped);
}

#endif	// CONFIG_NET_KISS
