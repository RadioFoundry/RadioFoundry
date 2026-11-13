// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
	uint8_t noise;		// chip noise reading, larger = more noise (1 dB units where the chip allows); UINT8_MAX when it can't hear
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
	// Optional (DMR): the end of a DMR transmission, once the stack is off the air. Ends it as tx_off() does, but
	// leaves the receiver as the last burst's switch left it when that is on the last rx() settings: a reply can come
	// 30 ms after our terminator, in our own timeslot, and a receive set-up then misses it. The modem glue implements
	// it (hrc_dmr_tx_end, stm32_hrc_dmr_tx_end). False: nothing done, the core calls tx_off(). NULL = always tx_off()
	bool (*dmr_tx_end)(void);
	// Optional (DMR): for a board whose tx() leaves the receiver deaf between bursts (RF chips held on the transmit
	// frequency). true: a repeater wake-up burst has gone, receive on the last rx() settings for the answer; false: back
	// to the transmit set-up of the last tx(), for the next burst. NULL: the modem switches between bursts by itself
	void (*dmr_wake_listen)(bool listen);
	void (*status)(radio_status_t *st);
	// Optional: a check of the receiver between status polls, for a board that watches it faster than the squelch runs
	// (the D578 shuts its speaker the moment an FM carrier goes). The radio task calls it again after the ms it returns,
	// 0 = at its next tick (RADIO_TICK_MS). Short; NULL = none
	uint8_t (*rx_watch)(void);
	// Speaker path (amp and mute) for received audio
	void (*audio)(bool on);
	// Optional: received audio level 0..100 % (radios whose volume knob is read by the MCU, not wired to the amp)
	void (*volume)(uint8_t percent);
	// With volume: true when that level also scales the PCM speaker path (beeps, DMR voice). False: beeps on the PCM
	// path would ignore it, so the core caps each beep at the volume percent (radio_beep_level)
	bool volume_pcm;
	// Optional, for a board without PCM speaker audio (board.audio NULL): sound a beep tone of hz at level 1..100 to
	// the speaker, whatever the squelch; 0 = off. Level 0 with hz: the tone runs but stays off the speaker
	// (beep_lead_ms). The core times each tone to the millisecond. Ignored while the radio transmits
	void (*beep_tone)(uint16_t hz, uint8_t level);
	// With beep_tone: ms each tone runs silent before it sounds, for a tone that takes time to settle; 0 = none
	uint8_t beep_lead_ms;
	// Optional: during FM TX, send a single audio tone of hz instead of the microphone (1750 Hz burst, roger beep);
	// 0 = the microphone again. False when the board can't (not in FM TX)
	bool (*tx_tone)(uint16_t hz);
	// Optional, with tx_tone: key the tone on and off without giving the microphone back (Morse keying, the hotspot's
	// CW ID): false = a silent carrier, true = the tone again. Only while a tone set with tx_tone runs. Without it the
	// core keys with tx_tone(hz) / tx_tone(0), which lets the microphone through in the gaps
	bool (*tx_tone_key)(bool down);
	// Optional (CONFIG_DTMF): during FM TX, send the DTMF pair of digit ('0'-'9', '*', '#', 'A'-'D'; hal_dtmf.h)
	// instead of the microphone until called again; 0 = the microphone again. A new digit replaces the one sounding.
	// False when the board can't (not in FM TX, not a digit). The core never mixes it with tx_tone
	bool (*tx_dtmf)(char digit);
	// Optional: the turn-off tail of the TX tone tx() set, at the end of an FM transmission (DCS: the 134.4 Hz tone that
	// closes the receivers' squelch before the carrier drops). on: start it; false when there is none (no DCS, or the
	// chip can't), and the core unkeys at once. off: stop it, still keyed. The core keeps the carrier up
	// RADIO_SUBTONE_TAIL_MS between the two, then calls tx_off(), which also ends a tail still running. PTT is up for
	// the tail, so the microphone is off from on until off or tx_off()
	bool (*tx_tail)(bool on);
	// Optional: receiver power saving. true = receiver off (no status until woken), false = receive again with the
	// last rx() settings
	void (*rx_sleep)(bool sleep);
	// Optional (CAP_AM_RX): tune and receive AM (the air band) instead of FM. Receive only: the core never calls tx()
	// while AM is on. The board leaves AM at the next rx() or set_mode(). No sub-audio in AM (the core takes
	// tone_ok as true). In AM status() reports noise on the squelch's scale (larger = weaker) from whichever of its
	// receiver's readings tells a carrier from an empty channel in AM, with its own scale and receiver gain taken into
	// account. False: out of band or the board can't. NULL = no AM on this radio
	bool (*rx_am)(uint32_t hz, radio_bw_t bw);
	// Optional: the microphone gain of FM and DMR transmissions, db relative to the board's own levels (the Mic gain
	// setting, -10..+10; core/radio/mic_gain.h). The board only stores it and applies it at the next transmission
	// start, so any task may call it. NULL = no gain in the board's audio path: DMR then scales the samples before the
	// vocoder and FM stays as it is
	void (*mic_gain)(int8_t db);
	// Optional: the reference oscillator trim, parts per billion (the Freq trim setting, -10000..+10000 in steps of
	// 100; core/radio/ref_trim.h). Positive raises the reference. It moves receive and transmit together, FM and DMR. Any task may call it. The board applies
	// it now (false) or at the next tune (true: the caller asks the radio task for one, radio_req_retune). NULL = the
	// radio has no trim
	bool (*ref_trim)(int16_t ppb);
	// Optional, both or neither: the PA drive, in the board's own units (its power control DAC counts), for transmit
	// power calibration with a wattmeter (docs/testing/power-calibration.md). tx_drive_level: the drive the board uses
	// for a power level at a transmit frequency, 0 when it has none (no calibration, out of band). tx_drive: the drive
	// for the next transmission instead of its level's, from the next tx() until tx_off() (one transmission); 0 = the
	// level's again. False: over the board's hardware limit. Any task may call it
	uint16_t (*tx_drive_level)(uint32_t hz, uint8_t level);
	bool (*tx_drive)(uint16_t drive);
} hal_radio_t;

#endif
