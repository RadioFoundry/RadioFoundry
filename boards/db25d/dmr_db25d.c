// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D DMR: the HR_C7000's modem on the shared glue (platform/hrc7000/dmr_glue.c) with the main transceiver (BK4819
// or FD6818, rfchip_db25d.c) as the RF front end (docs/spec/hardware-db25d.md 4.5, 9).
//
// Transmit only with make DB25D_DMR_TX=1 (off by default until checked on air); without it tx() refuses in DMR mode
// and the glue's RF hooks do nothing. With it the stock firmware's scheme (4.5): the modem's 4FSK goes on the
// reference oscillator, and every burst runs the whole TX sequence of the analog transmitter, keyed from the modem's
// RF TX interrupt (3.1 ms ahead of the burst) and ended at its RF RX interrupt plus the page's TX-off delay, by the
// shared RF-burst task (platform/hrc7000/dmr_rfburst.c).
//
// Compiled only with CONFIG_DMR (off by default on this board).
#include "db25d.h"
#include "dmr_db25d.h"
#include "rfchip_db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"

#if CONFIG_DMR

#define MIC_GAIN_DB		DB25D_MIC_GAIN_DB
#define DMR_PA_DELAY_US		28000u	// the glue's PA one-shot; the PA is keyed by the burst sequence instead
#define C7_PHASE_OUT		0x128u	// RO: largest / smallest 14-bit AF phase seen (modem-hrc7000.md 2.1)

// The modem's receive mode word as the stock firmware builds it for this board (hardware-db25d.md 4.5): AF receive with
// averaging, AF gain 0x4C (4.75; the other HR_C7000 boards' word has 0x26), RX IQ inverted, dynamic sync threshold, dual receive
// channels, two-point TX. make DB25D_DMR_RF_MODE=0x... replaces it on the bench
#ifndef DB25D_DMR_RF_MODE
#define DB25D_DMR_RF_MODE	0x034C9860u
#endif
// The main chip's AF DAC gain in DMR (0x48 bits 3:0): the modem's input level, whatever the speaker volume. The stock
// digital set's value is 4. make DB25D_DMR_AF_GAIN=N replaces it
#ifndef DB25D_DMR_AF_GAIN
#define DB25D_DMR_AF_GAIN	4u
#endif

static hrc7000_t *modem;

// From the modem interrupts, per burst: the RF task's TX on or off (dmr_rfburst.c). Without DB25D_DMR_TX nothing is
// armed and there is nothing to switch
static void dmr_rf_switch(bool tx)
{
#if DB25D_DMR_TX
	hrc_rfburst_switch(tx);
#else
	(void)tx;
#endif
}

// The PA one-shot: nothing. The burst sequence keys the PA (stock: no PA timer on this board)
static void dmr_pa_on(void)
{
}

static const hrc_dmr_board_t dmr_board = {
	.rf_switch = dmr_rf_switch, .pa_on = dmr_pa_on, .pa_delay_us = DMR_PA_DELAY_US,
#if DB25D_DMR_TX
	.tx_end = db25d_dmr_tx_end,
#endif
};

// The speaker source for the modem's PCM (audio_pcm.c): the ring (prompt FIFO) on, or off. The modem's FM audio path
// isn't used on this board (the main chip's audio goes by its own route, 3.3), so it stays off. The modem's line out
// reaches the speaker amplifier with PTC18 low (radio_db25d.c speaker_pins())
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
	hrc_audio_pcm_ring(modem, on);	// held off while the DMR engine transmits (audio_pcm.c)
	hrc_irq_restore(s);
	db25d_speaker_pcm(on);	// the amplifier and its source: the squelch path never opens for DMR voice
}

#if DB25D_DMR_TX
static void dmr_tx_init(void);
#endif

void db25d_dmr_init(hrc7000_t *m)
{
	modem = m;	// attached and powered on by the radio init (modem-hrc7000.md 4 steps 4-32)
	hrc_dmr_board_init(modem, &dmr_board);
	hrc_audio_pcm_attach(modem, speaker_pcm);
#if DB25D_DMR_TX
	dmr_tx_init();
#endif
}

void db25d_dmr_enter(uint32_t rx_hz)
{
	db25d_main_idle();
	db25d_main_dmr_enter(DB25D_DMR_AF_GAIN);
	hrc7000_dmr_enter(modem, hrc7000_mod_bias_dc(db25d_cal_tx_center(), db25d_cal_mod(rx_hz)), MIC_GAIN_DB);
	hrc7000_wr(modem, C7_RF_MODE, DB25D_DMR_RF_MODE);	// the stock writes it once at start-up; it only acts in DMR
	hrc_dmr_enter();
}

void db25d_dmr_leave(void)
{
	hrc_dmr_leave();
	hrc7000_fm_idle(modem);
	db25d_main_idle();
	db25d_main_dmr_leave();
}

uint32_t db25d_dmr_phase(void)
{
	return hrc7000_rd(modem, C7_PHASE_OUT);
}

#if DB25D_DMR_TX
// ---- DMR transmit: the shared RF-burst task (platform/hrc7000/dmr_rfburst.c; hardware-db25d.md 4.5) ----

#define RF_MODE_TX_INVERT	(1u << 7)	// RF_MODE rf_transiq_mode: the symbol-to-deviation mapping inverted
// Stock: RF_MODE bit 7 clear on this board (the other HR_C7000 boards' value has it set). make DB25D_DMR_TX_INVERT=1 sets it, to
// try the other 4FSK polarity on the bench
#ifndef DB25D_DMR_TX_INVERT
#define DB25D_DMR_TX_INVERT	0
#endif

static const hrc_rfburst_board_t rf_board = { .on = db25d_burst_on, .off = db25d_burst_off };

static void dmr_tx_init(void)
{
	hrc_rfburst_init(modem, &rf_board);
}

void db25d_dmr_tx_arm(void)
{
	uint32_t m = hrc7000_rd(modem, C7_RF_MODE);
	hrc7000_wr(modem, C7_RF_MODE, DB25D_DMR_TX_INVERT ? (m | RF_MODE_TX_INVERT) : (m & ~RF_MODE_TX_INVERT));
	hrc_rfburst_arm();
}
#endif	// DB25D_DMR_TX

#endif	// CONFIG_DMR
