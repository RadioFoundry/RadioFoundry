// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A RF and audio paths (common/gd88_rf.h): the TX / RX line PTB2, the audio routing in the serial output
// chain (chain_gd88.c) and the speaker amplifier on PTC18. A's band switch is its LO
// chip's GPIO4 / GPIO5 (radio_gd88.c); A has no other band, PA or supply switch.
//
// The chain is eight bytes; chain bit 8 * byte + bit:
// - bytes 0 and 1: the loudspeaker and the headset output. Bits 0-3 pick the source, bit 2 * (class - 1) + vfo, where
//   class 1 is analog FM and 2 DMR, and vfo 0 is A's own receiver, 1 is B's. Bit 5 enables the amplifier. On the radio
//   A's FM noise plays with bit 0 alone and the modem's beeps with bit 2 alone.
// - bytes 2-5, bit 4: the microphone's transmit path, one-hot (byte 2 FM A, 3 FM B, 4 DMR A, 5 DMR B). The chain is
//   an 8 x 8 analog crosspoint; bit 4 is its microphone input and bytes 2-5 the transmit audio inputs, so the DMR path
//   is the one that feeds A's codec ADC. It is held all the time, set for A's mode: without it DMR transmits silence.
// - bytes 0 and 1, bit 6: another source, taken to be the broadcast FM tuner.
// - bytes 6 and 7: the receivers' cross-feed for an A/B repeat, never written for a transmission.
// TODO(gd88): unknown, needs a radio: the headset byte is driven along with the loudspeaker byte; bit 6 as the
// broadcast tuner's source (inferred: no other bit is left for the tuner)
#include "gd88.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

#define BYTE_SPK		0u
#define BYTE_HEADSET		1u
#define BIT_AMP			5u
#define BIT_MIC			4u
#define BIT_BCAST		6u
#define VFO_A			0u
#define VFO_B			1u
// The amplifier goes off this long after the last source: off together with them it popped now and then (**HW**,
// by microphone: about 1 beep in 4 against none in 48 with the delay)
#define AMP_OFF_MS		40u

static bool spk_a, spk_b;
static bool a_dmr, b_dmr;	// the receivers' modes
static bool a_modem;		// A's speaker source is the modem's line out (DMR voice, beeps)
static bool bcast;		// the broadcast FM tuner has the speaker
static bool muted;		// volume step 0 (gd88_path_mute): the amplifier off, B's speaker switch open
static volatile bool spk_any;	// the amplifier on: B closes the switch in the speaker's path (gd88_path_speaker_wanted)
static bool amp_on, amp_off_pending;
static uint32_t amp_off_at;
// Where A receives, for B (gd88_path_wanted): A's front end and path pins are on SoC B. The TX start
// (gd88_path_rx_front_off) leaves it, since it also runs per DMR burst, faster than the link can follow
static volatile bool want_on;
static volatile uint32_t want_hz;

static uint8_t src_bit(bool dmr, uint8_t vfo)
{
	return (uint8_t)(2u * (dmr ? 1u : 0u) + vfo);
}

static uint8_t mic_byte(bool dmr, uint8_t vfo)
{
	return (uint8_t)((dmr ? 4u : 2u) + vfo);
}

static void bit(uint8_t byte, uint8_t b, bool on)
{
	gd88_chain_set((uint8_t)(8u * byte + b), on);
}

// The amplifier: the chain's bit in both groups, then PTC18. PTC18 low with the chain's bit still on pops every time
// (**HW**), so it goes last when off as well
static void amp(bool on)
{
	bit(BYTE_SPK, BIT_AMP, on);
	bit(BYTE_HEADSET, BIT_AMP, on);
	hrc_pin_write(PIN_SPK_AMP, on);
	amp_on = on;
	spk_any = on;
	amp_off_pending = false;
}

// Both output groups: the sources that play now; the amplifier on at once, off AMP_OFF_MS after the last source
// (gd88_path_service), or at once when muted
static void outputs(void)
{
	bool want = spk_a || spk_b || bcast;
	if (want && !muted && !amp_on) {
		amp(true);
	}
	for (uint8_t g = BYTE_SPK; g <= BYTE_HEADSET; g++) {
		for (uint8_t d = 0; d < 2u; d++) {
			bit(g, src_bit(d != 0, VFO_A), spk_a && (a_modem == (d != 0)));
			bit(g, src_bit(d != 0, VFO_B), spk_b && (b_dmr == (d != 0)));
		}
		bit(g, BIT_BCAST, bcast);
	}
	hrc_pin_write(PIN_DIG_AUDIO, spk_a && a_modem);
	if (muted && amp_on) {
		amp(false);
	} else if (want) {
		amp_off_pending = false;
	} else if (amp_on && !amp_off_pending) {
		amp_off_pending = true;
		amp_off_at = hal_millis() + AMP_OFF_MS;
	}
}

void gd88_path_service(uint32_t now)
{
	uint32_t s = hrc_irq_save();
	if (amp_off_pending && (int32_t)(now - amp_off_at) >= 0) {
		amp(false);
	}
	hrc_irq_restore(s);
}

// The microphone to A's transmit path for its mode, the other three off. The new path goes on before the old one goes
// off, so a mode change never leaves the microphone unconnected
static void mic_path(void)
{
	uint8_t on = mic_byte(a_dmr, VFO_A);
	bit(on, BIT_MIC, true);
	for (uint8_t byte = 2u; byte <= 5u; byte++) {
		if (byte != on) {
			bit(byte, BIT_MIC, false);
		}
	}
}

void gd88_path_init(void)
{
	gd88_chain_init();
	hrc_pin_out(PIN_TX_RX, false);
	hrc_pin_out(PIN_DIG_AUDIO, false);
	hrc_pin_out(PIN_SPK_AMP, false);
	spk_a = spk_b = a_dmr = b_dmr = a_modem = bcast = spk_any = muted = amp_on = amp_off_pending = false;
	mic_path();
}

void gd88_path_rx(uint32_t hz, bool on, bool dmr)
{
	uint32_t s = hrc_irq_save();	// A's band switch is the LO chip's GPIOs (radio_gd88.c); hz is for B
	want_on = on;
	want_hz = hz;
	if (on) {	// off (the radio's RX stopped) keeps the path of the last mode: a transmission may follow
		a_dmr = dmr;
	}
	mic_path();
	hrc_irq_restore(s);
}

bool gd88_path_wanted(uint32_t *hz)
{
	uint32_t s = hrc_irq_save();
	bool on = want_on;
	*hz = want_hz;
	hrc_irq_restore(s);
	return on;
}

bool gd88_path_speaker_wanted(void)
{
	return spk_any;
}

// TX start: PTB2 high. The microphone's path is already set for A's mode (mic_path), so nothing in the chain changes
void gd88_path_rx_front_off(void)
{
	hrc_pin_write(PIN_TX_RX, true);
}

void gd88_path_tx_off(void)
{
	hrc_pin_write(PIN_TX_RX, false);
}

void gd88_path_speaker(bool on, bool from_modem)
{
	spk_a = on;
	a_modem = from_modem;
	outputs();
}

void gd88_path_mute(bool on)
{
	uint32_t s = hrc_irq_save();
	if (on != muted) {
		muted = on;
		outputs();
	}
	hrc_irq_restore(s);
}

void gd88_audio_b(bool on, bool dmr)
{
	uint32_t s = hrc_irq_save();
	if (on != spk_b || dmr != b_dmr) {	// called at the link's pace: the chain is shifted out only for a change
		spk_b = on;
		b_dmr = dmr;
		outputs();
	}
	hrc_irq_restore(s);
}

void gd88_audio_bcast(bool on)
{
	uint32_t s = hrc_irq_save();
	bcast = on;
	outputs();
	hrc_irq_restore(s);
}
