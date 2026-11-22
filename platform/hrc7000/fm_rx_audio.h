// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 FM receive audio for the APRS demodulator or a data modem's flat receive, one at a time (fm_rx_audio.c):
// the modem's own copy of its demodulated FM audio, 8 kHz, into a ring the radio task drains (hal_afsk.h receive side,
// hal_audio_tap.h fm_rx_flat_*, modem-hrc7000.md 8.5)
#ifndef HRC_FM_RX_AUDIO_H
#define HRC_FM_RX_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc7000/hrc7000.h"

#if CONFIG_APRS_RX || CONFIG_FM_FLAT
// Once, from the radio init, after the modem is attached
void hrc_fm_rx_attach(hrc7000_t *modem);
// Start and stop (task level). Start takes the ring from the RTOS heap and fails without it, while another consumer
// has it, or without the DMR glue's system interrupt (CONFIG_DMR off)
bool hrc_fm_rx_start(void);
void hrc_fm_rx_stop(void);
// Up to max samples since the last call (hal_afsk_t rx_read, hal_audio_tap_t fm_rx_flat_read)
uint16_t hrc_fm_rx_read(int16_t *buf, uint16_t max);
// Drop what the ring holds (task level): the next read starts with the half after this one. After a transmission or
// a retune, whose samples belong to no reception
void hrc_fm_rx_flush(void);
// Halves taken and halves dropped because the ring was full
void hrc_fm_rx_stats(uint32_t *halves, uint32_t *dropped);
#else
static inline void hrc_fm_rx_attach(hrc7000_t *modem) { (void)modem; }
#endif

#endif
