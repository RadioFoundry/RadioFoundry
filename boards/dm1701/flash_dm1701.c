// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 SPI flash (W25Q128 on SPI1): STUB, to be replaced
#include "dm1701.h"

static bool init(void) { return false; }
static bool rd(uint32_t a, void *b, uint32_t n) { (void)a; (void)b; (void)n; return false; }
static bool wr(uint32_t a, const void *b, uint32_t n) { (void)a; (void)b; (void)n; return false; }
static bool er(uint32_t a) { (void)a; return false; }

hal_flash_t dm1701_flash = { .init = init, .size = 0, .erase_size = 4096, .read = rd, .write = wr, .erase = er };
