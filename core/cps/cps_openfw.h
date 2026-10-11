// SPDX-License-Identifier: MIT Copyright (c) 2026 The OpenDigitalRadio Authors The open-firmware CPS protocol shim
// (docs/spec/open-firmware-cps.md, our behavior in docs/spec/cps-protocol.md 2): lets qdmr's open-firmware drivers and
// the open-firmware CPS read and write the codeplug over the host port. The radio presents the open-firmware image
// directly (it is our storage today); writes go through the sector journal (core/storage), and the radio's own areas
// (settings, journal, native codeplug reserve) can't be written. CONFIG_CPS_OPENFW.
//
// The engine (cps_openfw.c) has no RTOS: the host link task feeds it bytes, and it calls cps_io_t for its replies
// and for the reboot; cps_port.c is the firmware glue, the host tests drive it directly.
#ifndef ODR_CPS_OPENFW_H
#define ODR_CPS_OPENFW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CPS_REQUEST_MAX		2048	// the request buffer (open-firmware-cps.md 1.1)
#define CPS_REPLY_MAX		32768	// longest read reply payload: streamed, so not bound by a buffer (open-firmware-cps.md 2.1)
#define CPS_GAP_MS		3	// quiet that ends a request whose length the header doesn't give
#define CPS_SESSION_MS		10000	// a session ends this long after the last byte (pending sector dropped)

typedef struct {
	void (*write)(const uint8_t *buf, size_t len);	// bytes to the host
	// "Save and reboot" (save) or "reboot" after the reply went out; NULL = not possible
	void (*reboot)(bool save);
} cps_io_t;

typedef struct {
	uint32_t requests;	// requests handled
	uint32_t reads;		// R requests answered with data
	uint32_t commits;	// sectors written (X 3)
	uint32_t refused;	// requests answered '-' as an error (bad area, protected sector, ...)
	uint32_t overflows;	// requests longer than CPS_REQUEST_MAX
	uint8_t last;		// first byte of the last request
	bool session;		// a CPS session is going on
	bool pending;		// a sector is prepared (X 1) and not committed yet
} cps_stats_t;

void cps_init(const cps_io_t *io);
// Bytes from the host, or len 0 as a tick (the host link calls it every few ms in a CPS session: gap-terminated
// requests complete, the session times out)
void cps_feed(const uint8_t *buf, size_t len, uint32_t now_ms);
void cps_get_stats(cps_stats_t *s);

// Firmware: set up the engine on the host link (cps_port.c)
void cps_openfw_start(void);

#endif
