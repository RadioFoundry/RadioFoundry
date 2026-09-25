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
//   tnc       a request starting with a byte the TNC protocol claims (KISS 0xC0, NMEA '$'; core/aprs/aprs_host.c):
//             its bytes go to it; back to idle after HOSTLINK_TNC_IDLE_MS without one. Its output (received
//             packets, iGate lines) also goes out in idle mode when it has some (the user turned KISS on)
//   cps       anything else goes to the CPS handler, when one is registered (docs/spec/legacy-cps-interop.md); back
//             to idle after HOSTLINK_CPS_IDLE_MS without a CPS byte (side protocol frames don't count)
//
// A side protocol (the debug link, docs/spec/debuglink.md) may be registered beside these: its frames start with a
// byte no other protocol starts a request with (0xA5; MMDVM is 0xE0, CPS requests are ASCII letters). It never
// changes the mode. Its first byte is recognized at a request boundary: in idle mode always, in a CPS session only
// after the CPS_GAP_MS quiet that starts a new request or right after another side frame, in hotspot mode only between MMDVM frames (where the MMDVM
// framer would throw the byte away as junk). Once a side frame has started, its bytes go to it until it ends or
// times out.
#ifndef ODR_HOSTLINK_H
#define ODR_HOSTLINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HOSTLINK_HOTSPOT_IDLE_MS	20000	// MMDVMHost polls GET_STATUS every 250 ms; 20 s of nothing = gone
#define HOSTLINK_CPS_IDLE_MS		10000
#define HOSTLINK_TNC_IDLE_MS		30000

typedef enum { HOSTLINK_IDLE, HOSTLINK_HOTSPOT, HOSTLINK_CPS, HOSTLINK_TNC } hostlink_mode_t;

// Start the task (after the settings are loaded: the USB personality depends on them). With settings_t.usb_mode 1
// (CONFIG_USB_NETWORK) the radio comes up as the network adapter; a host that doesn't configure it within 10 s of
// enumerating it gets the serial port back for this boot (docs/spec/usb-network.md 1)
void hostlink_start(void);
// The USB network settings changed in the settings store (usb_mode, net_flags, net_link_ip): apply what applies at
// run time (control station passthrough, the link addressing). usb_mode takes effect at the next power-on
void hostlink_settings_changed(void);

hostlink_mode_t hostlink_mode(void);

// The hotspot may be entered from the host (a setting; default allowed). Turning it off leaves hotspot mode
void hostlink_set_hotspot_allowed(bool allowed);
// Leave hotspot mode now (the UI's exit); the host must send a request again to come back
void hostlink_hotspot_exit(void);
// Switch the USB personality to the network adapter (true) or the serial port, after the current request's reply has
// gone out (the host link task does it between passes). False if the board has no network personality
bool hostlink_request_usb_network(bool network);
// The network personality is the one in use (UDP serial, the IP gateway)
bool hostlink_network_mode(void);

// The CPS protocol, when it exists: called from the host link task with each chunk of bytes in a CPS session
// (requests are not framed here; the handler splits them, e.g. by the 3 ms gap), and with len 0 (buf NULL) as a tick
// on every pass of the task while the session lasts. Replies go out with hostlink_write
typedef void (*hostlink_cps_fn)(const uint8_t *buf, size_t len, uint32_t now_ms);
void hostlink_set_cps_handler(hostlink_cps_fn fn);
// The side protocol: start is the first byte of its frames; feed takes bytes that start (buf[0] == start) or continue
// a frame and returns how many it took (0: not its own after all, the bytes go to the other protocols); in_frame
// says whether a frame is in progress (its next bytes belong to it). Called from the host link task; replies go out
// with hostlink_write. Must not block for long: the hotspot's GET_STATUS answers wait meanwhile
typedef struct {
	uint8_t start;
	size_t (*feed)(const uint8_t *buf, size_t len, uint32_t now_ms);
	bool (*in_frame)(uint32_t now_ms);
} hostlink_side_t;
void hostlink_set_side_protocol(const hostlink_side_t *side);
// The TNC protocol (KISS for APRS): claims says whether a request starting with byte b is its own (checked in idle
// mode at a request boundary); feed takes its bytes; in_frame says a frame is in progress (side protocol frames wait);
// output copies bytes it has for the host (idle and TNC modes, every pass), 0 when none. Host link task only
typedef struct {
	bool (*claims)(uint8_t b);
	void (*feed)(const uint8_t *buf, size_t len, uint32_t now_ms);
	bool (*in_frame)(void);
	size_t (*output)(uint8_t *buf, size_t max);
} hostlink_tnc_t;
void hostlink_set_tnc(const hostlink_tnc_t *tnc);
// Bytes to the host; only from the host link task (the CPS handler, the hotspot, the side protocol)
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

// Why hotspot mode was last left (HOSTLINK_STAY = not left yet since boot), for the debug link
hostlink_leave_t hostlink_last_leave(void);

// On entering hotspot mode, with the port's connected() at that moment
void hostlink_session_start(hostlink_session_t *s, bool connected);
// connected: the port's connected() now (DTR); present: the port exists (USB configured and not suspended; true when
// the port can't tell); idle_ms: since the last MMDVM frame
hostlink_leave_t hostlink_session_check(hostlink_session_t *s, bool connected, bool present, bool exit_requested,
				       uint32_t idle_ms);

#endif
