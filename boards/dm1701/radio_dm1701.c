// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 radio (AT1846S + HR_C6000 + RF path + calibration): STUB, to be replaced
#include "dm1701.h"

static void init(void) {}
static void set_mode(radio_mode_t mode) { (void)mode; }
static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *t) { (void)hz; (void)bw; (void)t; return true; }
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t) { (void)hz; (void)bw; (void)p; (void)t; return false; }
static void tx_off(void) {}
static void status(radio_status_t *st) { st->rssi_dbm = -130; st->noise = 255; st->carrier = false; st->tone_ok = true; }
static void audio(bool on) { (void)on; }

const hal_radio_t dm1701_radio = { .init = init, .set_mode = set_mode, .rx = rx, .tx = tx, .tx_off = tx_off, .status = status, .audio = audio };
