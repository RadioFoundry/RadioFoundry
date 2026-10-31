// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A Bluetooth module that speaks AT lines on a UART: PTT and keys from its PTT button and its hand microphone, taken a
// byte at a time from a UART interrupt. Pure and allocation free, so the host tests run it. The board's hardware page
// has the module, its wiring and the stock firmware's reading of these lines (static analysis, untested on a radio).
//
// The radio asks with BT_AT_PROBE ("AT+VER?"); the module answers lines that start with '+', 'O' (OK) or 'E' (ERR)
// and end with LF (a CR before it is dropped). A line longer than BT_AT_LINE_MAX is dropped whole. Any line counts as
// the module answering. A +VER line naming the APRS add-on board (which sits on the same UART) marks it: that board
// isn't a Bluetooth module, and its binary frames after +APRS: aren't lines, so the parser drops them (errors).
//
// PTT has three sources, any one held keys the radio:
//   +PTTKEY=1 / 0          the PTT button (key fob): pressed, released
//   +HFREQ:1 / 0           the hands-free button: 1 toggles a latch, 0 clears it
//   +ADATA:nn,len          a data tunnel: the next len bytes are lines of the hand microphone's own: +PTT=P (press),
//                          +PTT=R (release), +KEY=NN (a key event, two decimal digits; 01-04 only)
// A link dropped (+DISC=, or +CONN= with the all-zero address) or the module restarting (+READY) clears every PTT
// source, so a link lost mid-transmission doesn't hold the transmitter on.
#ifndef BT_AT_H
#define BT_AT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BT_AT_PROBE		"AT+VER?\r\n"
#define BT_AT_LINE_MAX		64u	// the longest line kept (a +CONN line with a device name)
#define BT_AT_KEY_MIN		1u	// +KEY codes the stock firmware acts on
#define BT_AT_KEY_MAX		4u

typedef struct {
	char line[BT_AT_LINE_MAX];
	uint16_t n;			// bytes of the line so far (counts past BT_AT_LINE_MAX: the line is dropped)
	uint16_t payload;		// bytes of an +ADATA payload still to come
	bool present;			// the module has answered
	bool aprs_board;		// what answered is the APRS add-on board, not a Bluetooth module
	bool fob_ptt, hf_ptt, mic_ptt;	// the three PTT sources
	uint8_t key;			// the last +KEY code (BT_AT_KEY_MIN..MAX), 0 for none yet
	uint32_t key_events;		// +KEY lines taken: a new one is a new key press
	uint32_t lines, errors;		// lines taken; bytes or lines dropped (diagnostics)
} bt_at_t;

static inline void bt_at_init(bt_at_t *b)
{
	memset(b, 0, sizeof(*b));
}

static inline bool bt_at_ptt(const bt_at_t *b)
{
	return b->fob_ptt || b->hf_ptt || b->mic_ptt;
}

static inline void bt_at_release(bt_at_t *b)
{
	b->fob_ptt = false;
	b->hf_ptt = false;
	b->mic_ptt = false;
}

// A UART error (overrun, framing, noise): the line under way is lost
static inline void bt_at_error(bt_at_t *b)
{
	b->n = 0;
	b->payload = 0;
	b->errors++;
}

static inline bool bt_at_starts(const char *s, const char *p)
{
	return strncmp(s, p, strlen(p)) == 0;
}

// A decimal number at s, -1 when s doesn't start with a digit
static inline int32_t bt_at_number(const char *s)
{
	if (*s < '0' || *s > '9') {
		return -1;
	}
	int32_t v = 0;
	while (*s >= '0' && *s <= '9' && v < 100000) {
		v = v * 10 + (*s++ - '0');
	}
	return v;
}

// A Bluetooth address of 12 hex digits all '0': a link that ended
static inline bool bt_at_no_address(const char *s)
{
	for (unsigned i = 0; i < 12u; i++) {
		if (s[i] != '0') {
			return false;
		}
	}
	return true;
}

// One line of the hand microphone's, from an +ADATA payload
static inline void bt_at_mic_line(bt_at_t *b, const char *s)
{
	if (strcmp(s, "+PTT=P") == 0) {
		b->mic_ptt = true;
	} else if (strcmp(s, "+PTT=R") == 0) {
		b->mic_ptt = false;
	} else if (bt_at_starts(s, "+KEY=") && strlen(s) == 7u) {
		int32_t k = bt_at_number(s + 5);
		if (k >= (int32_t)BT_AT_KEY_MIN && k <= (int32_t)BT_AT_KEY_MAX) {
			b->key = (uint8_t)k;
			b->key_events++;
		}
	}
}

// A whole line from the module
static inline void bt_at_line(bt_at_t *b, const char *s)
{
	b->lines++;
	b->present = true;
	if (bt_at_starts(s, "+VER")) {
		b->aprs_board = bt_at_starts(s + 4, ":APRS") || bt_at_starts(s + 4, "=APRS");
	} else if (strcmp(s, "+PTTKEY=1") == 0) {
		b->fob_ptt = true;
	} else if (strcmp(s, "+PTTKEY=0") == 0) {
		b->fob_ptt = false;
	} else if (strcmp(s, "+HFREQ:1") == 0) {
		b->hf_ptt = !b->hf_ptt;
	} else if (strcmp(s, "+HFREQ:0") == 0) {
		b->hf_ptt = false;
	} else if (bt_at_starts(s, "+ADATA:")) {
		const char *c = strchr(s + 7, ',');
		int32_t len = c != NULL ? bt_at_number(c + 1) : -1;
		if (len > 0 && len < 1000) {
			b->payload = (uint16_t)len;
		}
	} else if (bt_at_starts(s, "+DISC=") || strcmp(s, "+READY") == 0 ||
		   (bt_at_starts(s, "+CONN=") && strlen(s) >= 18u && bt_at_no_address(s + 6))) {
		bt_at_release(b);
	} else if (bt_at_starts(s, "+APRS:")) {
		b->aprs_board = true;
	}
}

// One received byte
static inline void bt_at_byte(bt_at_t *b, uint8_t c)
{
	if (b->n == 0 && c != '+' && c != 'O' && c != 'E') {
		if (b->payload > 0) {
			b->payload--;	// a payload's line ends (CR LF) don't start a line
		} else if (c != '\r' && c != '\n') {
			b->errors++;
		}
		return;
	}
	if (c == '\n') {
		bool payload = b->payload > 0;
		if (payload) {
			b->payload = b->payload > b->n + 1u ? (uint16_t)(b->payload - b->n - 1u) : 0u;
		}
		if (b->n > BT_AT_LINE_MAX) {
			b->errors++;
		} else {
			uint16_t n = b->n;
			if (n > 0 && b->line[n - 1u] == '\r') {
				n--;
			}
			b->line[n < BT_AT_LINE_MAX ? n : BT_AT_LINE_MAX - 1u] = '\0';
			if (payload) {
				bt_at_mic_line(b, b->line);
			} else {
				bt_at_line(b, b->line);
			}
		}
		b->n = 0;
		return;
	}
	if (b->n < BT_AT_LINE_MAX) {
		b->line[b->n] = (char)c;
	}
	if (b->n <= BT_AT_LINE_MAX) {
		b->n++;
	}
}

#endif
