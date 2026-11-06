// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D DMR (dmr_db25d.c, CONFIG_DMR; docs/spec/hardware-db25d.md 4.5): what the board files share
#ifndef DB25D_DMR_H
#define DB25D_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc7000/hrc7000.h"

#define DB25D_MIC_GAIN_DB	11u	// GCR_ADCL at init, as the other HR_C7000 boards. The stock digital channel uses a setting, 0 dB by default

// DMR transmit (make DB25D_DMR_TX=1, with CONFIG_DMR; hardware-db25d.md 4.5): off until it has been checked on air
#ifndef DB25D_DMR_TX
#define DB25D_DMR_TX		0
#endif

#if CONFIG_DMR
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "vocoder.h"

// Once, from the radio init, with the modem (already powered on by the radio init): the shared glue and the PCM audio
void db25d_dmr_init(hrc7000_t *modem);
// The main chip to its digital receive set and the modem into DMR; leave undoes both. The radio retunes afterwards
void db25d_dmr_enter(uint32_t rx_hz);
void db25d_dmr_leave(void);
void db25d_speaker_pcm(bool on);	// radio_db25d.c: the speaker on while the modem plays CPU samples
uint32_t db25d_dmr_phase(void);		// HR_C7000 PHASE_OUT: b29:16 / b13:0, the largest / smallest AF phase

#if DB25D_DMR_TX
#include "dmr_rfburst.h"
// DMR transmit (dmr_rfburst.h). Arm from tx() once the TX plan is set: RF_MODE's TX polarity, then hrc_rfburst_arm.
// Disarm with hrc_rfburst_disarm, the stats with hrc_rfburst_stats
void db25d_dmr_tx_arm(void);
// The per-burst RF sequences (radio_db25d.c), from the RF task (hrc_rfburst_board_t)
void db25d_burst_on(hrc_rfburst_wait_fn wait);
uint32_t db25d_burst_off(hrc_rfburst_wait_fn wait, uint32_t elapsed_us, bool now);
#endif
#endif

#endif
