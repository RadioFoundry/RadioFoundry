// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// K22F program flash erase and program for the flash stub (k22stub_flash.c). The protocol engine
// (stub/common/stubeng.c) has already applied the safety rules; these functions only drive the FTFA controller.
#ifndef K22STUB_FLASH_H
#define K22STUB_FLASH_H

#include <stdbool.h>
#include <stdint.h>

void k22flash_init(void);
// Erase the 2 KB sector at addr
bool k22flash_erase(uint32_t addr);
// Program n longwords at addr (erased, checked by the engine)
bool k22flash_program(uint32_t addr, const uint32_t *words, uint32_t n);

#endif
