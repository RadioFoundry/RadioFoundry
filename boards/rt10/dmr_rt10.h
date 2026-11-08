// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 DMR (dmr_rt10.c, CONFIG_DMR; docs/spec/hardware-rt10.md 7.1): what the board files share
#ifndef RT10_DMR_H
#define RT10_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// The modem's ADC gain in DMR (GCR_ADCL, dB): above the original firmware's 11 dB, as listened to through a parrot.
// make RT10_DMR_MIC_DB=N replaces it on the bench
#ifndef RT10_DMR_MIC_DB
#define RT10_DMR_MIC_DB		23u
#endif
// DAC C in DMR: the bias of the modem's AF ADC (its N input), which sets where the FD6818's 4FSK sits in the ADC's
// range. 1850 gave the fewest AMBE bits corrected (hardware-rt10.md 7.1); the original firmware's 1762 corrects about
// one per frame. make RT10_DMR_DAC_C=N replaces it on the bench
#ifndef RT10_DMR_DAC_C
#define RT10_DMR_DAC_C		1850u
#endif
#if CONFIG_DMR
#include "modem/hrc7000/hrc7000.h"
#include "dmr_rfburst.h"
// Once, from the radio init, with the modem attached and powered on: the shared glue, the PCM audio, the RF task
void rt10_dmr_init(hrc7000_t *modem);
// The modem into DMR with the bias for the receive frequency, and back to FM. The FD6818's set is the radio's
void rt10_dmr_enter(uint32_t mod_bias);
void rt10_dmr_leave(void);
// The modem's PHASE_OUT: b29:16 / b13:0, the largest / smallest AF phase it saw (modem-hrc7000.md 2.1)
uint32_t rt10_dmr_phase(void);
void rt10_speaker_pcm(bool on);	// radio_rt10.c: the amplifier on while the modem plays CPU samples
// The per-burst RF sequences (radio_rt10.c), from the RF task (hrc_rfburst_board_t)
void rt10_burst_on(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait);
void rt10_burst_off(void);
void rt10_dmr_tx_end(void);	// radio_rt10.c: hrc_dmr_board_t.tx_end
#endif

#endif
