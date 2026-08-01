// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Radio: what the core asks of the RF side, whatever chips the board has.
// The board composes its RF chip, modem / audio chip, band and PA switching and calibration behind this.
#ifndef HAL_RADIO_H
#define HAL_RADIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { RADIO_MODE_FM, RADIO_MODE_DMR } radio_mode_t;
typedef enum { RADIO_BW_12K5, RADIO_BW_25K } radio_bw_t;

typedef enum { TONE_NONE, TONE_CTCSS, TONE_DCS, TONE_DCS_INV } tone_type_t;
typedef struct {
	uint8_t type;		// tone_type_t
	uint16_t value;		// CTCSS: Hz x 10 (1000 = 100.0 Hz). DCS: the code as written, octal digits (023 -> 0x13 = 19 = 023 octal)
} subtone_t;

typedef struct {
	int16_t rssi_dbm;
	uint8_t noise;		// chip noise reading, larger = more noise (1 dB units where the chip allows)
	bool carrier;		// the chip's own squelch says carrier (if it has one)
	bool tone_ok;		// the RX sub-audio tone or code matches (true when none is set)
} radio_status_t;

typedef struct {
	void (*init)(void);
	void (*set_mode)(radio_mode_t mode);
	// Tune and receive. Out-of-band frequencies are refused (false)
	bool (*rx)(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone);
	// Transmit FM (or key up for DMR) on hz at a board power level index. False: not allowed here
	bool (*tx)(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone);
	// Back to receive with the last rx() settings
	void (*tx_off)(void);
	void (*status)(radio_status_t *st);
	// Speaker path (amp and mute) for received audio
	void (*audio)(bool on);
	// Optional: received audio level 0..100 % (radios whose volume knob is read by the MCU, not wired to the amp)
	void (*volume)(uint8_t percent);
} hal_radio_t;

#endif
