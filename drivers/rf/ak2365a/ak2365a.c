// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AK2365A IF receiver set-up (ak2365a.h)
#include <stddef.h>
#include "ak2365a.h"

uint8_t ak2365a_setup_words(const ak2365a_cfg_t *c, bool wide, uint16_t out[AK2365A_SETUP_WORDS])
{
	uint8_t i = 0;
	out[i++] = ak2365a_word(AK2365A_REG_SRST, AK2365A_SRST_VALUE);
	out[i++] = ak2365a_word(AK2365A_REG_CTRL1, wide ? c->ctrl1_wide : c->ctrl1_narrow);
	out[i++] = ak2365a_word(AK2365A_REG_CTRL2, c->ctrl2);
	out[i++] = ak2365a_word(AK2365A_REG_CTRL3, c->ctrl3);
	out[i++] = ak2365a_word(AK2365A_REG_CTRL4, c->ctrl4);
	out[i++] = ak2365a_word(AK2365A_REG_CTRL5, c->ctrl5);
	return i;
}

bool ak2365a_setup(const wire3_bus_t *bus, uint8_t cs, const ak2365a_cfg_t *c, bool wide)
{
	uint16_t w[AK2365A_SETUP_WORDS];
	uint8_t n = ak2365a_setup_words(c, wide, w);
	bool ok = true;
	for (uint8_t i = 0; i < n; i++) {
		ok &= bus->write(bus->ctx, cs, w[i], AK2365A_WORD_BITS);
	}
	return ok;
}
