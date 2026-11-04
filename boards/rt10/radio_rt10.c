// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 radio without an RF chip driver: receive accepts any in-band frequency and tunes nothing, transmit is refused,
// and the transmitter pins stay at their board_rf_safe levels
#include "rt10.h"
#include "pins.h"
#include "platform.h"

static void init(void)
{
	board_rf_safe();
}

static void set_mode(radio_mode_t mode)
{
	(void)mode;
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	(void)bw;
	(void)rx_tone;
	return board_in_band(hz, false);
}

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)hz;
	(void)bw;
	(void)power;
	(void)tx_tone;
	return false;
}

static void tx_off(void)
{
	board_rf_safe();
}

static void status(radio_status_t *st)
{
	st->rssi_dbm = -130;
	st->noise = 255;
	st->carrier = false;
	st->tone_ok = true;
}

static void audio(bool on)
{
	hrc_pin_write(PIN_AMP_EN, on);
}

const hal_radio_t rt10_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
};
