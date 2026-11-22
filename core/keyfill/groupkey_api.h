// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Group key fill's transport (CONFIG_GROUP_KEY_FILL, docs/spec/group-key-fill.md 3): one message as a UDP datagram on
 * GK_PORT to a talkgroup through the air queue, unconfirmed, and the one received message held for the UI task.
 *
 *   receiving  group_key_open(true) while Receive group key is open: a datagram to GK_PORT for a group is held, one at
 *              a time (another arriving before group_key_take is dropped: the next copy brings it again). Closed,
 *              every one is dropped
 *   sending    group_key_send: one copy at a time; group_key_send_state() says when it is on air or failed. The UI
 *              paces the copies (GK_COPIES, GK_COPY_GAP_MS)
 */
#ifndef RFOUNDRY_GROUPKEY_API_H
#define RFOUNDRY_GROUPKEY_API_H

#include <stdbool.h>
#include <stdint.h>
#include "keyfill/groupkey.h"

typedef enum { GK_SEND_IDLE, GK_SEND_SENDING, GK_SEND_SENT, GK_SEND_FAILED } gk_send_state_t;

typedef struct {
	uint32_t rx;		// datagrams to GK_PORT for a group
	uint32_t rx_closed;	// ... dropped: not listening
	uint32_t rx_bad;	// ... dropped: not a message's length
	uint32_t rx_busy;	// ... dropped: the one held not yet taken
	uint32_t sent, send_failed;
} gk_svc_stats_t;

// The radio task: once
void group_key_svc_init(void);
// Any task
void group_key_open(bool open);
// The held message: its length (0: none) copied to out, its sender in *src; the slot is free again
int group_key_take(uint8_t out[GK_MSG_LEN], uint32_t *src);
// One copy of msg to talkgroup tg. False while the last one is still going, or the air queue has no room
bool group_key_send(uint32_t tg, const uint8_t msg[GK_MSG_LEN]);
gk_send_state_t group_key_send_state(void);
// Drops a copy still waiting, and the state goes back to idle
void group_key_cancel(void);
void group_key_get_stats(gk_svc_stats_t *out);

#endif
