// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV: the plain hand microphone's two key lines, the backlight level and the speaker amplifier and its hang
// (docs/spec/hardware-d578uv.md 3.2, 3.1, 4.4). Pure, so the host tests check them; input_d578uv.c and radio_d578uv.c
// apply them.
#ifndef D578UV_FRONT_IO_H
#define D578UV_FRONT_IO_H

#include <stdbool.h>
#include <stdint.h>

// --- Plain microphone (PA9, PA10; pull-ups, low = pulled by the microphone) ---
// PA10 low alone is PTT, PA9 low alone the microphone's Up key, both low its Down key. Up and Down are inferred, not
// checked on a microphone. A key that pulls both lines can show
// PTT for an instant while its lines settle; the core's debounce (two agreeing scans) drops that.
typedef enum {
	MIC_KEY_NONE,
	MIC_KEY_PTT,
	MIC_KEY_UP,
	MIC_KEY_DOWN,
} mic_key_t;

static inline mic_key_t mic_key_decode(bool pa9_low, bool pa10_low)
{
	if (pa10_low) {
		return pa9_low ? MIC_KEY_DOWN : MIC_KEY_PTT;
	}
	return pa9_low ? MIC_KEY_UP : MIC_KEY_NONE;
}

// --- Backlight: register 8 of the PA5 device ---
// The radio's "Back Light" menu setting has five levels (codes, dimmest first); 0 is off, as at power-off and standby.
// Four of the five bytes sit close together, so brightness is likely far from linear in the byte (inferred), and no
// other byte is known to look right: the core's percent picks one of the five. 0 % is off, 1-20 % the first level,
// 21-40 % the second and so on, 81-100 % the brightest (two of the core's 10 % brightness steps per level)
static inline uint8_t backlight_code(uint8_t percent)
{
	static const uint8_t codes[5] = { 0x38, 0x3D, 0x42, 0x4B, 0xFF };
	if (percent == 0) {
		return 0;
	}
	if (percent > 100u) {
		percent = 100u;
	}
	return codes[(percent - 1u) / 20u];
}

// --- Speaker amplifier: PA7, high = off ---
// On only while something plays and its knob is up: with it on and nothing playing, a hiss that doesn't follow the
// volume can come from the speaker (heard on a radio). Off while transmitting. playing: side A's audio, side B's, a
// beep or the KTMicro receiver; loud: the knob of a side that plays is above 0
static inline bool spk_amp_off(bool transmitting, bool playing, bool loud)
{
	return transmitting || !playing || !loud;
}

// The amplifier's hang: once on, it stays on beep_ms after a beep and rx_ms after other audio, so a run of key beeps
// or a flickering squelch turns it on once (each turn-on pops). Off at once while transmitting or with the volume at 0
// (may false). want: !spk_amp_off(); beep: a beep (or its lead) is what plays. True: PA7 on. The radio uses no hang
// (SPK_HANG_BEEP_MS, SPK_HANG_RX_MS 0): the amplifier is off between sounds, and each sound's turn-on pop was judged
// better than the hang's hiss by ear (400 ms after a beep and 300 ms after the receiver's audio were tried)
#define SPK_HANG_BEEP_MS	0u
#define SPK_HANG_RX_MS		0u
typedef struct {
	bool on;
	bool beep;		// the last sound was a beep
	bool hanging;		// nothing plays: on until `until`
	uint32_t until;
	uint16_t beep_ms, rx_ms;	// the hang after a beep and after other audio
} spk_hang_t;

static inline bool spk_hang(spk_hang_t *h, bool want, bool beep, bool may, uint32_t now)
{
	if (want) {
		h->on = true;
		h->beep = beep;
		h->hanging = false;
		return true;
	}
	if (h->on && may && !h->hanging) {
		h->hanging = true;
		h->until = now + (h->beep ? h->beep_ms : h->rx_ms);
	}
	if (!h->on || !may || (int32_t)(now - h->until) >= 0) {
		h->on = false;
		h->hanging = false;
		return false;
	}
	return true;
}

#endif
