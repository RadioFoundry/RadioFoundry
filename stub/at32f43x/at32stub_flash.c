// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x internal flash: sector erase and word program through the flash controller (AT32F435/437 reference
// manual, flash memory controller: the UNLOCK key sequence, SECERS / FPRGM with ERSTR, the STS busy and error flags,
// written 1 to clear). Bank 2 has its own unlock, status, control and address registers 0x40 above bank 1's; a vendor
// image on this chip drives them the same way (docs/spec/stub-at32f43x.md 2). The stub runs from SRAM, so the CPU
// keeps going while the flash is busy; the erase wait services USB meanwhile. Nothing here touches the user system
// data key or the user system data (access protection, erase/program protection, EOPB0).
#include "at32f43x.h"
#include "at32stub.h"
#include "at32stub_flash.h"
#include "at32stub_usb.h"

#define FLASH_UNLOCK(b)	AT32_REG(FLASH_BASE + ((b) ? 0x44u : 0x04u))
#define FLASH_STS(b)	AT32_REG(FLASH_BASE + ((b) ? 0x4Cu : 0x0Cu))
#define FLASH_CTRL(b)	AT32_REG(FLASH_BASE + ((b) ? 0x50u : 0x10u))
#define FLASH_ADDR(b)	AT32_REG(FLASH_BASE + ((b) ? 0x54u : 0x14u))

#define STS_OBF		(1u << 0)	// operation busy
#define STS_PRGMERR	(1u << 2)
#define STS_EPPERR	(1u << 4)	// erase / program protection
#define STS_ODF		(1u << 5)	// operation done
#define STS_CLEAR	(STS_PRGMERR | STS_EPPERR | STS_ODF)
#define CTRL_FPRGM	(1u << 0)
#define CTRL_SECERS	(1u << 1)
#define CTRL_ERSTR	(1u << 6)
#define CTRL_OPLK	(1u << 7)

#define KEY1		0x45670123u
#define KEY2		0xCDEF89ABu
#define ERASE_MS	1000u	// one sector: tens of ms in the data sheet; more before calling it failed
#define PROGRAM_MS	10u	// one word: tens of us

static uint32_t bank2;		// where bank 2 starts, 0 for a part with one bank

// Which bank's registers serve addr
static unsigned bank_of(uint32_t addr)
{
	return bank2 != 0 && addr >= bank2 ? 1u : 0u;
}

static bool unlock(unsigned b)
{
	if (FLASH_CTRL(b) & CTRL_OPLK) {
		FLASH_UNLOCK(b) = KEY1;
		FLASH_UNLOCK(b) = KEY2;
	}
	return (FLASH_CTRL(b) & CTRL_OPLK) == 0;
}

static void lock(unsigned b)
{
	FLASH_CTRL(b) = CTRL_OPLK;
}

static bool wait_idle(unsigned b, uint32_t ms, bool poll_usb)
{
	uint32_t t0 = stub_ms();
	while (FLASH_STS(b) & STS_OBF) {
		if (poll_usb) {
			usb_poll();
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return (FLASH_STS(b) & (STS_PRGMERR | STS_EPPERR)) == 0;
}

void at32flash_init(uint16_t flash_kb)
{
	bank2 = at32_bank2_start(flash_kb);
	lock(0);
	if (bank2 != 0) {
		lock(1);
	}
}

static bool blank(uint32_t addr, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4u) {
		if (*(const volatile uint32_t *)(addr + i) != 0xFFFFFFFFu) {
			return false;
		}
	}
	return true;
}

static bool erase_sector(uint32_t addr)
{
	unsigned b = bank_of(addr);
	if (FLASH_STS(b) & STS_OBF) {
		return false;
	}
	FLASH_STS(b) = STS_CLEAR;
	if (!unlock(b)) {
		return false;
	}
	FLASH_CTRL(b) = CTRL_SECERS;
	FLASH_ADDR(b) = addr;
	FLASH_CTRL(b) = CTRL_SECERS | CTRL_ERSTR;
	__DSB();
	bool ok = wait_idle(b, ERASE_MS, true);
	lock(b);
	return ok;
}

// The part's sector is 2 KB or 4 KB (4 KB on the parts over 1 MB per the manual, not confirmed on a part). Each 2 KB
// half that isn't blank gets a sector erase: on a 4 KB part the first erase blanks both halves, so the second is
// skipped. A sector erase never reaches past the engine's 4 KB either way, and each half must read blank afterwards
bool at32flash_erase(uint32_t addr)
{
	for (uint32_t a = addr; a < addr + AT32_SECTOR_SIZE; a += AT32_ERASE_STEP) {
		if (!blank(a, AT32_ERASE_STEP) && (!erase_sector(a) || !blank(a, AT32_ERASE_STEP))) {
			return false;
		}
	}
	return true;
}

// Words in one bank
static bool program_run(uint32_t addr, const uint32_t *words, uint32_t n)
{
	unsigned b = bank_of(addr);
	if (FLASH_STS(b) & STS_OBF) {
		return false;
	}
	FLASH_STS(b) = STS_CLEAR;
	if (!unlock(b)) {
		return false;
	}
	FLASH_CTRL(b) = CTRL_FPRGM;
	bool ok = true;
	for (uint32_t i = 0; i < n && ok; i++) {
		*(volatile uint32_t *)(addr + 4u * i) = words[i];
		__DSB();
		ok = wait_idle(b, PROGRAM_MS, false);
	}
	lock(b);
	return ok;
}

bool at32flash_program(uint32_t addr, const uint32_t *words, uint32_t n)
{
	// A request that crosses from bank 1 into bank 2 goes to each bank's registers in turn
	uint32_t first = n;
	if (n > 0 && bank_of(addr) == 0u && bank_of(addr + 4u * n - 4u) == 1u) {
		first = (bank2 - addr) / 4u;
	}
	return program_run(addr, words, first) && (first == n || program_run(addr + 4u * first, words + first, n - first));
}
