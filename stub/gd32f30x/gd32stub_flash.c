// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x internal flash: page erase and word program through the FMC (GD32F30x user manual, "Flash memory
// controller": the KEY unlock sequence, PER / PG with START, the STAT busy and error flags, written 1 to clear). Parts
// over 512 KB have a second bank with its own key, status and control registers and 4 KB pages; bank 0 has 2 KB pages.
// The stub runs from SRAM, so the CPU keeps going while the flash is busy; the erase wait services USB meanwhile.
// Nothing here touches the option byte key or the option bytes (security protection, erase/program protection).
#include "gd32f30x.h"
#include "gd32stub.h"
#include "gd32stub_flash.h"
#include "gd32stub_usb.h"

#define FMC_KEY(b)	GD32_REG(FMC_BASE + ((b) ? 0x44u : 0x04u))
#define FMC_STAT(b)	GD32_REG(FMC_BASE + ((b) ? 0x4Cu : 0x0Cu))
#define FMC_CTL(b)	GD32_REG(FMC_BASE + ((b) ? 0x50u : 0x10u))
#define FMC_ADDR(b)	GD32_REG(FMC_BASE + ((b) ? 0x54u : 0x14u))

#define STAT_BUSY	(1u << 0)
#define STAT_PGERR	(1u << 2)
#define STAT_WPERR	(1u << 4)
#define STAT_ENDF	(1u << 5)
#define STAT_CLEAR	(STAT_PGERR | STAT_WPERR | STAT_ENDF)
#define CTL_PG		(1u << 0)
#define CTL_PER		(1u << 1)
#define CTL_START	(1u << 6)
#define CTL_LK		(1u << 7)

#define KEY1		0x45670123u
#define KEY2		0xCDEF89ABu
#define PAGE0		0x800u	// bank 0's page
#define ERASE_MS	1000u	// one page: some hundreds of ms at most in the data sheet; more before calling it failed
#define PROGRAM_MS	10u	// one word: tens of us

static bool two_banks;

// Which bank's registers serve addr. A part of 512 KB or less has bank 0's only
static unsigned bank_of(uint32_t addr)
{
	return two_banks && addr >= GD32_BANK0_END ? 1u : 0u;
}

static bool unlock(unsigned b)
{
	if (FMC_CTL(b) & CTL_LK) {
		FMC_KEY(b) = KEY1;
		FMC_KEY(b) = KEY2;
	}
	return (FMC_CTL(b) & CTL_LK) == 0;
}

static void lock(unsigned b)
{
	FMC_CTL(b) = CTL_LK;
}

static bool wait_idle(unsigned b, uint32_t ms, bool poll_usb)
{
	uint32_t t0 = stub_ms();
	while (FMC_STAT(b) & STAT_BUSY) {
		if (poll_usb) {
			usb_poll();
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return (FMC_STAT(b) & (STAT_PGERR | STAT_WPERR)) == 0;
}

void gd32flash_init(uint16_t flash_kb)
{
	two_banks = (uint32_t)flash_kb * 1024u > GD32_BANK0_END - GD32_FLASH_START;
	lock(0);
	if (two_banks) {
		lock(1);
	}
}

static bool erase_page(uint32_t addr)
{
	unsigned b = bank_of(addr);
	if (FMC_STAT(b) & STAT_BUSY) {
		return false;
	}
	FMC_STAT(b) = STAT_CLEAR;
	if (!unlock(b)) {
		return false;
	}
	FMC_CTL(b) = CTL_PER;
	FMC_ADDR(b) = addr;
	FMC_CTL(b) = CTL_PER | CTL_START;
	__DSB();
	bool ok = wait_idle(b, ERASE_MS, true);
	lock(b);
	return ok;
}

bool gd32flash_erase(uint32_t addr)
{
	if (bank_of(addr) == 1u) {
		return erase_page(addr);	// one 4 KB page
	}
	return erase_page(addr) && erase_page(addr + PAGE0);
}

// Words in one bank
static bool program_run(uint32_t addr, const uint32_t *words, uint32_t n)
{
	unsigned b = bank_of(addr);
	if (FMC_STAT(b) & STAT_BUSY) {
		return false;
	}
	FMC_STAT(b) = STAT_CLEAR;
	if (!unlock(b)) {
		return false;
	}
	FMC_CTL(b) = CTL_PG;
	bool ok = true;
	for (uint32_t i = 0; i < n && ok; i++) {
		*(volatile uint32_t *)(addr + 4u * i) = words[i];
		__DSB();
		ok = wait_idle(b, PROGRAM_MS, false);
	}
	lock(b);
	return ok;
}

bool gd32flash_program(uint32_t addr, const uint32_t *words, uint32_t n)
{
	// A request that crosses from bank 0 into bank 1 goes to each bank's registers in turn
	uint32_t first = n;
	if (bank_of(addr) == 0u && bank_of(addr + 4u * n - 4u) == 1u) {
		first = (GD32_BANK0_END - addr) / 4u;
	}
	return program_run(addr, words, first) && (first == n || program_run(addr + 4u * first, words + first, n - first));
}
