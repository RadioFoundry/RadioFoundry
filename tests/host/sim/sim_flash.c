// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated SPI NOR flash (sim_flash.h)
#include <stdlib.h>
#include <string.h>
#include "sim_flash.h"

uint8_t *sim_flash_data;
static uint32_t *erase_count;
static uint32_t budget;		// units left before the cut, 0 = not armed
static bool armed, cut;
static uint32_t units, violations;
static uint32_t bad_addr, bad_len;

static bool sim_init(void) { return true; }

static bool in_range(uint32_t addr, uint32_t len)
{
	return addr < sim_flash.size && len <= sim_flash.size - addr;
}

// One unit used; false when the power goes now
static bool spend(void)
{
	if (cut) {
		return false;
	}
	if (armed) {
		if (budget == 0) {
			cut = true;
			return false;
		}
		budget--;
	}
	units++;
	return true;
}

static bool sim_read(uint32_t addr, void *buf, uint32_t len)
{
	if (cut || !in_range(addr, len)) {
		return false;
	}
	memcpy(buf, &sim_flash_data[addr], len);
	return true;
}

static bool sim_write(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	if (cut || !in_range(addr, len)) {
		return false;
	}
	if (bad_len != 0 && addr < bad_addr + bad_len && addr + len > bad_addr) {
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (!spend()) {
			return false;
		}
		if ((p[i] & ~sim_flash_data[addr + i]) != 0) {
			violations++;
		}
		sim_flash_data[addr + i] &= p[i];
	}
	return true;
}

static bool sim_erase(uint32_t addr)
{
	uint32_t base = addr & ~(SIM_FLASH_SECTOR - 1u);
	if (cut || !in_range(base, SIM_FLASH_SECTOR)) {
		return false;
	}
	if (!spend()) {
		// Torn erase: the first half erased, the rest scrambled
		memset(&sim_flash_data[base], 0xFF, SIM_FLASH_SECTOR / 2);
		for (uint32_t i = SIM_FLASH_SECTOR / 2; i < SIM_FLASH_SECTOR; i++) {
			sim_flash_data[base + i] |= (uint8_t)(i * 37u);
		}
		return false;
	}
	memset(&sim_flash_data[base], 0xFF, SIM_FLASH_SECTOR);
	erase_count[base / SIM_FLASH_SECTOR]++;
	return true;
}

hal_flash_t sim_flash = {
	.init = sim_init, .size = 0, .erase_size = SIM_FLASH_SECTOR,
	.read = sim_read, .write = sim_write, .erase = sim_erase, .map = NULL,
};

void sim_flash_create(uint32_t size, const hal_flash_map_t *map)
{
	sim_flash_free();
	sim_flash_data = malloc(size);
	erase_count = calloc(size / SIM_FLASH_SECTOR + 1u, sizeof(uint32_t));
	memset(sim_flash_data, 0xFF, size);
	sim_flash.size = size;
	sim_flash.map = map;
	armed = cut = false;
	units = violations = 0;
	bad_len = 0;
}

void sim_flash_free(void)
{
	free(sim_flash_data);
	free(erase_count);
	sim_flash_data = NULL;
	erase_count = NULL;
}

void sim_flash_cut_after(uint32_t n)
{
	armed = n != 0;
	budget = n;
}

bool sim_flash_is_cut(void) { return cut; }

void sim_flash_power_on(void)
{
	cut = false;
	armed = false;
}

uint32_t sim_flash_units(void) { return units; }
uint32_t sim_flash_erases(uint32_t addr) { return erase_count[addr / SIM_FLASH_SECTOR]; }
uint32_t sim_flash_violations(void) { return violations; }

void sim_flash_bad_range(uint32_t addr, uint32_t len)
{
	bad_addr = addr;
	bad_len = len;
}
