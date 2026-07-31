// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Data flash (external SPI NOR on most radios)
#ifndef HAL_FLASH_H
#define HAL_FLASH_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	bool (*init)(void);
	uint32_t size;		// bytes, filled in by init on boards that probe it
	uint32_t erase_size;	// smallest erase unit
	bool (*read)(uint32_t addr, void *buf, uint32_t len);
	// Program already erased bytes; any length and alignment
	bool (*write)(uint32_t addr, const void *buf, uint32_t len);
	bool (*erase)(uint32_t addr);	// one erase_size unit
} hal_flash_t;

#endif
