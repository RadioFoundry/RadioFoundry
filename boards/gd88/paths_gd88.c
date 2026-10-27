// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A RF and audio paths (common/gd88_rf.h): the TX / RX line PTB2, the audio routing in the serial output
// chain (chain_gd88.c) and the speaker amplifier on PTC18 (docs/spec/hardware-gd88.md 9.3). A's band switch is its LO
// chip's GPIO4 / GPIO5 (radio_gd88.c); A has no other band, PA or supply switch.
//
// The chain is eight bytes; chain bit 8 * byte + bit:
// - bytes 0 and 1: the loudspeaker and the headset output. Bits 0-3 pick the source, bit 2 * (class - 1) + vfo, where
//   class 1 is analog FM and 2 DMR, and vfo 0 is A's own receiver, 1 is B's. Bit 5 enables the amplifier.
// - bytes 2-5, bit 4: the receive path in use, one-hot (byte 2 DMR A, 3 DMR B, 4 FM A, 5 FM B).
// - bytes 0 and 1, bit 6: another source, taken to be the broadcast FM tuner.
// - bytes 6 and 7: the transmit route, the source bit of the transmitting (class, vfo). Byte 6 is used.
// TODO(gd88): unknown, needs a radio: class 1 = FM and class 2 = DMR are inferred, not seen; byte 6 against byte 7 for
// the TX route (they differ by a volume range); the headset byte is driven along with the loudspeaker byte; bit 6 as
// the broadcast tuner's source (the vendor code sets it for "no source / tones", and nothing else is left for the tuner)
#include "gd88.h"
#include "pins.h"
#include "platform.h"

#define BYTE_SPK		0u
#define BYTE_HEADSET		1u
#define BYTE_TX_ROUTE		6u
#define BIT_AMP			5u
#define BIT_RX_PATH		4u
#define BIT_BCAST		6u
#define VFO_A			0u
#define VFO_B			1u

static bool spk_a, spk_b;
static bool a_dmr, b_dmr;	// the receivers' modes
static bool a_modem;		// A's speaker source is the modem's line out (DMR voice, beeps)
static bool receiving;		// A's receive path is on (not transmitting)
static bool bcast;		// the broadcast FM tuner has the speaker
static volatile bool spk_any;	// any of them: B closes the switch in the speaker's path (gd88_path_speaker_wanted)
// Where A receives, for B (gd88_path_wanted): A's front end and path pins are on SoC B. The TX start
// (gd88_path_rx_front_off) leaves it, since it also runs per DMR burst, faster than the link can follow
static volatile bool want_on;
static volatile uint32_t want_hz;

static uint8_t src_bit(bool dmr, uint8_t vfo)
{
	return (uint8_t)(2u * (dmr ? 1u : 0u) + vfo);
}

static uint8_t rx_byte(bool dmr, uint8_t vfo)
{
	return (uint8_t)((dmr ? 2u : 4u) + vfo);
}

static void bit(uint8_t byte, uint8_t b, bool on)
{
	gd88_chain_set((uint8_t)(8u * byte + b), on);
}

// Both output groups: the sources that play now, the amplifier
static void outputs(void)
{
	for (uint8_t g = BYTE_SPK; g <= BYTE_HEADSET; g++) {
		for (uint8_t d = 0; d < 2u; d++) {
			bit(g, src_bit(d != 0, VFO_A), spk_a && (a_modem == (d != 0)));
			bit(g, src_bit(d != 0, VFO_B), spk_b && (b_dmr == (d != 0)));
		}
		bit(g, BIT_BCAST, bcast);
		bit(g, BIT_AMP, spk_a || spk_b || bcast);
	}
	spk_any = spk_a || spk_b || bcast;
	hrc_pin_write(PIN_SPK_AMP, spk_any);
	hrc_pin_write(PIN_DIG_AUDIO, spk_a && a_modem);
}

// The receive path bit: B's while B has the speaker, else A's
static void rx_path(bool on)
{
	for (uint8_t byte = 2u; byte <= 5u; byte++) {
		bit(byte, BIT_RX_PATH, false);
	}
	if (on) {
		bit(spk_b ? rx_byte(b_dmr, VFO_B) : rx_byte(a_dmr, VFO_A), BIT_RX_PATH, true);
	}
}

void gd88_path_init(void)
{
	gd88_chain_init();
	hrc_pin_out(PIN_TX_RX, false);
	hrc_pin_out(PIN_DIG_AUDIO, false);
	hrc_pin_out(PIN_SPK_AMP, false);
	spk_a = spk_b = a_dmr = b_dmr = a_modem = receiving = bcast = spk_any = false;
}

void gd88_path_rx(uint32_t hz, bool on, bool dmr)
{
	uint32_t s = hrc_irq_save();	// A's band switch is the LO chip's GPIOs (radio_gd88.c); hz is for B
	want_on = on;
	want_hz = hz;
	a_dmr = dmr;
	receiving = on;
	rx_path(on);
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

// TX start: PTB2 high, the receive path bits off, the transmitter's route
void gd88_path_rx_front_off(void)
{
	hrc_pin_write(PIN_TX_RX, true);
	receiving = false;
	rx_path(false);
	bit(BYTE_TX_ROUTE, src_bit(a_dmr, VFO_A), true);
}

void gd88_path_tx_off(void)
{
	hrc_pin_write(PIN_TX_RX, false);
	for (uint8_t b = 0; b < 4u; b++) {
		bit(BYTE_TX_ROUTE, b, false);
	}
}

void gd88_path_speaker(bool on, bool from_modem)
{
	spk_a = on;
	a_modem = from_modem;
	outputs();
}

void gd88_audio_b(bool on, bool dmr)
{
	uint32_t s = hrc_irq_save();
	spk_b = on;
	b_dmr = dmr;
	outputs();
	rx_path(receiving);
	hrc_irq_restore(s);
}

void gd88_audio_bcast(bool on)
{
	uint32_t s = hrc_irq_save();
	bcast = on;
	outputs();
	hrc_irq_restore(s);
}
