// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AM kind in the codeplug (docs/spec/airband-and-broadcast.md 3.4, channel-modes.md 2): a channel set to AM is
// its record in the safe stand-in shape plus a tag of kind AM with no parameter block. Kept apart from radio_am.c,
// which stays pure for the tests that link it without the codeplug
#include <stddef.h>
#include "config.h"
#if CONFIG_AM_RX
#include "radio/radio_am.h"
#include "codeplug/codeplug_tag.h"

bool radio_am_save(uint16_t number, const channel_t *ch)
{
	channel_t c = *ch;
	// Receive only, always: the tag never says transmit (flag bit 0 clear), so a reader that knows the tag but not
	// the kind's rule still never keys it. No system, no base-own flag: the record underneath is the stand-in every
	// reader that doesn't know tags takes as a receive-only DMR channel, and never transmits on
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_AM, .system = CODEPLUG_SYSTEM_NONE, .flags = 0u };

	radio_am_set(&c, true);
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	// A tag of the kind that followed the record keeps what a later firmware or an editor stored after its header
	// (rewrite with no block); else a new one
	return codeplug_set_channel(number, &c) && (codeplug_tag_rewrite(number, c.name, &t, NULL, 0u) ||
						     codeplug_tag_write(number, c.name, &t, NULL, 0u));
}
#endif
