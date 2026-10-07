// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Taps on the radio's audio path beyond the microphone and speaker of hal_audio.h: receive audio to the MCU and MCU
 * audio into the transmitter, for the consumers that need raw audio instead of the radio's own analog path:
 *
 *   - USB / network audio (docs/spec/usb-audio.md): a computer hears the receiver and talks through the transmitter
 *   - APRS / AFSK on FM (core/aprs): the 1200 baud demodulator samples receive audio, the modulator sends tones
 *   - later: DTMF / 5-tone decoding in software, voice recording
 *
 * Both directions ride on the board's one PCM stream (hal_audio_t: 8 kHz, 16-bit, 20 ms blocks). A tap only changes
 * what the stream's "mic" samples are (receive audio instead of the microphone) and where its "spk" samples go (the
 * modulator instead of, or as well as, the speaker). The core multiplexes the one stream between its users (DMR voice,
 * beeps, APRS, USB audio): see usb-audio.md 5.1 for the router that owns hal_audio_t.start() once there is more than
 * one consumer.
 *
 * Status: a SKETCH. Only the probe part (bottom) is implemented, on the DM-1701 and behind CONFIG_AUDIO_PROBE, to find
 * out on the bench which of the HR_C6000 settings put FM audio on the I2S link at all (usb-audio.md 2.3). The
 * routing part is the interface the USB audio and APRS work packages are meant to share; no board implements it yet,
 * and it may change once the probe results are in.
 */
#ifndef HAL_AUDIO_TAP_H
#define HAL_AUDIO_TAP_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_audio.h"

// What a board can tap (hal_audio_tap_t.caps)
enum {
	AUDIO_TAP_FM_RX		= 1u << 0,	// FM receive audio (demodulated, de-emphasized) as the stream's input
	AUDIO_TAP_FM_TX		= 1u << 1,	// the stream's output as the FM modulation source
	AUDIO_TAP_DMR		= 1u << 2,	// DMR voice as PCM both ways: always there with a vocoder (the stream itself)
	AUDIO_TAP_FM_RX_RAW	= 1u << 3,	// FM receive audio before de-emphasis and the voice filter (better for AFSK)
	AUDIO_TAP_FM_TX_FLAT	= 1u << 4,	// the modulator without pre-emphasis, compressor and voice filter
};

// What the FM taps carry (usb-audio.md 5.5): voice audio, or the flat signal for data modems with their own waveform
typedef enum {
	AUDIO_TAP_VOICE = 0,	// RX de-emphasized, voice band; TX with the radio's pre-emphasis, filters and tone
	AUDIO_TAP_FLAT	= 1,	// RX the discriminator; TX straight to the modulator (limiter kept)
} audio_tap_mode_t;

typedef struct {
	uint32_t caps;

	// --- routing (sketch, not implemented by any board yet) ---

	// The stream's input becomes FM receive audio in the given mode (true) or the microphone again (false). FM
	// receive mode only; the speaker keeps working as before. False if the board can't (or not in that mode)
	bool (*fm_rx_capture)(bool on, audio_tap_mode_t mode);
	// While transmitting FM: the stream's output becomes the modulation source instead of the microphone (true), or
	// the microphone again (false). The deviation stays the radio's; in AUDIO_TAP_VOICE the sub-audio tone too, in
	// AUDIO_TAP_FLAT the caller says (tone). False if the board can't (or not in that mode)
	bool (*fm_tx_inject)(bool on, audio_tap_mode_t mode, bool tone);
	// Sample rate of AUDIO_TAP_FLAT receive audio when it doesn't come through the 8 kHz stream (an MCU ADC on the
	// discriminator); 0 = it rides the stream
	uint32_t fm_rx_flat_rate_hz;
	// Level of injected TX audio that gives the channel's nominal deviation (60 % of the peak, e.g. 1.5 kHz on a
	// 12.5 kHz channel), as a 16-bit sample peak; lets the core scale host audio. 0 = unknown
	int16_t fm_tx_nominal_peak;

	// --- probe (diagnostics, CONFIG_AUDIO_PROBE; NULL otherwise) ---

	// Read (write false) or write (write true) one register of the chip that owns the audio path (DM-1701: the
	// HR_C6000, page and register as in docs/spec/modem-hrc6000.md). Returns false when the bus failed
	bool (*modem_reg)(uint8_t page, uint8_t reg, uint8_t *val, bool write);
	// Read or write len bytes of the same chip's RAM from an 11-bit address (the HR_C6000's 1.2K TX / RX RAMs on
	// page 2, which hold its FM data buffers). NULL when the chip has none. Returns false when the bus failed
	bool (*modem_ram)(uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write);
	// Write len bytes from an 8-bit address of a page in one transaction (the HR_C6000's prompt tone FIFO: page 3,
	// address 0, 32 samples). NULL when the chip has none
	bool (*modem_burst)(uint8_t page, uint8_t reg, const uint8_t *buf, uint16_t len);
	// The last 20 ms block as received from the codec, both I2S channels (AUDIO_BLOCK_SAMPLES each), and the stream's
	// block counter; false when the stream isn't running
	bool (*last_block)(int16_t *left, int16_t *right, uint32_t *blocks);
	// A square wave of hz (0 = off, back to its idle level) on a spare output the board names (DM-1701: the stock
	// beep PWM, PC8), to find out whether that net reaches the transmit audio. NULL when the board has none
	void (*pin_tone)(uint16_t hz);
	// Sample every MCU GPIO input for ms and set, per port (A, B, ...), the pins that changed; returns the ports
	// filled (at most max). Finds which MCU pins a clock from the audio chip reaches. NULL when not supported
	uint8_t (*pin_scan)(uint16_t ms, uint16_t *changed, uint8_t max);
} hal_audio_tap_t;

#endif
