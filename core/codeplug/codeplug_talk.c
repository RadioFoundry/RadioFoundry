// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A channel's default talk target (codeplug.h, docs/spec/dmr-voice.md 2). Only codeplug_contact() and
// codeplug_listen_list() underneath, so the host tests' codeplug fakes serve it too
#include "config.h"
#include "codeplug/codeplug.h"

#if CONFIG_DMR

uint16_t codeplug_channel_talk_contact(const channel_t *ch)
{
	if (ch->contact != 0) {
		return ch->contact;
	}
	// No TX contact: the first talkgroup of the RX group list. qdmr's open-firmware encoder writes a channel's TX
	// contact or its group list, never both
	listen_list_t list;		// on the stack: the radio and UI tasks both call this
	contact_t c;
	if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &list)) {
		for (uint8_t i = 0; i < list.count; i++) {
			if (codeplug_contact(list.contacts[i], &c) && c.type != CONTACT_PRIVATE) {
				return list.contacts[i];
			}
		}
	}
	return 0;
}

#endif
