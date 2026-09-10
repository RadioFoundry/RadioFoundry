// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The host link task (see hostlink.h)
#include "config.h"
#include "hostlink/hostlink.h"
#include "board.h"
#include "hal_time.h"
#if CONFIG_HOTSPOT
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"
#if CONFIG_SFR
#include "hotspot/hotspotSfr.h"
#endif
#endif
#if CONFIG_DEBUGLINK
#include "debuglink/debuglink.h"
#endif
#if CONFIG_DMR_IP_GATEWAY
#include "hostlink/hostlink_net.h"
#endif
#if CONFIG_APRS
#include "aprs/aprs_host.h"
#endif
#include "debuglink/dlog.h"
#include "FreeRTOS.h"
#include "task.h"

#define HOSTLINK_TASK_PRIORITY	3	// with input and USB; above the UI (core/app/app.c task table)
#if CONFIG_DEBUGLINK
#define HOSTLINK_TASK_STACK	640	// words: MMDVM frames on the stack; debug link replies format diagnostics lines
#else
#define HOSTLINK_TASK_STACK	512	// words: MMDVM frames are built on the stack
#endif
#define POLL_MS			5	// hotspot housekeeping while nothing arrives
#define READ_CHUNK		64
#define CPS_GAP_MS		3	// a new CPS request starts after this much quiet (legacy-cps-interop.md 1.2)

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

static volatile hostlink_mode_t mode = HOSTLINK_IDLE;
static volatile hostlink_leave_t last_leave = HOSTLINK_STAY;
static volatile bool hotspot_allowed = true;
static volatile bool exit_requested;
static volatile int8_t usb_switch = -1;		// a personality switch asked for: 1 network, 0 serial, -1 none
static hostlink_cps_fn cps_handler;
static const hostlink_side_t *side;
static const hostlink_tnc_t *tnc;
static uint32_t last_byte_ms;
static uint32_t last_main_ms;		// the last byte for the mode's own protocol (side frames don't count)
static bool side_ended;			// a side frame just ended: the next byte is at a request boundary
#if CONFIG_HOTSPOT
static mmdvm_framer_t framer;
static char description[64];
static hostlink_session_t session;
static uint32_t entered_ms;		// when hotspot mode began (idle time before the first whole frame)
#endif

hostlink_mode_t hostlink_mode(void)
{
	return mode;
}

hostlink_leave_t hostlink_last_leave(void)
{
	return last_leave;
}

void hostlink_set_hotspot_allowed(bool allowed)
{
	hotspot_allowed = allowed;
	if (!allowed) {
		exit_requested = true;
	}
}

void hostlink_hotspot_exit(void)
{
	exit_requested = true;
}

bool hostlink_request_usb_network(bool network)
{
	if ((board.netif == NULL) || (board.netif->select == NULL)) {
		return false;
	}
	usb_switch = network ? 1 : 0;
	return true;
}

bool hostlink_network_mode(void)
{
	return (board.netif != NULL) && board.netif->selected();
}

void hostlink_set_cps_handler(hostlink_cps_fn fn)
{
	cps_handler = fn;
}

void hostlink_set_side_protocol(const hostlink_side_t *p)
{
	side = p;
}

void hostlink_set_tnc(const hostlink_tnc_t *t)
{
	tnc = t;
}

size_t hostlink_write(const uint8_t *buf, size_t len)
{
#if CONFIG_DMR_IP_GATEWAY
	if (hostlink_network_mode()) {
		return hostlink_net_write(buf, len);	// UDP serial: to whoever last sent to the radio's serial port
	}
#endif
	if (board.host_port == NULL) {
		return 0;
	}
	return board.host_port->write(buf, len);
}

#if CONFIG_HOTSPOT
static void mmdvm_write(const uint8_t *buf, size_t len)
{
	hostlink_write(buf, len);
}

static const mmdvm_io_t mmdvm_io = {
	.write = mmdvm_write,
	.description = description,
};

static void make_description(void)
{
	static const char prefix[] = "OpenDigitalRadio ";
	const char *parts[3] = { prefix, board.name, " " ODR_GIT };
	size_t n = 0;

	for (int p = 0; p < 3; p++) {
		for (const char *s = parts[p]; (*s != '\0') && (n < sizeof(description) - 1); s++) {
			description[n++] = *s;
		}
	}
	description[n] = '\0';
}

static void leave_hotspot(hostlink_leave_t why)
{
	static const char *const names[] = { "", "user", "port closed (DTR dropped)", "port gone", "idle" };

	last_leave = why;
	dlog("hostlink: leaving hotspot mode: %s", ((unsigned)why < 5) ? names[why] : "?");
	(void)names;	// dlog() may be compiled out
	mmdvm_end();
	mmdvm_framer_reset(&framer);
	mode = HOSTLINK_IDLE;
}

static bool framer_idle(void)
{
	return (framer.len == 0) && (framer.skip == 0);
}

// MMDVM bytes up to the end of buf, or up to a side protocol frame starting between MMDVM frames. Returns the bytes
// taken
static size_t feed_mmdvm(const uint8_t *buf, size_t len, uint32_t now)
{
	for (size_t i = 0; i < len; i++) {
		if ((i > 0) && (side != NULL) && (buf[i] == side->start) && framer_idle()) {
			return i;
		}
		uint16_t n = mmdvm_framer_byte(&framer, buf[i]);

		if (n > 0) {
			mmdvm_frame(framer.buf, n, now);
		}
	}
	return len;
}
#endif	// CONFIG_HOTSPOT

// Bytes of the side protocol at the start of buf, if they are its own (see hostlink.h for where a frame may start)
static size_t side_take(const uint8_t *buf, size_t len, uint32_t now, bool new_request)
{
	if (side == NULL) {
		return 0;
	}
	if (!side->in_frame(now)) {
		if (buf[0] != side->start) {
			return 0;
		}
		switch (mode) {
		case HOSTLINK_HOTSPOT:
#if CONFIG_HOTSPOT
			if (!framer_idle()) {
				return 0;
			}
#endif
			break;
		case HOSTLINK_CPS:
			if (!new_request) {
				return 0;
			}
			break;
		case HOSTLINK_TNC:
			if (tnc->in_frame()) {
				return 0;
			}
			break;
		case HOSTLINK_IDLE:
			break;
		}
	}
	return side->feed(buf, len, now);
}

// The other protocols: returns the bytes taken (all of them, except in hotspot mode before a side frame)
static size_t dispatch_main(const uint8_t *buf, size_t len, uint32_t now, bool new_request)
{
#if CONFIG_HOTSPOT
	if ((mode != HOSTLINK_HOTSPOT) && new_request && (buf[0] == MMDVM_FRAME_START) && hotspot_allowed) {
		mode = HOSTLINK_HOTSPOT;
		mmdvm_framer_reset(&framer);
		hostlink_session_start(&session, (board.host_port != NULL) && board.host_port->connected());
		entered_ms = now;
		dlog("hostlink: entering hotspot mode");
	}
	if (mode == HOSTLINK_HOTSPOT) {
		return feed_mmdvm(buf, len, now);
	}
#endif
	if ((tnc != NULL) && (mode == HOSTLINK_IDLE) && new_request && tnc->claims(buf[0])) {
		mode = HOSTLINK_TNC;
		dlog("hostlink: TNC (KISS)");
	}
	if (mode == HOSTLINK_TNC) {
		tnc->feed(buf, len, now);
		return len;
	}
	if (cps_handler != NULL) {
		mode = HOSTLINK_CPS;
		cps_handler(buf, len, now);
	}
	return len;
}

static void dispatch(const uint8_t *buf, size_t len, uint32_t now)
{
	// A side frame's end is a request boundary too, also when the next request comes in a later chunk without the
	// gap (a debug link client sends its next request as soon as the reply is in)
	bool new_request = side_ended || ((now - last_byte_ms) >= CPS_GAP_MS);

	last_byte_ms = now;
	while (len > 0) {
		size_t n = side_take(buf, len, now, new_request);

		if (n > 0) {
			side_ended = !side->in_frame(now);
			new_request = side_ended;	// what follows a side frame starts a new request
		} else {
			side_ended = false;
			last_main_ms = now;
			n = dispatch_main(buf, len, now, new_request);
			new_request = false;
			if (n == 0) {
				break;
			}
		}
		buf += n;
		len -= n;
	}
}

#if CONFIG_DMR_IP_GATEWAY
// UDP serial bytes from the IP gateway (network mode): the same protocols as on the serial port
static void net_serial_in(const uint8_t *buf, size_t len)
{
	dispatch(buf, len, hal_millis());
}
#endif

// A personality switch asked for (the debug link's reply is out by now): hotspot mode ends first
static void usb_switch_now(void)
{
	int8_t s = usb_switch;

	if (s < 0) {
		return;
	}
	usb_switch = -1;
#if CONFIG_HOTSPOT
	if (mode == HOSTLINK_HOTSPOT) {
		leave_hotspot(HOSTLINK_LEAVE_USER);
	}
#endif
	mode = HOSTLINK_IDLE;
	dlog("hostlink: USB %s", s ? "network" : "serial");
	hal_delay_ms(20);	// the reply's last USB packet
	board.netif->select(s != 0);
}

static void hostlink_task(void *arg)
{
	(void)arg;
	const hal_stream_t *port = board.host_port;
	uint8_t buf[READ_CHUNK];

#if CONFIG_HOTSPOT
	make_description();
	hotspot_init(board.dmr, mmdvm_send);
	mmdvm_init(&mmdvm_io);
#if CONFIG_SFR
	hotspot_sfr_init();
#endif
	mmdvm_framer_reset(&framer);
#endif
#if CONFIG_DMR_IP_GATEWAY
	hostlink_net_init(net_serial_in);
#endif
	if ((port == NULL) || !port->start()) {
		vTaskDelete(NULL);
		return;
	}

	for (;;) {
		bool more = port->wait_rx(POLL_MS);
		uint32_t now = hal_millis();
		bool exit_now = exit_requested;

		exit_requested = false;
		(void)exit_now;

		while (more) {
			size_t n = port->read(buf, sizeof(buf));

			if (n == 0) {
				break;
			}
			dispatch(buf, n, now);
			now = hal_millis();
		}
#if CONFIG_DMR_IP_GATEWAY
		if (hostlink_network_mode()) {
			hostlink_net_poll(now);		// frames from the host (UDP serial among them), packets from the air
			now = hal_millis();
		}
#endif
		usb_switch_now();

		switch (mode) {
		case HOSTLINK_HOTSPOT:
#if CONFIG_HOTSPOT
			mmdvm_poll(now);
			// Never gated on DTR being set, only on it dropping (hostlink.h). The port can't report USB unmount or
			// suspend apart from DTR yet (hal_stream_t has no such call), so with DTR never set an unplugged host
			// is noticed by the idle timeout
			uint32_t last = (mmdvm_last_frame_ms() != 0) ? mmdvm_last_frame_ms() : entered_ms;

			hostlink_leave_t why = hostlink_session_check(&session, port->connected(), true, exit_now,
								      now - last);

			if (why != HOSTLINK_STAY) {
				leave_hotspot(why);
			}
#else
			mode = HOSTLINK_IDLE;
#endif
			break;
		case HOSTLINK_CPS:
			if (cps_handler != NULL) {
				cps_handler(NULL, 0, now);	// tick: gap-terminated requests, the session time-out
			}
			if ((now - last_main_ms) > HOSTLINK_CPS_IDLE_MS) {	// debug link traffic doesn't hold it
				mode = HOSTLINK_IDLE;
			}
			break;
		case HOSTLINK_TNC:
			if ((now - last_main_ms) > HOSTLINK_TNC_IDLE_MS) {
				mode = HOSTLINK_IDLE;
			}
			break;
		case HOSTLINK_IDLE:
			break;
		}
		if ((tnc != NULL) && ((mode == HOSTLINK_IDLE) || (mode == HOSTLINK_TNC))) {
			size_t n;

			while ((n = tnc->output(buf, sizeof(buf))) > 0) {	// received packets, iGate lines
				hostlink_write(buf, n);
			}
		}
	}
}

void hostlink_start(void)
{
#if CONFIG_DEBUGLINK
	debuglink_start();	// registers itself as the side protocol
#endif
#if CONFIG_APRS
	aprs_host_start();	// registers the KISS TNC
#endif
	xTaskCreate(hostlink_task, "host", HOSTLINK_TASK_STACK, NULL, HOSTLINK_TASK_PRIORITY, NULL);
}
