// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mode-scoped record families of the codeplug settings area (docs/spec/channel-modes.md 5): a kind's radio-wide
// record, call targets the contact record can't hold, receive lists of them, and traffic keys. Generic: a kind's or a
// system type's bytes are opaque here, decoded by its mode module. Readers only; the editor and sharing write them.
// Compiled only when a built mode uses them (CODEPLUG_MODE_RECORDS), so a build without one pays nothing
#ifndef RFOUNDRY_CODEPLUG_RECORDS_H
#define RFOUNDRY_CODEPLUG_RECORDS_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"

// A reference to a record of a family (5.1): its number + 1 (0 = none) and the name check of the name it had, so a
// record deleted or replaced never resolves. 4 octets wherever stored
typedef struct {
	uint16_t index;		// record number + 1, 0 = none
	uint16_t check;		// codeplug_name_check of the record's name
} codeplug_ref_t;
#define CODEPLUG_REF_LEN	4u

// Call types of a target (flags bits 0-1); bits 2-3 reserved, 4-7 the kind's
enum {
	CODEPLUG_TARGET_INDIVIDUAL,
	CODEPLUG_TARGET_GROUP,
	CODEPLUG_TARGET_ALL,
	CODEPLUG_TARGET_OTHER,
};
#define CODEPLUG_TARGET_TYPE	0x03u

// A target's or a list's header (5.2, 5.3): kind, flags, name (16 octets); the address or the references follow
#define CODEPLUG_REC_HDR	(2u + CODEPLUG_NAME_LEN)
typedef struct {
	uint8_t kind;
	uint8_t flags;
	char name[CODEPLUG_NAME_LEN + 1];
} codeplug_rec_head_t;

// A traffic key's header (5.4): system, algorithm, key id (2), flags; the key's octets follow
#define CODEPLUG_TRAFFIC_HDR	5u

// A system record's header (7.1): type, flags, sharing, identity length, own ID (4), fallback channel (2) and its name
// check (2), name (16); the type's block follows, its identity first
#define CODEPLUG_SYS_HDR	28u
#define CODEPLUG_SYS_NEEDS_KEY	0x01u	// flags: the system can't be used without its secret
#define CODEPLUG_SYS_TX		0x02u	// flags: may register, affiliate and transmit (clear: listen only, never)
enum { CODEPLUG_SHARE_OFF, CODEPLUG_SHARE_IDENTITY, CODEPLUG_SHARE_WHOLE };
typedef struct {
	uint8_t type;
	uint8_t flags;
	uint8_t sharing;
	uint8_t id_len;		// the identity: the first id_len octets of the type's block
	uint32_t own_id;	// this radio's ID on the system, 0 = the kind's radio-wide one
	uint16_t fb_chan;	// fallback channel, 0 = none
	uint16_t fb_check;
	char name[CODEPLUG_NAME_LEN + 1];
} codeplug_system_t;

// A system entry's header (7.2): system, class, group, flags; the class's body follows
#define CODEPLUG_SYS_ENTRY_HDR	4u
enum {
	CODEPLUG_ENTRY_CHANNEL,	// body: channel number (2), RX Hz (4), TX Hz (4), the type's tail
	CODEPLUG_ENTRY_SITE,	// body: the type's; the entries of group = this entry's group belong to it
	CODEPLUG_ENTRY_PLAN,	// body: the type's (a channel plan row)
};
#define CODEPLUG_ENTRY_CONTROL	0x01u	// flags: a control channel
#define CODEPLUG_GROUP_NONE	0xFFu	// the whole system's
typedef struct {
	uint8_t system;
	uint8_t cls;
	uint8_t group;
	uint8_t flags;
	uint16_t len;		// the body's stored length
} codeplug_sys_entry_t;

#if CODEPLUG_MODE_RECORDS
// System n: its header in *s and the type's block into block (at most cap octets; block needs CODEPLUG_SYS_HDR more).
// The block's stored length, -1 when there is no system n
int codeplug_system(uint8_t n, codeplug_system_t *s, uint8_t *block, uint16_t cap);
// The next entry of system with class cls, after entry number *at (start with *at = -1): its header in *e, its body into
// body (at most cap octets, up to 64). Its entry number, -1 at the end
int codeplug_sys_entry_next(int *at, uint8_t system, uint8_t cls, codeplug_sys_entry_t *e, uint8_t *body,
			    uint16_t cap);
// System n's own secret into buf (at most cap): its length, -1 when none
int codeplug_system_secret(uint8_t n, uint8_t *buf, uint16_t cap);

// kind's radio-wide record into buf (at most cap bytes): its stored length, -1 when absent
int codeplug_mode_settings(uint8_t kind, uint8_t *buf, uint16_t cap);
// The target ref points at, when it is still that target and of kind: its header in *h, its address into addr (at most
// cap octets; addr needs CODEPLUG_REC_HDR octets more: the value is read whole); the address's stored length, -1 when
// the reference doesn't resolve
int codeplug_target(codeplug_ref_t ref, uint8_t kind, codeplug_rec_head_t *h, uint8_t *addr, uint16_t cap);
// List ref of kind: its header in *h and up to max of its references into refs; how many it holds, -1 when it doesn't
// resolve. Each reference resolves (or not) on its own: a list outlives a target deleted from under it
int codeplug_target_list(codeplug_ref_t ref, uint8_t kind, codeplug_rec_head_t *h, codeplug_ref_t *refs, uint16_t max);
// The traffic key of system with this algorithm and key id: its octets into key (at most cap); their length, -1 when
// there is none
int codeplug_traffic_key(uint8_t system, uint8_t alg, uint16_t key_id, uint8_t *key, uint16_t cap);
// A reference from the 4 octets a record stores
codeplug_ref_t codeplug_ref_at(const uint8_t *p);
#endif

#endif
