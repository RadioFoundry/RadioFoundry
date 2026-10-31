// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The two RF chips of a superheterodyne radio, whichever parts a board has: the main transceiver (receives at the IF,
// transmits on the channel) and the local oscillator. A chip driver that can fill a role provides the ops for it
// (bk4819_superhet_main, fd6818sh_superhet_lo, at1141_superhet_lo, ...); the board detects its chips, binds each to
// its ops with the driver's device, and calls only these. Generic: no radio, no pin, no IF frequency in here.
#ifndef SUPERHET_H
#define SUPERHET_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { SUPERHET_TONE_NONE, SUPERHET_TONE_CTCSS, SUPERHET_TONE_DCS, SUPERHET_TONE_DCS_INV } superhet_tone_t;
typedef enum { SUPERHET_AF_MUTE, SUPERHET_AF_FM, SUPERHET_AF_DMR } superhet_af_t;

// The main transceiver. Every op is required. States: idle, receive, transmit; the driver goes through idle between
// them as its chip needs
typedef struct {
	void (*idle)(void *chip);
	void (*rx)(void *chip);					// receive at the frequency set
	void (*to_if)(void *chip, uint32_t if_hz);		// idle, tuned to the IF
	void (*bandwidth)(void *chip, bool wide);		// analog receive: 25 kHz or 12.5 kHz
	void (*rx_tone)(void *chip, superhet_tone_t type, uint16_t value);	// Hz x 10, or the 9-bit DCS code
	void (*af)(void *chip, superhet_af_t af);
	void (*af_gain)(void *chip, uint8_t gain);		// 0..15
	// FM transmit: words written while still receiving (pa_gain 0..15, for chips with a PA gain), the tune into TX,
	// the end back to receive on the TX frequency
	void (*tx_prepare)(void *chip, superhet_tone_t type, uint16_t value, uint32_t hz, uint8_t pa_gain);
	void (*tx_tune)(void *chip, uint32_t hz);
	void (*tx_end)(void *chip);
	bool (*tail)(void *chip, bool on);			// turn-off tail on (false: none for this transmission) / off
	void (*tone)(void *chip, uint16_t hz, uint8_t gain);	// TX tone in place of the microphone; hz 0 = stop
	bool (*tone_on)(void *chip);
	void (*tone_key)(void *chip, bool down);
	void (*dtmf)(void *chip, uint16_t low_hz, uint16_t high_hz);	// low 0 = stop
	// DMR: the digital set (af_gain = the AF gain the modem hears), its end, the TX words, the per-burst tune
	void (*dmr_enter)(void *chip, uint8_t af_gain);
	void (*dmr_leave)(void *chip);
	void (*dmr_tx_prepare)(void *chip, uint8_t pa_gain);
	void (*burst)(void *chip, uint32_t hz, bool tx);
	void (*gpio)(void *chip, uint8_t pin, bool high);	// NULL: the chip's GPIOs aren't driven
} superhet_main_ops_t;

// The local oscillator
typedef struct {
	void (*tune)(void *chip, uint32_t hz);		// run at hz; the frequency is written only when it changed
	void (*off)(void *chip);			// stopped for a transmission, the frequency kept
	void (*resume)(void *chip);			// running again, not retuned
	void (*gpio)(void *chip, uint8_t pin, bool high);	// NULL: the chip's GPIOs aren't driven
} superhet_lo_ops_t;

typedef struct {
	const superhet_main_ops_t *ops;
	void *chip;
} superhet_main_t;

typedef struct {
	const superhet_lo_ops_t *ops;
	void *chip;
} superhet_lo_t;

static inline void superhet_main_gpio(const superhet_main_t *m, uint8_t pin, bool high)
{
	if (m->ops->gpio != NULL) {
		m->ops->gpio(m->chip, pin, high);
	}
}

static inline void superhet_lo_gpio(const superhet_lo_t *lo, uint8_t pin, bool high)
{
	if (lo->ops->gpio != NULL) {
		lo->ops->gpio(lo->chip, pin, high);
	}
}

#endif
