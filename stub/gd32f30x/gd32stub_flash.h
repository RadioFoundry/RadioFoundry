// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x internal flash erase and program for the flash stub (gd32stub_flash.c). The protocol engine
// (stub/common/stubeng.c) has already applied the safety rules; these functions only drive the controller.
#ifndef GD32STUB_FLASH_H
#define GD32STUB_FLASH_H

#include <stdbool.h>
#include <stdint.h>

void gd32flash_init(uint16_t flash_kb);
// Erase the 4 KB at addr (a sector of the engine's map: two pages in bank 0, one in bank 1)
bool gd32flash_erase(uint32_t addr);
bool gd32flash_program(uint32_t addr, const uint32_t *words, uint32_t n);

#endif
