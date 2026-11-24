// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RFOUNDRY_RADIO_AES_H
#define RFOUNDRY_RADIO_AES_H

#include "codeplug/codeplug.h"
#include "dmr/call/dmr_call.h"

// Radio-task-only integration for conventional DMR AES (docs/spec/dmr-voice.md 5.1).
void radio_aes_start(uint32_t saved);
uint8_t radio_aes_tx_deny(const channel_t *ch);
bool radio_aes_configure(const channel_t *ch, dmr_call_config_t *cfg);
// Call between overs only, including while tuned to another mode.
void radio_aes_poll(const channel_t *ch, uint32_t own_id, uint32_t now_ms);

#endif
