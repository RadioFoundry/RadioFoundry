// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// STM32F4 internal flash: sector erase and word program through the flash interface (reference manual, "Embedded
// flash memory interface": unlock keys, PSIZE x32 for a 2.7-3.6 V supply, SER/SNB/STRT, PG, the SR error flags). The
// stub runs from SRAM, so the CPU keeps going while the flash is busy; the wait loop services USB meanwhile.
// Nothing here touches FLASH_OPTCR or its key: the option bytes (read protection, write protection) are never written.
#include "stm32f4xx.h"
#include "f4stub_flash.h"
#include "f4stub_usb.h"

#define KEY1		0x45670123u
#define KEY2		0xCDEF89ABu
#define SR_ERRORS	(FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)
#define ERASE_MS	4000u	// 128 KB at x32: 2 s maximum in the data sheet; twice that before calling it failed
#define PROGRAM_MS	10u	// one word: 100 us maximum

static bool unlock(void)
{
	if (FLASH->CR & FLASH_CR_LOCK) {
		FLASH->KEYR = KEY1;
		FLASH->KEYR = KEY2;
	}
	return (FLASH->CR & FLASH_CR_LOCK) == 0;
}

static void lock(void)
{
	FLASH->CR = FLASH_CR_LOCK;
}

static bool wait_idle(uint32_t ms, bool poll_usb)
{
	uint32_t t0 = stub_ms();
	while (FLASH->SR & FLASH_SR_BSY) {
		if (poll_usb) {
			usb_poll();
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return (FLASH->SR & SR_ERRORS) == 0;
}

void f4flash_init(void)
{
	// Caches and prefetch off: the stub runs from SRAM, and a disabled data cache can't return a stale word after an
	// erase or a program. The wait states RadioFoundry set stay (they match the clock it left running)
	FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN);
	FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
	FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
	lock();
}

bool f4flash_erase(unsigned sector)
{
	if (!wait_idle(ERASE_MS, true) && (FLASH->SR & FLASH_SR_BSY)) {
		return false;
	}
	FLASH->SR = SR_ERRORS | FLASH_SR_EOP;	// write 1 to clear
	if (!unlock()) {
		return false;
	}
	FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_SER | ((uint32_t)sector << FLASH_CR_SNB_Pos);
	FLASH->CR |= FLASH_CR_STRT;
	__DSB();
	bool ok = wait_idle(ERASE_MS, true);
	lock();
	return ok;
}

bool f4flash_program(uint32_t addr, const uint32_t *words, uint32_t n)
{
	if (FLASH->SR & FLASH_SR_BSY) {
		return false;
	}
	FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
	if (!unlock()) {
		return false;
	}
	FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
	bool ok = true;
	for (uint32_t i = 0; i < n && ok; i++) {
		*(volatile uint32_t *)(addr + 4u * i) = words[i];
		__DSB();
		ok = wait_idle(PROGRAM_MS, false);
	}
	lock();
	return ok;
}
