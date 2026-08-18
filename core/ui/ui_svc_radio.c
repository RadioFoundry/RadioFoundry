// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls the radio core already answers (docs/spec/dmr-voice.md): the talk target for now. These are
// the strong definitions; ui_svc_stub.c keeps the weak ones for what other packages still have to provide.
#include "ui_svc.h"
#include "radio/radio.h"
#include "config.h"

uint32_t ui_svc_feat_voice(void)
{
	return CONFIG_DMR ? UI_FEAT_TALK_TARGET : 0;
}

#if CONFIG_DMR
bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life)
{
	switch (life) {
	case UI_TALK_TEMPORARY:
		radio_req_dmr_target(id, private_call);
		return true;
	case UI_TALK_DEFAULT:
		radio_req_dmr_target_clear();
		return true;
	default:
		return false;	// permanent: writing the channel's contact needs the codeplug writer (storage-cps)
	}
}
#endif
