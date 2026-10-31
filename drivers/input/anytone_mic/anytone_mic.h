// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone serial hand microphone: its frames from the microphone, taken a byte at a time from a UART interrupt
// (115200 8N1 on the radio's microphone lines). Pure and allocation free, so the host tests run it.
//
// The stock firmware's reading (static analysis; the board's hardware page has the details): a frame starts with its
// type byte, which gives its length: 'A' 8 bytes (the microphone's), 'F' 7, '+' 31 (the PC link's), 0x06 one (an
// acknowledgement). Only 'A' frames are used, and only with 0x06 in their last byte. Byte 4 0x82 is the microphone
// saying it is there; otherwise byte 1 is PTT (nonzero = pressed), byte 3 the key-valid flag and byte 4 the key code
// (the radio's own key codes: anytone_mic_key_t). The radio answers each accepted frame with ANYTONE_MIC_ACK. Byte 2
// (a key state) and bytes 5 and 6 are not used: what byte 2 adds to the valid flag isn't known, so a key counts as held
// while the last key frame had its valid flag set (inferred).
//
// The microphone counts as present while frames keep coming: ANYTONE_MIC_TIMEOUT_MS without one drops it and with it
// PTT and the key, so a microphone pulled out mid-transmission doesn't hold the transmitter on.
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
// No frame for this long: the microphone is gone (the stock firmware's presence count is 250 of its ticks, whose
// length isn't known; inferred)
#define ANYTONE_MIC_TIMEOUT_MS	3000u

// Key codes in byte 4 (the radio's own numbering). 0x01-0x0A as the digits 1-9 and 0, 0x0B '*' and 0x0C '#' are
// inferred from the stock firmware's digit entry
typedef enum {
	ANYTONE_MIC_KEY_NONE = 0x00,
	ANYTONE_MIC_KEY_1 = 0x01,	// .. 0x09 = 9
	ANYTONE_MIC_KEY_0 = 0x0A,
	ANYTONE_MIC_KEY_STAR = 0x0B,
	ANYTONE_MIC_KEY_HASH = 0x0C,
	ANYTONE_MIC_KEY_AB = 0x0D,	// the main channel switch (A / B)
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
	uint32_t last_ms;		// when the last frame was accepted
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

// One received byte at now_ms. True when it completed an accepted microphone frame: the caller answers
// ANYTONE_MIC_ACK
static inline bool anytone_mic_byte(anytone_mic_t *m, uint8_t c, uint32_t now_ms)
{
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
		return false;	// another device's frame, or a broken one
	}
	m->present = true;
	m->last_ms = now_ms;
	m->frames++;
	if (m->buf[4] != ANYTONE_MIC_ID) {
		m->ptt = m->buf[1] != 0;
		m->key = m->buf[3] != 0 ? m->buf[4] : ANYTONE_MIC_KEY_NONE;
	}
	return true;
}

// From the key scan: drops the microphone after ANYTONE_MIC_TIMEOUT_MS without a frame. True while it is present
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
