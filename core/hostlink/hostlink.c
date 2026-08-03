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
#endif
#include "FreeRTOS.h"
#include "task.h"

#define HOSTLINK_TASK_PRIORITY	3	// with input and USB; above the UI (core/app/app.c task table)
#define HOSTLINK_TASK_STACK	512	// words: MMDVM frames are built on the stack
#define POLL_MS			5	// hotspot housekeeping while nothing arrives
#define READ_CHUNK		64
#define CPS_GAP_MS		3	// a new CPS request starts after this much quiet (legacy-cps-interop.md 1.2)

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

static volatile hostlink_mode_t mode = HOSTLINK_IDLE;
static volatile bool hotspot_allowed = true;
static volatile bool exit_requested;
static hostlink_cps_fn cps_handler;
static uint32_t last_byte_ms;
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

void hostlink_set_cps_handler(hostlink_cps_fn fn)
{
	cps_handler = fn;
}

size_t hostlink_write(const uint8_t *buf, size_t len)
{
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

static void leave_hotspot(void)
{
	mmdvm_end();
	mmdvm_framer_reset(&framer);
	mode = HOSTLINK_IDLE;
}

static void feed_mmdvm(const uint8_t *buf, size_t len, uint32_t now)
{
	for (size_t i = 0; i < len; i++) {
		uint16_t n = mmdvm_framer_byte(&framer, buf[i]);

		if (n > 0) {
			mmdvm_frame(framer.buf, n, now);
		}
	}
}
#endif	// CONFIG_HOTSPOT

static void dispatch(const uint8_t *buf, size_t len, uint32_t now)
{
	bool new_request = (now - last_byte_ms) >= CPS_GAP_MS;

	last_byte_ms = now;
#if CONFIG_HOTSPOT
	if ((mode != HOSTLINK_HOTSPOT) && new_request && (buf[0] == MMDVM_FRAME_START) && hotspot_allowed) {
		mode = HOSTLINK_HOTSPOT;
		mmdvm_framer_reset(&framer);
		hostlink_session_start(&session, (board.host_port != NULL) && board.host_port->connected());
		entered_ms = now;
	}
	if (mode == HOSTLINK_HOTSPOT) {
		feed_mmdvm(buf, len, now);
		return;
	}
#else
	(void)new_request;
#endif
	if (cps_handler != NULL) {
		mode = HOSTLINK_CPS;
		cps_handler(buf, len, now);
	}
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
	mmdvm_framer_reset(&framer);
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

		switch (mode) {
		case HOSTLINK_HOTSPOT:
#if CONFIG_HOTSPOT
			mmdvm_poll(now);
			// Never gated on DTR being set, only on it dropping (hostlink.h). The port can't report USB unmount or
			// suspend apart from DTR yet (hal_stream_t has no such call), so with DTR never set an unplugged host
			// is noticed by the idle timeout
			uint32_t last = (mmdvm_last_frame_ms() != 0) ? mmdvm_last_frame_ms() : entered_ms;

			if (hostlink_session_check(&session, port->connected(), true, exit_now, now - last) !=
			    HOSTLINK_STAY) {
				leave_hotspot();
			}
#else
			mode = HOSTLINK_IDLE;
#endif
			break;
		case HOSTLINK_CPS:
			if ((now - last_byte_ms) > HOSTLINK_CPS_IDLE_MS) {
				mode = HOSTLINK_IDLE;
			}
			break;
		case HOSTLINK_IDLE:
			break;
		}
	}
}

void hostlink_start(void)
{
	xTaskCreate(hostlink_task, "host", HOSTLINK_TASK_STACK, NULL, HOSTLINK_TASK_PRIORITY, NULL);
}
