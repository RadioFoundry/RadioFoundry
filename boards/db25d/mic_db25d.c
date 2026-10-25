// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D hand mic keys (docs/spec/hardware-db25d.md 3.4): the mic's key frames arrive on the host UART (PTA9 is both
// the mic's data line and the PC link's RXD). Its receive interrupt hands every byte to the shared decoder
// (drivers/input/serialmic), which tells them apart from the PC's traffic.
#include "db25d.h"
#include "config.h"
#include "platform.h"
#include "input/serialmic/serialmic.h"

static serialmic_t mic = { .cand = SERIALMIC_NONE, .held = SERIALMIC_NONE };

void hrc_host_uart_rx_tap(uint8_t b, bool error)
{
	uint32_t now = hrc_micros();
	serialmic_byte(&mic, b, error, now);
#if CONFIG_DIAG
	db25d_miccap_byte(b, error, now);
#endif
}

// The mic's key number held now, or 0xFF
uint8_t db25d_mic_key(void)
{
	uint32_t s = hrc_irq_save();
	uint8_t code = serialmic_key(&mic, hrc_micros());
	hrc_irq_restore(s);
	return code;
}
