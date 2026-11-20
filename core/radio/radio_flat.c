// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flat FM paths' calibration (radio_flat.h)
#include <string.h>
#include "radio/radio_flat.h"

#if CONFIG_FM_FLAT
#include "settings/settings.h"

void radio_flat_cal(const hal_audio_tap_t *tap, audio_tap_flat_cal_t *out)
{
	memset(out, 0, sizeof(*out));
	if (tap != NULL && tap->flat_cal != NULL && tap->flat_cal() != NULL) {
		*out = *tap->flat_cal();
	}
	settings_t *s = settings_begin();
	int16_t tx_1khz = s->flat_tx_1khz;
	uint8_t flags = s->flat_flags;
	uint16_t tx_hpf = s->flat_tx_hpf, rx_hpf = s->flat_rx_hpf;
	settings_end(0);
	if (tx_1khz != 0) {
		out->tx_1khz = tx_1khz;
	}
	if (flags & SETTINGS_FLAT_TX_INV_SET) {
		out->tx_invert = (flags & SETTINGS_FLAT_TX_INV) != 0;
	}
	if (flags & SETTINGS_FLAT_RX_INV_SET) {
		out->rx_invert = (flags & SETTINGS_FLAT_RX_INV) != 0;
	}
	if (tx_hpf != 0) {
		out->tx_hpf_hz = (uint16_t)(tx_hpf - 1u);
	}
	if (rx_hpf != 0) {
		out->rx_hpf_hz = (uint16_t)(rx_hpf - 1u);
	}
}
#endif
