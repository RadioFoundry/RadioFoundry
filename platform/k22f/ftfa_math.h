// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Program flash self-programming, the pure part (ftfa.c runs the commands): which addresses the API may touch, and the
// FTFA command object for an erase or a longword program. Pure: tests/host/k22fSystemTest.c checks it.
#ifndef K22F_FTFA_MATH_H
#define K22F_FTFA_MATH_H

#include <stdint.h>
#include <stdbool.h>

#define K22F_FTFA_CMD_PROGRAM_LONGWORD	0x06u
#define K22F_FTFA_CMD_ERASE_SECTOR	0x09u
#define K22F_FTFA_SECTOR		0x800u		// 2 KB
// What the API never writes: the radio's bootloader below 0x4000 (with the flash configuration field at 0x400) and
// the last sector, radio data that must survive (docs/spec/hardware-gd77.md 6.1)
#define K22F_FTFA_LO			0x00004000u
#define K22F_FTFA_HI			0x0007F800u	// exclusive

// [addr, addr + len) is inside the writable window. len 0 is refused
static inline bool k22f_ftfa_range_ok(uint32_t addr, uint32_t len)
{
	return len != 0u && addr >= K22F_FTFA_LO && addr < K22F_FTFA_HI && len <= K22F_FTFA_HI - addr;
}

static inline bool k22f_ftfa_erase_ok(uint32_t addr)
{
	return (addr & (K22F_FTFA_SECTOR - 1u)) == 0u && k22f_ftfa_range_ok(addr, K22F_FTFA_SECTOR);
}

// Longwords only: the address and the length are multiples of 4
static inline bool k22f_ftfa_program_ok(uint32_t addr, uint32_t len)
{
	return (addr & 3u) == 0u && (len & 3u) == 0u && k22f_ftfa_range_ok(addr, len);
}

// The command object FCCOB0..7: the command, the 24-bit address high byte first, then for a program the longword in
// the order the flash stores it (FCCOB4 is the byte at addr + 3, FCCOB7 the byte at addr)
static inline void k22f_ftfa_fccob(uint8_t cmd, uint32_t addr, uint32_t word, uint8_t out[8])
{
	out[0] = cmd;
	out[1] = (uint8_t)(addr >> 16);
	out[2] = (uint8_t)(addr >> 8);
	out[3] = (uint8_t)addr;
	out[4] = (uint8_t)(word >> 24);
	out[5] = (uint8_t)(word >> 16);
	out[6] = (uint8_t)(word >> 8);
	out[7] = (uint8_t)word;
}

// The cause of the last reset (RCM_SRS0 | RCM_SRS1 << 8) as one word for the diagnostics page and the debug link. The
// registers hold the last reset's sources only; a power-on sets the low voltage bit too, so "por" comes before "lvd"
static inline const char *k22f_reset_name(uint16_t srs)
{
	static const struct {
		uint16_t bit;
		const char *name;
	} order[] = {
		{ 1u << 9, "lockup" }, { 1u << 5, "wdog" }, { 1u << 13, "sackerr" }, { 1u << 3, "lol" }, { 1u << 2, "loc" },
		{ 1u << 10, "sw" }, { 1u << 11, "mdm" }, { 1u << 8, "jtag" }, { 1u << 12, "ezport" }, { 1u << 7, "por" },
		{ 1u << 6, "pin" }, { 1u << 1, "lvd" }, { 1u << 0, "wakeup" },
	};
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (srs & order[i].bit) {
			return order[i].name;
		}
	}
	return "none";
}

#endif
