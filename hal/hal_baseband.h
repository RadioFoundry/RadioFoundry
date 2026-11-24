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

typedef struct {
	// Receive: discriminator samples, signed 16 bit, positive = above the carrier
	uint32_t rx_rate_hz;		// 8000 or more: the soft modem interpolates, so what a tap serves is set by its band
					// (rx_hi_hz), not by its rate
	uint8_t rx_channels;		// 1; 2 when the band is split over two pins (interleaved samples; the soft modem
					// equalizes and sums them)
	uint16_t rx_lo_hz, rx_hi_hz;	// the tap's -3 dB band; rx_lo_hz 0 = DC coupled
	uint16_t rx_counts_per_khz;	// 0 unknown
	bool (*rx_start)(void);
	// Up to max samples (frames x rx_channels) received since the last call; the board buffers 60 ms or more
	uint16_t (*rx_read)(int16_t *buf, uint16_t max);
	void (*rx_stop)(void);
	// Transmit: samples to the modulator, signed 16 bit
	uint32_t tx_rate_hz;
	uint16_t tx_lo_hz, tx_hi_hz;
	uint16_t tx_counts_per_khz;	// from the board's deviation calibration, on the tuned channel
	bool (*tx_start)(void);
	// Queue n samples; returns how many it took (the board buffers 60 ms or more and holds the last value when dry)
	uint16_t (*tx_write)(const int16_t *buf, uint16_t n);
	void (*tx_stop)(void);
	// The sample clock in microseconds (wraps); rx_read's first sample of the next call is at rx_time_us()
	uint32_t (*now_us)(void);
	uint32_t (*rx_time_us)(void);
} hal_baseband_t;

#endif
