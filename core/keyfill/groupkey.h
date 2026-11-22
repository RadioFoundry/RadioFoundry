// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Group key fill's message (docs/spec/group-key-fill.md 4): an AES-256 key wrapped under keys derived from the channel's
 * AES key, with a tag. Pure: no RTOS, no allocation; every secret on the stack is wiped before return.
 *
 *   sender     gk_build: the message for one key, from the channel key, a random session and the sender's ID
 *   receiver   gk_open: the message checked (format, sender, tag) and the key taken out, or why not
 */
#ifndef RFOUNDRY_GROUPKEY_H
#define RFOUNDRY_GROUPKEY_H

#include <stdbool.h>
#include <stdint.h>

#define GK_PORT			4102	// UDP source and destination port (3)
#define GK_MSG_LEN		65	// the message (4)
#define GK_SESSION_LEN		8
#define GK_KEY_LEN		32	// AES-256
#define GK_ALG_AES256		0x25
#define GK_COPIES		4	// copies a send puts on air (3)
#define GK_COPY_GAP_MS		3000u
#define GK_LISTEN_MS		(30u * 60u * 1000u)	// the receive window (5)
#define GK_SEND_WAIT_MS		10000	// a copy the data service doesn't take by then fails

// What gk_open found
typedef enum {
	GK_OK,
	GK_BAD_FORMAT,		// length, magic, version, algorithm or key length
	GK_BAD_SENDER,		// the sender's ID in the message isn't the packet's source
	GK_BAD_TAG,		// another channel key, or altered
} gk_result_t;

typedef struct {
	uint8_t session[GK_SESSION_LEN];
	uint32_t sender;
	uint8_t alg;
	uint8_t key_id;
	uint8_t key[GK_KEY_LEN];	// the key in the clear: wipe it (gk_wipe) when done
} gk_item_t;

// The message for key `key` (ID key_id) sent by `sender` under channel key chan_key: GK_MSG_LEN octets into out
void gk_build(uint8_t out[GK_MSG_LEN], const uint8_t chan_key[GK_KEY_LEN], const uint8_t session[GK_SESSION_LEN],
	      uint32_t sender, uint8_t key_id, const uint8_t key[GK_KEY_LEN]);
// msg (len octets) received from packet source src, checked under chan_key; GK_OK fills *out (cleared otherwise)
gk_result_t gk_open(const uint8_t *msg, int len, uint32_t src, const uint8_t chan_key[GK_KEY_LEN], gk_item_t *out);
void gk_wipe(gk_item_t *it);

#endif
