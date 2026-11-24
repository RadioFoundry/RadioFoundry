// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Key fill's transport (CONFIG_KEY_FILL, docs/spec/key-fill.md 3): its messages as UDP datagrams on KF_PORT through the
 * air queue, and the one received message held for the UI task, which runs the engine (keyfill/keyfill.h).
 *
 *   receiving  key_fill_open(true) while a session runs: a datagram to KF_PORT for our own ID is held, one at a time
 *              (another arriving before key_fill_take is dropped: the sender's confirmed data repeats it). Closed,
 *              every one is dropped
 *   sending    key_fill_send: one message at a time, a confirmed packet to the other radio; key_fill_send_state()
 *              says when it is on air or failed
 */
#ifndef RFOUNDRY_KEYFILL_API_H
#define RFOUNDRY_KEYFILL_API_H

#include <stdbool.h>
#include <stdint.h>
#include "keyfill/keyfill.h"

#define KF_SEND_WAIT_MS		10000	// a message the data service doesn't take by then fails (3)

typedef enum { KF_SEND_IDLE, KF_SEND_SENDING, KF_SEND_SENT, KF_SEND_FAILED } kf_send_state_t;

typedef struct {
	uint32_t rx;		// datagrams to KF_PORT for us
	uint32_t rx_closed;	// ... dropped: no session
	uint32_t rx_busy;	// ... dropped: the one held not yet taken
	uint32_t sent, send_failed;
} kf_svc_stats_t;

// The radio task: once
void key_fill_svc_init(void);
// Any task
void key_fill_open(bool open);
// The held message: its length (0: none) copied to out, its sender in *src; the slot is free again
int key_fill_take(uint8_t out[KF_MSG_MAX], uint32_t *src);
// len octets to dst, confirmed. False while the last one is still going, or the air queue has no room
bool key_fill_send(uint32_t dst, const uint8_t *msg, int len);
kf_send_state_t key_fill_send_state(void);
void key_fill_get_stats(kf_svc_stats_t *out);

#endif
