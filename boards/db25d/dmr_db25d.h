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
#include "rf/bk4819/bk4819.h"

// Once, from the radio init, with the main chip and the modem (already powered on by the radio init): the shared glue
// and the PCM audio
void db25d_dmr_init(bk4819_t *main_chip, hrc7000_t *modem);
// The main chip to its digital receive set and the modem into DMR; leave undoes both. The radio retunes afterwards
void db25d_dmr_enter(uint32_t rx_hz);
void db25d_dmr_leave(void);
void db25d_speaker_pcm(bool on);	// radio_db25d.c: the speaker on while the modem plays CPU samples
uint32_t db25d_dmr_phase(void);		// HR_C7000 PHASE_OUT: b29:16 / b13:0, the largest / smallest AF phase

#if DB25D_DMR_TX
// DMR transmit. Arm from tx() once the TX plan is set: the bursts' RF TX interrupts then key the RF. Disarm (task
// level) waits until the RF task has finished the burst it is in and left the RF receiving
void db25d_dmr_tx_arm(void);
void db25d_dmr_tx_disarm(void);
// The per-burst RF sequences (radio_db25d.c), from the RF task. wait() sleeps for us microseconds (short ones spin)
typedef void (*db25d_wait_fn)(uint32_t us);
void db25d_burst_on(db25d_wait_fn wait);
// elapsed_us after the burst's RF TX interrupt; now = no hold (an aborted transmission). Returns how late the ramp
// down started against the stock point, in us
uint32_t db25d_burst_off(db25d_wait_fn wait, uint32_t elapsed_us, bool now);
// For the diagnostics page
typedef struct {
	uint32_t bursts;	// TX-on sequences run
	uint32_t ends;		// TX-off sequences run
	uint32_t rfrx_ints;	// modem RF RX interrupts seen
	uint32_t late;		// TX-offs whose ramp down started after the stock point
	uint32_t last_late_us;
	char last_by;		// what woke the last TX-off: 'R' RF RX interrupt, 'T' the engine (slot tick), 'A' abort
} db25d_dmr_tx_stats_t;
void db25d_dmr_tx_stats(db25d_dmr_tx_stats_t *st);
#endif
#endif

#endif
