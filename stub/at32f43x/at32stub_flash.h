// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x internal flash erase and program for the flash stub (at32stub_flash.c). The protocol engine
// (stub/common/stubeng.c) has already applied the safety rules; these functions only drive the controller.
#ifndef AT32STUB_FLASH_H
#define AT32STUB_FLASH_H

#include <stdbool.h>
#include <stdint.h>

void at32flash_init(uint16_t flash_kb);
// Erase the 4 KB at addr (a sector of the engine's map: one or two of the part's sectors)
bool at32flash_erase(uint32_t addr);
bool at32flash_program(uint32_t addr, const uint32_t *words, uint32_t n);

#endif
