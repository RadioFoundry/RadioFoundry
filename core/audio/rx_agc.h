// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Receive speech normalization, after decoding and before local sounds and user volume.
#ifndef RFOUNDRY_RX_AGC_H
#define RFOUNDRY_RX_AGC_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef enum { RX_AGC_DMR, RX_AGC_P25, RX_AGC_NXDN, RX_AGC_MODES } rx_agc_mode_t;
#define RX_AGC_TARGETS 11u
#define RX_AGC_CALLERS 10u
#define RX_AGC_SILENT (-96)

typedef struct {
	uint32_t source;
	uint32_t input_power, output_power;
	uint16_t scope;
	uint16_t speech_blocks, quiet_blocks;
	int16_t gain_q8, input_q8, output_q8;
	uint16_t scale_q12;
	uint8_t mode, choice, display_blocks;
	bool started, cached, measured;
} rx_agc_t;

typedef struct {
	int8_t input_dbfs, output_dbfs, gain_db;
	bool enabled, valid, acquiring, cached;
} rx_agc_view_t;

#if CONFIG_RX_AGC
// Choice 0 is Off; the remaining choices are stable settings values, in target order.
int8_t rx_agc_target(uint8_t choice);
// Any task: atomically publish a preference. No settings or RTOS calls in the audio task.
void rx_agc_configure(rx_agc_mode_t mode, uint8_t choice);
// Begin, identify, process and end belong to the audio task. scope distinguishes networks when known.
void rx_agc_begin(rx_agc_t *s, rx_agc_mode_t mode, uint32_t source, uint16_t scope);
void rx_agc_identify(rx_agc_t *s, uint32_t source, uint16_t scope);
// One 160-sample, 8 kHz block. learn=false for concealed/lost frames: apply gain, do not learn from them.
void rx_agc_process(rx_agc_t *s, int16_t *pcm, bool learn);
void rx_agc_end(rx_agc_t *s);
// Any task: one atomic snapshot, safe even if the reader preempts the audio task.
void rx_agc_view(rx_agc_mode_t mode, rx_agc_view_t *out);
// Before audio starts (boot and host tests): clear history, preferences and readings.
void rx_agc_reset(void);
#endif
#endif
