// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Peek and poke on the debug link (CONFIG_DEBUG_MEM, DL_CMD_MEM, docs/spec/debuglink.md 3.24): memory and register
// reads of 1, 2 or 4 bytes, and, once armed with the key, writes, bit sets and clears and pin changes. For the bench:
// the register state a fault leaves, a chip's settings, a pin's role. Portable: the accesses go through dl_mem_io_t,
// so the host tests run it on a buffer.
#ifndef DL_MEM_H
#define DL_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t (*rd)(uint32_t addr, uint8_t width);
	void (*wr)(uint32_t addr, uint8_t width, uint32_t value);
	// A pin's state (DL_MEM_PS_*) after the action (DL_MEM_PIN_*); false: no such pin. NULL: the platform has none
	bool (*pin)(uint8_t pin, uint8_t action, uint8_t *state);
} dl_mem_io_t;

// One DL_CMD_MEM request into out (status first); returns the length
size_t dl_mem_cmd(const dl_mem_io_t *io, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, uint32_t now_ms);
void dl_mem_disarm(void);
// The target's: plain volatile accesses and board.system's pin hook (dl_mem_target.c)
size_t debuglink_cmd_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);

#endif
