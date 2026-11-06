// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KTMicro AM/FM receiver (chip ID "KT" in registers 0x02/0x03): chip driver, independent of the board. No public
// register map was found; every register and value here is the sequence a stock radio firmware writes, read by
// static analysis, and the role given to each is inferred (docs/spec/airband-and-broadcast.md 2). Two modes: AM, which
// the radios that carry it use as the demodulator behind their RF chip's mixer (an air band superhet at an IF the chip
// tunes as an AM station), and broadcast FM on the chip's own input. The chip has no seek and no tune-done flag the
// stock firmware uses; it reads a station by its signal readings (ktamfm_fm_status).
//
// The board supplies 8-bit register access (I2C at 8-bit address 0x6A, register then data; a read is the register,
// repeated START, one byte) and a delay. Changes read the register and write it back. Not reentrant: one task.
#ifndef KTAMFM_H
#define KTAMFM_H

#include <stdint.h>
#include <stdbool.h>

#define KTAMFM_I2C_ADDR		0x35u	// 7-bit (0x6A / 0x6B on the wire)
#define KTAMFM_ID		0x4B54u	// registers 0x02 (high) and 0x03: "KT"

#define KTAMFM_VOL_MASK		0x1Fu	// register 0x0F bits 4:0, the audio level
#define KTAMFM_VOL_MUTE		0x01u	// the level the stock firmware writes to mute
#define KTAMFM_VOL_AM		0x1Eu	// its AM listening level
#define KTAMFM_VOL_FM		0x1Au	// its broadcast FM listening level

#define KTAMFM_FM_STEP_KHZ	50u	// broadcast FM channel step: the tune word counts 50 kHz
#define KTAMFM_FM_MAX_KHZ	(0xFFFu * KTAMFM_FM_STEP_KHZ)	// the word's 12 bits

typedef struct {
	bool (*read)(uint8_t reg, uint8_t *val);
	bool (*write)(uint8_t reg, uint8_t val);
	void (*delay_us)(uint32_t us);	// may sleep for the longer waits (50 ms at power-up)
} ktamfm_bus_t;

typedef struct {
	const ktamfm_bus_t *bus;
	bool ok;		// every transfer since ktamfm_attach was acknowledged
	bool ready;		// ktamfm_init ran
	bool am;		// AM mode set
	bool fm;		// broadcast FM mode set
} ktamfm_t;

typedef struct {
	uint8_t level;		// 0xEA bits 6:0: rises with the signal (the stock squelch's level, 1 count per step of 3)
	uint8_t carrier;	// 0xEC bits 6:0: the stock squelch's second reading, compared with a fixed calibrated floor
} ktamfm_status_t;

// Broadcast FM readings and the stock firmware's station test on them (scales inferred)
typedef struct {
	uint8_t rssi;		// 0xE6 bits 6:0: the signal strength
	uint8_t snr;		// 0xE2 bits 5:0: rises with a clean signal
	uint8_t offset;		// |2 x (0x79 bits 6:0) - 0xE9|: small on a station's center (a tuning offset, inferred)
	bool station;		// rssi > 14, snr >= 12 and offset < 12 (the stock firmware debounces it over 3 polls)
} ktamfm_fm_status_t;

void ktamfm_attach(ktamfm_t *k, const ktamfm_bus_t *bus);
// The chip answers with its ID
bool ktamfm_probe(ktamfm_t *k);
// Wake and set up, once; leaves the audio muted. False on a bus error or when the chip never says it is ready
bool ktamfm_init(ktamfm_t *k);
// AM mode with the stock AM front-end values; the level stays as it was
bool ktamfm_am(ktamfm_t *k);
// Tune AM to khz (up to 32767 kHz)
bool ktamfm_tune_am(ktamfm_t *k, uint32_t khz);
// Broadcast FM mode (0x88 bit 6 clear); the level stays as it was. The AM mode's other values stay too, as in the
// stock firmware
bool ktamfm_fm(ktamfm_t *k);
// Tune broadcast FM to khz, a multiple of KTAMFM_FM_STEP_KHZ up to KTAMFM_FM_MAX_KHZ
bool ktamfm_tune_fm(ktamfm_t *k, uint32_t khz);
bool ktamfm_fm_status(ktamfm_t *k, ktamfm_fm_status_t *st);
// The audio level, 0..31 (KTAMFM_VOL_MUTE mutes)
bool ktamfm_volume(ktamfm_t *k, uint8_t level);
bool ktamfm_status(ktamfm_t *k, ktamfm_status_t *st);

#endif
