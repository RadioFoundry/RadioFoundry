// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV debug link view keys (the `board` section, docs/spec/debuglink.md 3.7; key list in
// docs/spec/hardware-dm32.md 9): what a HIL test needs to see of the speaker path
#include "config.h"
#if CONFIG_DEBUGLINK	// the whole file
#include "dm32.h"
#include "debuglink/dl_fmt.h"

bool dm32_view_line(uint8_t i, char *buf, uint8_t len)
{
	switch (i) {
	case 0:		// who keeps the speaker amplifier on (dm32_amp_users): 0 while nothing plays
		dl_fmt(buf, len, "amp_users=0x%02x", (unsigned)dm32_amp_users());
		return true;
	default:
		return false;
	}
}

#endif
