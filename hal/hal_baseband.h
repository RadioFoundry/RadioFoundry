// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Sample taps (docs/spec/air-interface.md 6): the discriminator read by the MCU and samples from the MCU into the
// modulator, on a board that has either path. The soft modem (core/air) turns them into an air interface provider
// for every profile the tap's band allows. Either direction may be missing (its start function NULL).
//
// The isolation rules of hal_air.h hold from rx_start / tx_start to the matching stop: raw receive never reaches the
// speaker, and nothing but tx_write reaches the modulator.
#ifndef HAL_BASEBAND_H
#define HAL_BASEBAND_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_traits.h"

typedef struct {
	// Receive: discriminator samples, signed 16 bit, positive = above the carrier
	uint32_t rx_rate_hz;		// 8000 or more: the soft modem interpolates, so what a tap serves is set by its band
					// (rx_hi_hz), not by its rate
	uint8_t rx_channels;		// 1; 2 when the band is split over two pins (interleaved samples; the soft modem
					// equalizes and sums them)
	uint16_t rx_lo_hz, rx_hi_hz;	// the tap's -3 dB band; rx_lo_hz 0 = DC coupled
	uint16_t rx_counts_per_khz;	// 0 unknown
	// 0: the samples are the deviation (a flat discriminator). Else they are its derivative (they rise 6 dB an octave
	// over rx_lo_hz..rx_hi_hz): receive integrates them with a leak near this corner, in Hz, and tracks the level
	// (rx_counts_per_khz is not used). Taken only where board.mk says BOARD_SLOPE_TAP := y (hal_traits.h)
	uint16_t rx_slope_hz;
	bool (*rx_start)(void);
	// Up to max samples (frames x rx_channels) received since the last call; the board buffers 60 ms or more
	uint16_t (*rx_read)(int16_t *buf, uint16_t max);
	void (*rx_stop)(void);
	// Transmit: samples to the modulator, signed 16 bit, positive = above the carrier. The soft modem runs them
	// through tx_eq first, so the band and scale below are what the path gives with it
	uint32_t tx_rate_hz;
	uint16_t tx_lo_hz, tx_hi_hz;	// the path's band, flat in level and delay, with tx_eq
	uint16_t tx_counts_per_khz;	// counts per kHz of deviation in that band (with tx_eq); 0: no transmit
	// The path's equalizer (NULL: flat): tx_eq_n taps at tx_rate_hz, Q14, the board's measured inverse of its
	// modulator, 1 at 1 kHz. Tap tx_eq_at is time 0; the taps before it look ahead (a path whose inverse needs
	// that: the soft modem delays its output to give it)
	const int16_t *tx_eq;
	uint16_t tx_eq_n, tx_eq_at;
	// The path's low end (NULL: none to undo): tx_lf_n taps, Q14, a filter on the symbol levels at tx_lf_rate symbols
	// a second (a profile at that rate or half of it), tap tx_lf_at the symbol itself, the taps before it ahead. A path
	// with high-pass corners below its band (an audio input's coupling) makes a run of one level sag; this undoes
	// them (tools/air_tx_lf.py makes it from the corners). The soft modem delays its output by the look-ahead
	const int16_t *tx_lf;
	uint8_t tx_lf_n, tx_lf_at;
	uint16_t tx_lf_rate;
	// Start sending: from the call the board clocks the queued samples out at tx_rate_hz on the now_us clock;
	// false when it can't. Called with the transmitter keyed and the receive tap stopped (a board may share their
	// memory)
	bool (*tx_start)(void);
	// Queue n samples; returns how many it took (the board buffers 40 ms or more and holds the last value when dry)
	uint16_t (*tx_write)(const int16_t *buf, uint16_t n);
	void (*tx_stop)(void);
	// The sample clock in microseconds (wraps); rx_read's first sample of the next call is at rx_time_us()
	uint32_t (*now_us)(void);
	uint32_t (*rx_time_us)(void);
} hal_baseband_t;

#endif
