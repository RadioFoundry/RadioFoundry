// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated SPI NOR flash for host tests, as a hal_flash_t: programming only clears bits (old & new), erase sets a
// 4 KB sector to 0xFF, and a power cut can be armed after a number of "units" (one programmed byte or one erase):
// the operation in progress is torn (the bytes after the cut are not programmed; a torn erase leaves the sector
// half old, half erased), and every operation fails until sim_flash_power_on().
#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

#define SIM_FLASH_SECTOR	4096u

extern hal_flash_t sim_flash;
extern uint8_t *sim_flash_data;

// A fresh flash of size bytes, all erased, with the given map (NULL: none)
void sim_flash_create(uint32_t size, const hal_flash_map_t *map);
void sim_flash_free(void);
// Arm a power cut after units more units; 0 disarms
void sim_flash_cut_after(uint32_t units);
bool sim_flash_is_cut(void);
void sim_flash_power_on(void);
// Units used so far (programmed bytes + erases), and erases per sector so far
uint32_t sim_flash_units(void);
uint32_t sim_flash_erases(uint32_t addr);
// A worn range: programs there fail (nothing programmed); erases still work. len 0 clears it
void sim_flash_bad_range(uint32_t addr, uint32_t len);
// Count an operation that programs a 1 where the flash holds a 0 (a missing erase), as an error
uint32_t sim_flash_violations(void);

#endif
