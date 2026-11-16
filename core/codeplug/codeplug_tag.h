// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Channel tags (docs/spec/channel-modes.md 2): a key in the codeplug settings area that makes a codeplug channel one
// of another kind (a digital mode the open-firmware record can't say). Built in every configuration, so a channel of a
// kind the build doesn't run is an unsupported mode, never a plain DMR channel. Knows no kind's fields: a kind's
// parameter block is opaque bytes here, fetched and decoded by its mode module. Every call takes storage_lock
#ifndef RFOUNDRY_CODEPLUG_TAG_H
#define RFOUNDRY_CODEPLUG_TAG_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"

// The tag's header (2.1): kind, system, name check (2), flags; with CODEPLUG_TAG_FALLBACK the fallback channel (2) and
// its name check (2) follow; then the kind's parameter block
#define CODEPLUG_TAG_HDR	5u
#define CODEPLUG_TAG_FB_LEN	4u
#define CODEPLUG_TAG_HEAD_MAX	(CODEPLUG_TAG_HDR + CODEPLUG_TAG_FB_LEN)
#define CODEPLUG_SYSTEM_NONE	0xFFu	// no system

// Flags: the low four bits are common to every kind, the high four the kind's own
#define CODEPLUG_TAG_TX		0x01u	// transmit allowed (clear: receive only, so a zeroed tag never transmits)
#define CODEPLUG_TAG_BASE_OWN	0x02u	// the base record stands on its own: any record shape (a mixed channel's FM)
#define CODEPLUG_TAG_FALLBACK	0x04u	// a fallback channel follows the header
#define CODEPLUG_TAG_COMMON	0x0Fu	// bit 3 is reserved: written 0, kept as found

typedef struct {
	uint8_t kind;
	uint8_t system;		// key CODEPLUG_KEY_SYSTEM + system, or CODEPLUG_SYSTEM_NONE
	uint8_t flags;
	uint16_t check;		// the channel's name check
	uint16_t fb_chan;	// with CODEPLUG_TAG_FALLBACK: the fallback channel, 0 = none (else: the system's)
	uint16_t fb_check;	// its name check
	uint16_t len;		// the stored value's length; the parameter block is what follows the header
} codeplug_tag_t;

// FNV-1a (32 bit) over a name as a channel record stores it (16 octets, NUL padded), the halves XORed (2.2)
uint16_t codeplug_name_check(const char *name);

// The tag stored at channel number, as stored (whether live or not); false when none or too short to be one
bool codeplug_tag_read(uint16_t number, codeplug_tag_t *t);
// Whether a tag is live for a channel record of this name, mode and receive-only bit (2.3)
bool codeplug_tag_live(const codeplug_tag_t *t, const char *name, uint8_t mode, bool rx_only);
// Where the parameter block starts in the stored value
static inline uint16_t codeplug_tag_params_at(const codeplug_tag_t *t)
{
	return (t->flags & CODEPLUG_TAG_FALLBACK) != 0 ? CODEPLUG_TAG_HEAD_MAX : CODEPLUG_TAG_HDR;
}

// A channel read by codeplug_channel (ch->kind set): its kind's parameter block into buf, at most cap bytes. The
// block's stored length, or -1 when the channel has no live tag of that kind under that name any more. A block longer
// than cap is cut (the length says so). buf needs CODEPLUG_TAG_HEAD_MAX bytes more than cap: the value is read whole
int codeplug_tag_params(uint16_t number, const channel_t *ch, uint8_t *buf, uint16_t cap);

// Write channel number's tag: t's kind, system, flags and fallback, the check of name, then len bytes of params. The
// record is the caller's to write first (the base record rule, 2.3). False when the area can't be written
bool codeplug_tag_write(uint16_t number, const char *name, const codeplug_tag_t *t, const uint8_t *params, uint16_t len);
// Remove channel number's tag; true when none is left
bool codeplug_tag_remove(uint16_t number);

// --- For codeplug.c, under storage_lock ---
// A live tag applied to a channel just read (kind, system, receive only); nothing for an untagged one. A codeplug with
// no tag at all pays one walk of the tag keys per change of codeplug_kv_serial()
void codeplug_tag_apply(uint16_t number, channel_t *ch);
// What a channel write does to the tag at its number (2.4)
enum {
	CODEPLUG_TAG_W_NONE,	// there is none
	CODEPLUG_TAG_W_FOLLOW,	// it follows the channel: name check, system, transmit allowed
	CODEPLUG_TAG_W_KEEP,	// a stale tag, kept as found
	CODEPLUG_TAG_W_REMOVE,	// it goes
};
// Before a write of ch at number (in_use: a channel is stored there, its name old_name as stored, 16 octets): the
// action, the stored tag in *t, and in *safe_shape whether the record must be written DMR, receive only (the base
// record rule: a channel of a tagged kind, unless its tag says the base stands on its own)
uint8_t codeplug_tag_before_write(uint16_t number, const channel_t *ch, bool in_use, const char *old_name,
				  codeplug_tag_t *t, bool *safe_shape);
// After the record is written: the action carried out. False when the area couldn't be written
bool codeplug_tag_after_write(uint16_t number, const channel_t *ch, uint8_t action, const codeplug_tag_t *t);

#endif
