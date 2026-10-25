// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fast codeplug protocol (docs/spec/cps-fast.md): the debug link's CODEPLUG command reads and writes the
// open-firmware codeplug image (the same bytes the open-firmware CPS shows, open-firmware-cps.md 7) in 4 KB blocks.
// Blank blocks are reported, not sent; the host compares CRC-32s so only changed blocks move; blocks travel LZ4
// compressed both ways; writes go to the staging area and are committed under a record that a power cut can't break
// (or straight through the sector journal on a radio without staging). CONFIG_CPS_FAST.
//
// The engine has no RTOS: the debug link hands it each request (cps_fast_cmd), the host link task ticks it (the
// session time-out, the commit's sector copies), and the host tests drive both directly.
#ifndef RFOUNDRY_CPS_FAST_H
#define RFOUNDRY_CPS_FAST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CPSF_VERSION		1u
#define CPSF_BLOCK		4096u	// = STORAGE_SECTOR
#define CPSF_EE_SIZE		0x10000u	// the EEPROM image at image offset 0
#define CPSF_SUMS_MAX		16u	// blocks per SUMS (each one 4 KB of flash read and hashed)
#define CPSF_DATA_MAX		61u	// stream bytes per DATA
#define CPSF_READ_MAX		1000u	// stream bytes per READ reply
#define CPSF_STAGE_MAX		64u	// staged blocks per write session at most (the staging area may hold fewer)
#define CPSF_SESSION_MS		10000u	// a write session ends this long after its last request (not while committing)

// Operations (the first payload byte)
enum {
	CPSF_OP_INFO = 0,	// -> the layout and limits
	CPSF_OP_SUMS,		// addr u32, count u8 -> addr, count, blank mask u16, CRC-32 u32 x count
	CPSF_OP_READ,		// addr u32, offset u16 -> addr, format u8, length u16, CRC-32 u32, offset u16, stream bytes
	CPSF_OP_BEGIN,		// mode u8 (CPSF_MODE_*) -> stage_max u16: a write session
	CPSF_OP_BLOCK,		// addr u32, format u8, length u16, CRC-32 u32: a block follows
	CPSF_OP_DATA,		// offset u16, stream bytes -> received u16
	CPSF_OP_END,		// -> none: the block is decoded, checked and written (direct) or staged
	CPSF_OP_COMMIT,		// -> none: staged: the commit starts (poll STATUS); direct: the session ends
	CPSF_OP_STATUS,		// -> state u8, mode u8, blocks u16, copied u16, reason u8
	CPSF_OP_ABORT,		// -> none: the session ends; staged blocks are dropped
};

// Block formats: an LZ4 stream, or none at all for a block of 0xFF (0 is kept for a raw block, not offered: an LZ4
// stream of incompressible data is only 0.4 % longer, and one format keeps the radio smaller)
enum { CPSF_FMT_LZ4 = 1, CPSF_FMT_BLANK = 2 };
enum { CPSF_MODE_DIRECT = 0, CPSF_MODE_STAGED = 1 };

// STATUS states
enum {
	CPSF_ST_IDLE = 0,	// no session
	CPSF_ST_OPEN,		// a write session, between blocks
	CPSF_ST_BLOCK,		// a block is coming (BLOCK seen, END not yet)
	CPSF_ST_COMMITTING,	// the staged blocks are being copied in
	CPSF_ST_DONE,		// the last session ended well (the codeplug was re-read if it changed)
	CPSF_ST_FAILED,		// the last commit failed: its record stays and the next boot finishes it
};

// Why a request was refused (DL_ST_REFUSED, the byte after the status; also STATUS's last reason)
enum {
	CPSF_R_NONE = 0,
	CPSF_R_RANGE,		// outside the image, or a block that touches the radio's own areas
	CPSF_R_STORAGE,		// the flash can't be written (no flash, or a foreign settings region)
	CPSF_R_NO_STAGING,	// staged mode on a radio without a staging area
	CPSF_R_STAGE_FULL,	// more blocks than the staging area holds
	CPSF_R_STAGE_BUSY,	// the staging area is in use (a TFTP write, or a commit record waiting for the next boot)
	CPSF_R_STATE,		// not now: no session, no block, or one already open
	CPSF_R_CRC,		// the block decoded, but its CRC-32 differs from the one BLOCK gave
	CPSF_R_STREAM,		// a corrupt stream: bad LZ4, a gap in the offsets, the wrong length
	CPSF_R_FLASH,		// a flash operation failed
};

typedef struct {
	uint32_t blocks_read;	// READ replies with data
	uint32_t blocks_written;	// blocks written or staged
	uint32_t commits;	// sessions that ended with the codeplug changed
	uint32_t refused;
	uint8_t state;		// CPSF_ST_*
	uint8_t reason;		// the last refusal's CPSF_R_*
} cps_fast_stats_t;

// At boot, after storage_init and before codeplug_init (and before tftp_codeplug_resume): a commit that a power cut
// interrupted is finished, a torn record is erased. True if a commit was finished
bool cps_fast_resume(void);
// One CODEPLUG request (docs/spec/debuglink.md 3.20): payload in, reply payload out (out[0] = DL_ST_*). Returns its
// length
size_t cps_fast_cmd(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, uint32_t now_ms);
// Every pass of the host link task: the session time-out, one sector of a running commit
void cps_fast_tick(uint32_t now_ms);
// The staging area is ours: a staged session, a commit running, or our record waiting for the next boot
bool cps_fast_staging_busy(void);
void cps_fast_get_stats(cps_fast_stats_t *s);

#endif
