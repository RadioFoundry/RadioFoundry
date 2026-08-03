// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The host link: one task that owns the board's host port (board.host_port: USB CDC-ACM on the STM32 radios, the
// programming cable's UART on others) and hands its byte stream to one protocol at a time:
//
//   idle      nothing going on; the next request picks the protocol
//   hotspot   a request starting 0xE0 (an MMDVM frame) when the hotspot is allowed: the radio becomes an MMDVM
//             modem (core/hotspot) and takes the DMR modem from the call stack (through the radio core's
//             hotspot_radio_ops_t). It leaves after HOSTLINK_HOTSPOT_IDLE_MS without a frame, when the port closes
//             (the port's connected() going from true to false: a POSIX host closing it drops DTR, or USB unmounted
//             while open), or when the UI asks (hostlink_hotspot_exit). Never because DTR is clear: MMDVMHost on
//             Windows opens the port with DTR cleared (docs/spec/mmdvm-protocol.md 5.1)
//   cps       anything else goes to the CPS handler, when one is registered (docs/spec/legacy-cps-interop.md); back
//             to idle after HOSTLINK_CPS_IDLE_MS without a byte
#ifndef ODR_HOSTLINK_H
#define ODR_HOSTLINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HOSTLINK_HOTSPOT_IDLE_MS	20000	// MMDVMHost polls GET_STATUS every 250 ms; 20 s of nothing = gone
#define HOSTLINK_CPS_IDLE_MS		10000

typedef enum { HOSTLINK_IDLE, HOSTLINK_HOTSPOT, HOSTLINK_CPS } hostlink_mode_t;

// Start the task (after the settings are loaded: the USB personality depends on them)
void hostlink_start(void);

hostlink_mode_t hostlink_mode(void);

// The hotspot may be entered from the host (a setting; default allowed). Turning it off leaves hotspot mode
void hostlink_set_hotspot_allowed(bool allowed);
// Leave hotspot mode now (the UI's exit); the host must send a request again to come back
void hostlink_hotspot_exit(void);

// The CPS protocol, when it exists: called from the host link task with each chunk of bytes in a CPS session
// (requests are not framed here; the handler splits them, e.g. by the 3 ms gap). Replies go out with hostlink_write
typedef void (*hostlink_cps_fn)(const uint8_t *buf, size_t len, uint32_t now_ms);
void hostlink_set_cps_handler(hostlink_cps_fn fn);
// Bytes to the host; only from the host link task (the CPS handler, the hotspot)
size_t hostlink_write(const uint8_t *buf, size_t len);

// When to leave hotspot mode (hostlink_policy.c, no RTOS: the host tests run it). Fed after every pass of the task
typedef enum {
	HOSTLINK_STAY,
	HOSTLINK_LEAVE_USER,		// hostlink_hotspot_exit, or the hotspot was disallowed
	HOSTLINK_LEAVE_CLOSED,		// connected() went from true to false (port closed, DTR dropped, USB unmounted)
	HOSTLINK_LEAVE_GONE,		// the port isn't there any more (unplugged, suspended), whatever DTR said
	HOSTLINK_LEAVE_IDLE,		// no frame for HOSTLINK_HOTSPOT_IDLE_MS
} hostlink_leave_t;

typedef struct {
	bool was_connected;
} hostlink_session_t;

// On entering hotspot mode, with the port's connected() at that moment
void hostlink_session_start(hostlink_session_t *s, bool connected);
// connected: the port's connected() now (DTR); present: the port exists (USB configured and not suspended; true when
// the port can't tell); idle_ms: since the last MMDVM frame
hostlink_leave_t hostlink_session_check(hostlink_session_t *s, bool connected, bool present, bool exit_requested,
				       uint32_t idle_ms);

#endif
