// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 flat FM transmit (fm_flat_tx.h, modem-hrc7000.md 8.6). With AUDIO_CONTROL b0 set the modulator plays two
// 256-sample halves of the TX RAM in turn (32 ms each at 8 kHz) and raises system interrupt bit 15 for the next. The
// DMR glue owns that interrupt and calls half_wanted for the bit, as it calls the FM receive hook for bit 14; it pulls
// the half straight from the data modem into the modem's RAM in 32-sample chunks (hrc7000_fm_tx_audio_fill), so no
// half-sized buffer is held in the CPU's RAM.
//
// What a sample costs in latency: the half being played (up to 32 ms) plus the one just filled (32 ms), so about 64 ms
// from a pull to the air, before whatever the modulator itself adds (unmeasured).
#include "config.h"
#if CONFIG_FM_FLAT
#include "fm_flat_tx.h"
#include "dmr_glue.h"
#include "platform.h"

static hrc7000_t *modem;
static audio_tap_pull_t volatile pull;	// non-NULL while the route is on: the interrupt's go-ahead
static volatile bool other;
static uint32_t bw_bits;
static volatile uint32_t halves, misses;

// One copy of the inline writer, in RAM: called from the interrupt and twice at the start, it would otherwise be
// outlined into flash
static HRC_RAMFUNC void fill(uint32_t half, audio_tap_pull_t p)
{
	hrc7000_fm_tx_audio_fill(modem, half, p);
}

// System interrupt, interrupts off. From RAM like its neighbors in the glue: flash busy time would delay the half
static HRC_RAMFUNC void half_wanted(void)
{
	audio_tap_pull_t p = pull;
	int h = hrc7000_fm_tx_audio_due(modem, other);
	if (p == NULL || h < 0) {
		misses++;
		return;
	}
	fill((uint32_t)h, p);
	halves++;
}

void hrc_fm_flat_tx_attach(hrc7000_t *m)
{
	modem = m;
}

bool hrc_fm_flat_tx_start(audio_tap_pull_t p, uint32_t bw_clr, bool fill_other)
{
	if (modem == NULL || p == NULL || pull != NULL) {
		return false;
	}
	other = fill_other;
	bw_bits = bw_clr;
	// Which half the modem plays first once the source switches is unknown: both are filled, so it is the first or
	// the second 32 ms of the transmission's opening (a data modem's preamble), never silence or old audio
	fill(0, p);
	fill(1, p);
	pull = p;
	hrc_barrier();
	if (!hrc_dmr_fm_tx_hook(half_wanted)) {
		pull = NULL;
		return false;
	}
	hrc7000_wr(modem, C7_SYS_INTERP_CLEAR, C7_SYS_FM_TX);	// a request left from before asks for nothing of ours
	hrc7000_fm_tx_audio_irq(modem, true);
	hrc7000_fm_flat_tx(modem, bw_bits, true);
	return true;
}

void hrc_fm_flat_tx_stop(void)
{
	if (pull == NULL) {
		return;
	}
	// The source first: the modulator then never plays a half nobody fills any more
	hrc7000_fm_flat_tx(modem, bw_bits, false);
	hrc7000_fm_tx_audio_irq(modem, false);
	hrc_dmr_fm_tx_hook(NULL);	// after this the interrupt no longer pulls
	pull = NULL;
}

bool hrc_fm_flat_tx_active(void)
{
	return pull != NULL;
}

void hrc_fm_flat_tx_stats(uint32_t *h, uint32_t *m)
{
	*h = halves;
	*m = misses;
}
#endif
