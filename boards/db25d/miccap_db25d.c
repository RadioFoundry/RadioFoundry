// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D hand mic capture for the bring-up. The hand mic has its own MCU: while a key is held it sends a short frame
// every 2.5 ms at 115200 baud on PTA9, the PC link's RXD (hardware-db25d.md 3.4), so the host UART receives it. This
// logs those frames from the UART's receive interrupt: a gap of more than 0.5 ms between bytes ends a frame, and a
// frame identical to the one before it less than 50 ms earlier (the same key still held) isn't logged again. The
// UART's receive tap (mic_db25d.c) hands it every byte.
// Resetting the diagnostics clears the log. The stub reads db25d_miccap back (stubtool --peek) once the PC cable is in
// again; the diagnostics page shows the last frame.
#include <string.h>
#include "db25d.h"
#include "config.h"
#include "platform.h"

#if CONFIG_DIAG

#define FRAME_MAX	8u
#define LOG_FRAMES	128u
#define FRAME_GAP_US	500u
#define REPEAT_US	50000u
#define CAP_MAGIC	0x4D434150u	// "MCAP"

typedef struct {
	uint32_t us;			// hrc_micros() at the first byte
	uint8_t len;
	uint8_t errors;			// characters the UART flagged
	uint8_t b[FRAME_MAX];
	uint8_t pad[2];
} mic_frame_t;

// Not static: found by its symbol in the build's ELF for the read-back
struct {
	uint32_t magic;
	volatile uint32_t n;		// frames logged
	uint32_t dropped;		// frames that didn't fit
	mic_frame_t log[LOG_FRAMES];
} db25d_miccap;

static mic_frame_t cur, last;
static uint32_t last_byte_us, last_end_us;
static bool have_last;

static void finish(void)
{
	if (cur.len == 0) {
		return;
	}
	bool repeat = have_last && cur.len == last.len && memcmp(cur.b, last.b, cur.len) == 0 &&
		cur.us - last_end_us < REPEAT_US;
	if (!repeat) {
		if (db25d_miccap.n < LOG_FRAMES) {
			db25d_miccap.log[db25d_miccap.n] = cur;
			db25d_miccap.n = db25d_miccap.n + 1u;
		} else {
			db25d_miccap.dropped++;
		}
	}
	last = cur;
	have_last = true;
	last_end_us = last_byte_us;
	cur.len = 0;
	cur.errors = 0;
}

void db25d_miccap_byte(uint8_t b, bool error, uint32_t now)
{
	if (cur.len != 0 && now - last_byte_us > FRAME_GAP_US) {
		finish();
	}
	if (cur.len == 0) {
		cur.us = now;
	}
	if (cur.len < FRAME_MAX) {
		cur.b[cur.len++] = b;
	}
	if (error && cur.errors < UINT8_MAX) {
		cur.errors++;
	}
	last_byte_us = now;
}

void db25d_miccap_init(void)
{
	db25d_miccap.magic = CAP_MAGIC;
	db25d_miccap_arm();
}

void db25d_miccap_arm(void)
{
	uint32_t s = hrc_irq_save();
	db25d_miccap.n = 0;
	db25d_miccap.dropped = 0;
	cur.len = 0;
	cur.errors = 0;
	have_last = false;
	hrc_irq_restore(s);
}

// The newest frame: the one being received, else the last finished one
uint8_t db25d_miccap_last(uint8_t *b, uint8_t max)
{
	uint32_t s = hrc_irq_save();
	const mic_frame_t *f = cur.len != 0 ? &cur : have_last ? &last : NULL;
	uint8_t n = 0;
	if (f != NULL) {
		n = f->len < max ? f->len : max;
		memcpy(b, f->b, n);
	}
	hrc_irq_restore(s);
	return n;
}

uint16_t db25d_miccap_count(void)
{
	return (uint16_t)db25d_miccap.n;
}

#endif
