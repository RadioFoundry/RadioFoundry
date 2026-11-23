// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Rooms (docs/spec/ysf.md 9): the commands a radio sends the node it hears (a repeater or a gateway linked to a
// room network), as a short data transmission, and the node's replies. Bytes in, values out: the frames are the
// call layer's (ysf.h), the keying the radio core's. No RTOS or board call.
#ifndef RFOUNDRY_YSF_WX_H
#define RFOUNDRY_YSF_WX_H

#include <stdbool.h>
#include <stdint.h>

// Commands
enum { YSF_WX_DX, YSF_WX_CONNECT, YSF_WX_DISCONNECT, YSF_WX_ALL, YSF_WX_SEARCH, YSF_WX_NONE = 0xFF };
#define YSF_WX_CMD_MAX		40	// a command's bytes, at most
#define YSF_WX_TEXT_LEN		16	// a search's text
#define YSF_WX_ROOMS		20	// rooms on one page of a list
#define YSF_WX_ROOM_ID_MAX	99999u

// cmd's bytes for arg (CONNECT: the room ID; ALL: the first entry, 1-based; SEARCH: text) into out; their count, 0
// for no such command
uint8_t ysf_wx_command(uint8_t cmd, uint32_t arg, const char *text, uint8_t out[YSF_WX_CMD_MAX]);

typedef struct {
	char id[6];		// 5 digits
	char name[17];
	uint8_t users;
} ysf_wx_room_t;

// What the node said last
typedef struct {
	uint32_t serial;		// moves at every reply taken
	uint8_t kind;			// YSF_WX_*: what it answered (DX, CONNECT, DISCONNECT, ALL)
	bool connected;			// the node is in a room
	char node_id[6], node_call[11], node_name[15];
	char room_id[6], room_name[17];	// "" when not in a room
	bool search;			// the list is a search's result
	uint8_t rooms;			// rooms on this page
	uint16_t rooms_total;		// rooms the node knows (a list)
	ysf_wx_room_t room[YSF_WX_ROOMS];
} ysf_wx_t;

// A reply being received: the data of a data transmission's frames, in order, parsed as it comes (no buffer for the
// whole reply: a room list is over 1 KB). The caller's memory
typedef struct {
	bool on;			// a data transmission is being received
	bool broken;			// a frame was lost: this reply is dropped
	uint8_t bn, blk;		// the last block number heard, and the blocks counted (BN has two bits)
	uint16_t pos;			// bytes taken so far
	uint8_t sum, prev;		// their byte sum, the last one
	uint16_t end;			// where a checked end marker was, + 1; 0 none
	uint8_t hdr[64];		// the reply's first bytes
	uint8_t rec[50];		// the room entry being filled
	ysf_wx_t out;			// what it is being parsed into; it keeps the last reply taken, the room list too
} ysf_wx_rx_t;

void ysf_wx_rx_begin(ysf_wx_rx_t *r);
// A frame of a data transmission: its FN, BN and FT and the two 20-byte channels (ok bit 0: the first decoded, bit 1
// the second). True when the reply is whole (FN = FT of a block that isn't full)
bool ysf_wx_rx_frame(ysf_wx_rx_t *r, uint8_t fn, uint8_t bn, uint8_t ft, const uint8_t c1[20], const uint8_t c2[20],
		     uint8_t ok);
// The reply is over (a terminator, or rx_frame said so): true, and r->out filled (its serial not moved), when it
// parsed as a node's reply
bool ysf_wx_rx_end(ysf_wx_rx_t *r);

#endif
