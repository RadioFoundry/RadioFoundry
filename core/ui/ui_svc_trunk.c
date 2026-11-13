// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's view of trunked channels (docs/spec/dmr-trunking.md 12.4): which system a channel uses, the system's name
// and whether it is set up. The radio core starts and leaves systems as channels are selected
#include <string.h>
#include "config.h"
#include "ui/ui_svc.h"
#include "radio/radio_trunk.h"

#if CONFIG_DMR_TRUNK
int ui_svc_t3_channel(uint16_t num, const char *name, bool *usable)
{
	radio_t3_tag_t tag;
	if (!radio_t3_tag(num, name, &tag)) {
		return -1;
	}
	if (usable != NULL) {
		*usable = radio_t3_usable(tag.sys);
	}
	return tag.sys;
}

bool ui_svc_t3_name(uint8_t n, char *name)
{
	return radio_t3_name(n, name);
}
#endif
