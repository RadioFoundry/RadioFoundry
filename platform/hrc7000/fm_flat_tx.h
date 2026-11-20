// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 flat FM transmit for the boards (fm_flat_tx.c, modem-hrc7000.md 8.6): the modem's TX RAM as the FM
// transmit source, each half pulled from a data modem (hal_audio_tap.h fm_tx_inject with AUDIO_TAP_FLAT) in the
// system interrupt. The board supplies its RF chip's part of the route and the microphone's
#ifndef HRC_FM_FLAT_TX_H
#define HRC_FM_FLAT_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_audio_tap.h"
#include "modem/hrc7000/hrc7000.h"

#if CONFIG_FM_FLAT
// Once, from the radio init, after the modem is attached
void hrc_fm_flat_tx_attach(hrc7000_t *modem);
// In FM transmit (task level): pulls both halves at once, in the caller's context, so the first half the modem plays
// is not empty, then turns the route on: the FM_BANDWIDTH bits in bw_clr cleared (HRC7000_FM_FLAT_TX_CLR or a
// subset), the TX RAM as the source, the FM TX interrupt on. From then on the interrupt pulls each half the modem asks
// for: the one FM_ADDR_SW names, or the other with fill_other (which one is right is unsettled, 8.6). False when
// running already, without the DMR glue's system interrupt (CONFIG_DMR off) or without a modem
bool hrc_fm_flat_tx_start(audio_tap_pull_t pull, uint32_t bw_clr, bool fill_other);
// The codec ADC as the source again, the bits set again, the interrupt off; nothing when not running. Task level
void hrc_fm_flat_tx_stop(void);
bool hrc_fm_flat_tx_active(void);
// Halves filled by the interrupt, and requests that named neither half (bench counters, since boot)
void hrc_fm_flat_tx_stats(uint32_t *halves, uint32_t *misses);
#else
static inline void hrc_fm_flat_tx_attach(hrc7000_t *modem) { (void)modem; }
#endif

#endif
