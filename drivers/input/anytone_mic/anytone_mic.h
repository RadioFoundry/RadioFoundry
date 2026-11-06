// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone serial hand microphone: its frames from the microphone, taken a byte at a time from a UART interrupt
// (115200 8N1 on the radio's microphone lines), and what the radio sends back. Pure and allocation free, so the host
// tests run it.
//
// From the microphone (seen on a radio; the board's hardware page has the details): a frame starts with its type byte,
// which gives its length: 'A' 8 bytes (the microphone's), 'F' 7, '+' 31 (the PC link's), 0x06 one (the microphone's
// acknowledgement). Only 'A' frames are used, and only with 0x06 in their last byte. Byte 4 0x82 is the microphone
// saying it is there; otherwise byte 1 is PTT (nonzero = pressed), byte 2 the key (1 pressed, 0 released) and byte 4
// its code (anytone_mic_key_t). Bytes 3, 5 and 6 are 0.
//
// To the microphone: ANYTONE_MIC_ACK for each accepted frame, and after a presence frame a status frame
// (anytone_mic_status). Without the status frame the microphone never gets past its presence frame: it sends that 10
// times a second and no PTT or key. Once answered it sends a frame only when something changes, nothing while idle and
// nothing while PTT is held, but answers each status frame with a run of 0x06. So the radio sends one every
// ANYTONE_MIC_POLL_MS, and any byte from the microphone keeps it present: ANYTONE_MIC_TIMEOUT_MS without one drops it
// and with it PTT and the key, so a microphone pulled out mid-transmission doesn't hold the transmitter on. The bytes
// to send come from anytone_mic_out, called from a task: the stock firmware too answers from its key loop, and this
// way an answer never lands inside a status frame.
#ifndef ANYTONE_MIC_H
#define ANYTONE_MIC_H

#include <stdbool.h>
#include <stdint.h>

#define ANYTONE_MIC_FRAME	'A'
#define ANYTONE_MIC_FRAME_LEN	8u
#define ANYTONE_MIC_END		0x06u	// an accepted frame's last byte
#define ANYTONE_MIC_ID		0x82u	// byte 4 of the presence frame
#define ANYTONE_MIC_ACK		0xAAu	// the radio's answer to each accepted frame
#define ANYTONE_MIC_MAX_LEN	31u	// the longest frame type ('+')
// No byte for this long, three polls: the microphone is gone
#define ANYTONE_MIC_TIMEOUT_MS	3000u
// A status frame this often while the microphone is present, and never closer than ANYTONE_MIC_STATUS_GAP_MS (the
// stock firmware's spacing, inferred: 20 passes of its key loop)
#define ANYTONE_MIC_POLL_MS	1000u
#define ANYTONE_MIC_STATUS_GAP_MS	200u
#define ANYTONE_MIC_STATUS_LEN	16u
// Byte 12 of the status frame: the stock firmware puts its long press setting there, in 100 ms steps less one (5:
// 600 ms, the core's long press). What the microphone does with it isn't known
#define ANYTONE_MIC_STATUS_LONG	5u

// Key codes in byte 4 (the radio's own numbering). All but MENU and EXIT seen on a radio; those two are the stock
// firmware's, which the keypad microphone's keys don't send (its C and D, labeled EXIT and MENU, are PC and PD)
typedef enum {
	ANYTONE_MIC_KEY_NONE = 0x00,
	ANYTONE_MIC_KEY_0 = 0x01,
	ANYTONE_MIC_KEY_1 = 0x02,	// .. 0x0A = 9
	ANYTONE_MIC_KEY_STAR = 0x0B,
	ANYTONE_MIC_KEY_HASH = 0x0C,
	ANYTONE_MIC_KEY_AB = 0x0D,	// the small A/B button above PTT ("A/B SUBPTT")
	ANYTONE_MIC_KEY_MENU = 0x0E,
	ANYTONE_MIC_KEY_EXIT = 0x0F,
	ANYTONE_MIC_KEY_UP = 0x10,	// channel +1
	ANYTONE_MIC_KEY_DOWN = 0x11,
	ANYTONE_MIC_KEY_PA = 0x1A,	// the programmable PA .. PD
	ANYTONE_MIC_KEY_PB = 0x1B,
	ANYTONE_MIC_KEY_PC = 0x1C,
	ANYTONE_MIC_KEY_PD = 0x1D,
} anytone_mic_key_t;

typedef struct {
	uint8_t buf[ANYTONE_MIC_FRAME_LEN];
	uint8_t n;			// bytes of the frame so far
	uint8_t len;			// its length from the type byte; 0 = waiting for a type byte
	uint32_t last_ms;		// when the last byte came, once present
	uint32_t status_ms;		// when the last status frame went out
	uint8_t acks;			// ANYTONE_MIC_ACK answers owed
	bool want_status;		// a presence frame came: a status frame next
	bool present;
	bool ptt;
	uint8_t key;			// the key code held, ANYTONE_MIC_KEY_NONE for none
	uint32_t frames, errors;	// accepted frames; bytes dropped (diagnostics)
} anytone_mic_t;

static inline void anytone_mic_init(anytone_mic_t *m)
{
	*m = (anytone_mic_t){ 0 };
}

// The frame length a type byte starts, 0 for a byte that starts none
static inline uint8_t anytone_mic_frame_len(uint8_t type)
{
	switch (type) {
	case 'A':
		return ANYTONE_MIC_FRAME_LEN;
	case 'F':
		return 7u;
	case '+':
		return ANYTONE_MIC_MAX_LEN;
	case 0x06:
		return 1u;
	default:
		return 0u;
	}
}

// A UART error (overrun, framing, noise): the frame under way is lost
static inline void anytone_mic_error(anytone_mic_t *m)
{
	m->n = 0;
	m->len = 0;
	m->errors++;
}

// One received byte at now_ms. True when it completed an accepted microphone frame (anytone_mic_out answers it)
static inline bool anytone_mic_byte(anytone_mic_t *m, uint8_t c, uint32_t now_ms)
{
	if (m->present) {
		m->last_ms = now_ms;	// any byte, the answers to a status frame too
	}
	if (m->len == 0) {
		m->len = anytone_mic_frame_len(c);
		if (m->len == 0) {
			m->errors++;
			return false;
		}
		m->n = 0;
	}
	if (m->n < ANYTONE_MIC_FRAME_LEN) {
		m->buf[m->n] = c;
	}
	if (++m->n < m->len) {
		return false;
	}
	bool mic = m->buf[0] == ANYTONE_MIC_FRAME && m->buf[ANYTONE_MIC_FRAME_LEN - 1u] == ANYTONE_MIC_END;
	m->len = 0;
	m->n = 0;
	if (!mic) {
		return false;	// another device's frame, an acknowledgement or a broken frame
	}
	m->present = true;
	m->last_ms = now_ms;
	m->frames++;
	if (m->acks < 0xFFu) {
		m->acks++;
	}
	if (m->buf[4] == ANYTONE_MIC_ID) {
		m->want_status = true;
	} else {
		m->ptt = m->buf[1] != 0;
		m->key = m->buf[2] != 0 ? m->buf[4] : ANYTONE_MIC_KEY_NONE;
	}
	return true;
}

// The status frame, all idle: byte 0 'S', bytes 1-7 the radio's state for the microphone's lights (side A busy, the
// main side's audio open, side A transmitting, side B busy, the other side's audio open, side B transmitting, the main
// side 0 A / 1 B; the stock firmware's fields, not checked on the microphone), byte 9 0x10, byte 12 the long press,
// byte 15 0x06
static inline void anytone_mic_status(uint8_t out[ANYTONE_MIC_STATUS_LEN])
{
	for (uint8_t i = 0; i < ANYTONE_MIC_STATUS_LEN; i++) {
		out[i] = 0;
	}
	out[0] = 'S';
	out[9] = 0x10;
	out[12] = ANYTONE_MIC_STATUS_LONG;
	out[ANYTONE_MIC_STATUS_LEN - 1u] = 0x06;
}

// The bytes to send the microphone at now_ms, into out (cap: room for the answers owed and a status frame): the answers
// first, then a status frame when a presence frame asked for one or a poll is due. Their count; 0 while it isn't present
static inline uint8_t anytone_mic_out(anytone_mic_t *m, uint32_t now_ms, uint8_t *out, uint8_t cap)
{
	uint8_t n = 0;
	if (!m->present) {
		m->acks = 0;
		m->want_status = false;
		return 0;
	}
	while (m->acks > 0 && n < cap) {
		out[n++] = ANYTONE_MIC_ACK;
		m->acks--;
	}
	uint32_t since = now_ms - m->status_ms;
	if ((m->want_status || since >= ANYTONE_MIC_POLL_MS) && since >= ANYTONE_MIC_STATUS_GAP_MS &&
	    (unsigned)(cap - n) >= ANYTONE_MIC_STATUS_LEN) {
		anytone_mic_status(out + n);
		n = (uint8_t)(n + ANYTONE_MIC_STATUS_LEN);
		m->status_ms = now_ms;
		m->want_status = false;
	}
	return n;
}

// From the key scan: drops the microphone after ANYTONE_MIC_TIMEOUT_MS without a byte. True while it is present
static inline bool anytone_mic_poll(anytone_mic_t *m, uint32_t now_ms)
{
	if (m->present && now_ms - m->last_ms >= ANYTONE_MIC_TIMEOUT_MS) {
		m->present = false;
		m->ptt = false;
		m->key = ANYTONE_MIC_KEY_NONE;
	}
	return m->present;
}

#endif
