// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 DMR, either SoC: the HR_C7000's modem on the shared glue (platform/hrc7000/dmr_glue.c) with the main BK4819
// as the RF front end (docs/spec/hardware-gd88.md 11), the scheme of hardware-db25d.md 4.
//
// Transmit only with GD88_DMR_TX=1 (gd88_rf.h: A with GD88_TX, B as A's second transmitter); without it tx() refuses
// in DMR mode and the glue's RF hooks do nothing.
// With it the modem's 4FSK goes on the reference oscillator, and every burst runs the whole TX sequence of the analog
// transmitter on the shared RF-burst task (platform/hrc7000/dmr_rfburst.c), keyed from the modem's RF TX interrupt
// (3.1 ms ahead of the burst) and ended at its RF RX interrupt plus the page's TX-off delay.
#include "gd88_rf.h"
#include "config.h"
#include "platform.h"

#if CONFIG_DMR
#define MIC_GAIN_DB		GD88_MIC_GAIN_DB
#define C7_PHASE_OUT		0x128u	// RO: largest / smallest 14-bit AF phase seen (modem-hrc7000.md 2.1)

// The modem's receive mode word of hardware-db25d.md 4.5 (the same modem library and RF scheme): AF receive with
// averaging, AF gain 0x4C, RX IQ inverted (high-side LO: gd88_dmr_lo_side), dynamic sync threshold, dual receive
// channels, two-point TX.
// GD88_DMR_RF_MODE=0x... replaces it on the bench. Receive works with it on the radio (voice and data decode).
// TODO(gd88): unknown, needs a GD88_TX build: its transmit half on the GD-88
#ifndef GD88_DMR_RF_MODE
#define GD88_DMR_RF_MODE	0x034C9860u
#endif
// The main chip's AF DAC gain in DMR (0x48 bits 3:0): the modem's input level, whatever the speaker volume. The digital
// set's value is 4. make GD88_DMR_AF_GAIN=N replaces it
#ifndef GD88_DMR_AF_GAIN
#define GD88_DMR_AF_GAIN	4u
#endif

static hrc7000_t *modem;
static bk4819_t *rf;

#if GD88_DMR_TX
static const hrc_rfburst_board_t rf_board = {
	.on = gd88_burst_on, .off = gd88_burst_off, .off_ahead_us = GD88_DMR_OFF_AHEAD_US,
};
#endif

static const hrc_dmr_board_t dmr_board = {
#if GD88_DMR_TX
	.burst = &rf_board, .tx_end = gd88_dmr_tx_end,
#endif
};

// The speaker source for the modem's PCM (audio_pcm.c): the ring (prompt FIFO) on, or off. The modem's FM audio path
// isn't used on this board (the main chip's audio goes by its own route, 3.3), so it stays off. The modem's line out
// reaches the speaker amplifier with PTC18 low (radio_gd88.c speaker_pins())
static void speaker_pcm(bool on)
{
	// beep.c repeats the call every block: rewriting AUDIO_CONTROL 50 times a second made its tones scratchy
	static int8_t last = -1;
	if (last == (int8_t)on) {
		return;
	}
	last = (int8_t)on;
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	hrc7000_audio_bits(modem, 0, C7_AUDIO_FM_PLAY);
	// Held off while the DMR engine transmits (audio_pcm.c). Set here directly, the gate's reopening applied its own
	// (never set) state: ring play off with the amplifier on, a hum after a DMR over
	hrc_audio_pcm_ring(modem, on);
	hrc_irq_restore(s);
	gd88_speaker_pcm(on);	// the amplifier and its source: the squelch path never opens for DMR voice
}

void gd88_dmr_init(bk4819_t *main_chip, hrc7000_t *m)
{
	rf = main_chip;
	modem = m;	// attached and powered on by the radio init (modem-hrc7000.md 4 steps 4-32)
	hrc_dmr_board_init(modem, &dmr_board);
	hrc_audio_pcm_attach(modem, speaker_pcm);
}

void gd88_dmr_enter(uint32_t rx_hz)
{
	bk4819_set_state(rf, BK4819_STATE_IDLE);
	bk4819_dmr_rx_enter(rf);
	bk4819_set_af_gain(rf, GD88_DMR_AF_GAIN);
	hrc7000_dmr_enter(modem, hrc7000_mod_bias_dc(gd88_cal_tx_center(), gd88_cal_mod(rx_hz)), MIC_GAIN_DB);
	hrc7000_wr(modem, C7_RF_MODE, GD88_DMR_RF_MODE);	// it only acts in DMR
	hrc_dmr_enter();
}

#define RF_MODE_RX_INVERT	(1u << 15)

// The word's bit 15 is for a high-side LO. An AT1141 LO runs low-side from 400 to 440 MHz, which turns the spectrum
// the other way round: the original firmware keeps a side flag per channel
void gd88_dmr_lo_side(bool high)
{
	uint32_t m = hrc7000_rd(modem, C7_RF_MODE);
	hrc7000_wr(modem, C7_RF_MODE, high ? (m | RF_MODE_RX_INVERT) : (m & ~RF_MODE_RX_INVERT));
}

void gd88_dmr_leave(void)
{
	hrc_dmr_leave();
	hrc7000_fm_idle(modem);
	bk4819_set_state(rf, BK4819_STATE_IDLE);
	bk4819_dmr_rx_end(rf);
}

uint32_t gd88_dmr_phase(void)
{
	return hrc7000_rd(modem, C7_PHASE_OUT);
}

bool gd88_dmr_ring_play(void)
{
	return modem != NULL && (modem->audio_ctl & C7_AUDIO_RING_PLAY) != 0;
}

#if GD88_DMR_TX
#define RF_MODE_TX_INVERT	(1u << 7)	// RF_MODE rf_transiq_mode: the symbol-to-deviation mapping inverted
// RF_MODE bit 7 clear, as in hardware-db25d.md (hardware-dm32.md's value has it set). GD88_DMR_TX_INVERT=1 sets it,
// to try the other 4FSK polarity on the bench
#ifndef GD88_DMR_TX_INVERT
#define GD88_DMR_TX_INVERT	0
#endif

void gd88_dmr_tx_arm(void)
{
	uint32_t m = hrc7000_rd(modem, C7_RF_MODE);
	hrc7000_wr(modem, C7_RF_MODE, GD88_DMR_TX_INVERT ? (m | RF_MODE_TX_INVERT) : (m & ~RF_MODE_TX_INVERT));
	hrc_rfburst_arm();
}
#endif	// GD88_DMR_TX

#endif	// CONFIG_DMR
