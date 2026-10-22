// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// STM32F4 internal flash erase and program for the flash stub (f4stub_flash.c). The protocol engine
// (stub/common/stubeng.c) has already applied the safety rules; these functions only drive the controller.
#ifndef F4STUB_FLASH_H
#define F4STUB_FLASH_H

#include <stdbool.h>
#include <stdint.h>

void f4flash_init(void);
bool f4flash_erase(unsigned sector);
bool f4flash_program(uint32_t addr, const uint32_t *words, uint32_t n);

#endif
