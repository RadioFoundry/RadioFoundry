// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 hardware options (hal_hwopt.h): the board revision, which nothing on the radio reports (docs/spec/
// hardware-md9600.md 16, 19.3), stored in the board's record sector; with CONFIG_MD9600_TX, the transmit arm for this
// power cycle (RAM only: every start is safe again)
#include <stddef.h>
#include "md9600.h"
#include "config.h"

// Choice i is revision i + 1 (md9600_rev_t)
static const char *const rev_choices[] = { "V1-V3", "V4", "V4A/V5" };

static int8_t rev_get(void)
{
	md9600_rev_t r = md9600_flash_rev_load();
	return r == MD9600_REV_UNKNOWN ? HWOPT_NONE : (int8_t)(r - 1);
}

static bool rev_set(uint8_t choice)
{
	if (choice >= MD9600_REV_COUNT - 1) {
		return false;
	}
	return md9600_flash_rev_store((md9600_rev_t)(choice + 1u));
}

#if CONFIG_MD9600_TX
static const char *const tx_choices[] = { "Off", "Armed" };

static int8_t tx_get(void)
{
	md9600_radio_diag_t d;
	md9600_radio_diag(&d);
	return d.tx_armed ? 1 : 0;
}

static bool tx_set(uint8_t choice)
{
	return choice < 2 && md9600_radio_tx_arm(choice == 1);
}
#endif

const hal_hwopt_t md9600_hwopts[MD9600_HWOPT_COUNT] = {
	{ "Board revision", sizeof(rev_choices) / sizeof(rev_choices[0]), rev_choices, rev_get, rev_set, true },
#if CONFIG_MD9600_TX
	{ "Transmit (bring-up)", 2, tx_choices, tx_get, tx_set, false },
#endif
};
