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
// What an AES channel does with packet data (docs/spec/dmr-data.md 13): false, out untouched, off AES channels. AES
// has no data form yet, so packets are held back (DMR_DATA_PRIVACY_BLOCK), never sent in the clear; signaling still goes
// monitor: digital monitor on, which takes clear packets on an encrypted-only channel too
struct dmrDataPrivacy_s;
bool radio_aes_data_config(const channel_t *ch, bool monitor, struct dmrDataPrivacy_s *out);
// Whether the channel ignores calls and packet data in the clear (an AES channel set encrypted only), monitor off
bool radio_aes_enc_only(const channel_t *ch, bool monitor);
// Call between overs only, including while tuned to another mode.
void radio_aes_poll(const channel_t *ch, uint32_t own_id, uint32_t now_ms);

#endif
