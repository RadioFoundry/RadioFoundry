// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV APRS AFSK (hal_afsk.h, docs/spec/aprs.md 5): receive only. The HR_C7000 copies its demodulated FM audio to
// the CPU (platform fm_rx_audio.c); while APRS listens the FD6818's audio stays on the HR_C7000's input and the
// speaker plays the HR_C7000's FM output instead (radio_dm32.c, hardware-dm32.md 9). No transmit path yet: tx_start
// is NULL, so the core sends no packets on RF from this radio.
#include "config.h"
#if CONFIG_APRS_RX
#include "dm32.h"
#include "fm_rx_audio.h"

static bool rx_start(void)
{
	dm32_aprs_listen(true);
	if (!hrc_fm_rx_start()) {
		dm32_aprs_listen(false);
		return false;
	}
	return true;
}

static void rx_stop(void)
{
	hrc_fm_rx_stop();
	dm32_aprs_listen(false);
}

const hal_afsk_t dm32_afsk = {
	.rx_rate_hz = HRC7000_FM_RX_RATE,
	.rx_start = rx_start,
	.rx_read = hrc_fm_rx_read,
	.rx_stop = rx_stop,
};
#endif
