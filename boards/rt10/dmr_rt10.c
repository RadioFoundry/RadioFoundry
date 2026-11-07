// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 DMR: the HR_C7000's modem on the shared glue (platform/hrc7000/dmr_glue.c) with the FD6818 as the RF front end
// (docs/spec/hardware-rt10.md 7.1). The modem's 4FSK goes on the reference oscillator (its MOD1 output is the TCXO's
// control voltage, 4.1), and every burst runs the whole RF key-up and key-down of the analog transmitter, keyed from
// the modem's RF TX interrupt by the shared RF-burst task (platform/hrc7000/dmr_rfburst.c); the sequences are
// rt10_burst_on / rt10_burst_off in radio_rt10.c.
//
// Compiled only with CONFIG_DMR.
#include "rt10.h"
#include "dmr_rt10.h"
#include "config.h"
#include "platform.h"

#if CONFIG_DMR
#include "dmr_glue.h"
#include "dmr_rfburst.h"
#include "audio_pcm.h"

#define C7_PHASE_OUT		0x128u		// RO: largest / smallest 14-bit AF phase seen (modem-hrc7000.md 2.1)
#define DMR_PA_DELAY_US		28000u	// the glue's PA one-shot; the PA is keyed by the burst sequence instead

// The modem's receive mode word, as the stock firmware writes it on this radio (hardware-rt10.md 7.1), with the TX
// symbol mapping not inverted (bit 7 clear). make RT10_DMR_RF_MODE=0x... replaces it on the bench
#ifndef RT10_DMR_RF_MODE
#define RT10_DMR_RF_MODE	0x034C9860u
#endif

static hrc7000_t *modem;

// From the modem interrupts, per burst: the RF task's TX on or off
static void dmr_rf_switch(bool tx)
{
	hrc_rfburst_switch(tx);
}

// The PA one-shot: nothing. The burst sequence keys the PA
static void dmr_pa_on(void)
{
}

static const hrc_dmr_board_t dmr_board = {
	.rf_switch = dmr_rf_switch, .pa_on = dmr_pa_on, .pa_delay_us = DMR_PA_DELAY_US,
};

static const hrc_rfburst_board_t rf_board = { .on = rt10_burst_on, .off = rt10_burst_off };

// The speaker source for the modem's PCM (audio_pcm.c): the ring (prompt FIFO) on, or off. The modem's FM audio path
// isn't used on this radio (the FD6818 demodulates FM), so it stays off
static void speaker_pcm(bool on)
{
	// beep.c repeats the call every block: write only the changes
	static int8_t last = -1;
	if (last == (int8_t)on) {
		return;
	}
	last = (int8_t)on;
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	hrc7000_audio_bits(modem, 0, C7_AUDIO_FM_PLAY);
	hrc_audio_pcm_ring(modem, on);	// held off while the DMR engine transmits (audio_pcm.c)
	hrc_irq_restore(s);
	rt10_speaker_pcm(on);
}

void rt10_dmr_init(hrc7000_t *m)
{
	modem = m;	// attached and powered on by the radio init
	hrc_dmr_board_init(modem, &dmr_board);
	hrc_audio_pcm_attach(modem, speaker_pcm);
	hrc_rfburst_init(modem, &rf_board);
}

void rt10_dmr_enter(uint32_t mod_bias)
{
	hrc7000_dmr_enter(modem, mod_bias, RT10_DMR_MIC_DB);
	hrc7000_wr(modem, C7_RF_MODE, RT10_DMR_RF_MODE);
	hrc_dmr_enter();
}

void rt10_dmr_leave(void)
{
	hrc_dmr_leave();
	hrc7000_fm_idle(modem);
}

uint32_t rt10_dmr_phase(void)
{
	return hrc7000_rd(modem, C7_PHASE_OUT);
}
#endif	// CONFIG_DMR
