// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Program flash self-programming with the FTFA controller: erase a 2 KB sector, program longwords. The flash can't be
// read while a command runs, so the launch and the wait run from RAM (.ramfunc, copied there with .data at start-up)
// with interrupts off, and the FMC's cache and prefetch buffer are invalidated afterwards. The controller doesn't run
// commands in HSRUN, so the clock steps down to RUN around each one (clock.c). The bootloader region below 0x4000 and
// the last sector are refused (ftfa_math.h). Every wait is bounded by the cycle counter: a command that never ends
// reports a timeout instead of hanging the radio.
#include <string.h>
#include "platform.h"
#include "ftfa_math.h"

// Data sheet maxima: sector erase 114 ms, longword program 145 us; the limits leave a wide margin
#define ERASE_LIMIT_MS		300u
#define PROGRAM_LIMIT_MS	5u
#define FSTAT_ERRORS		(FTFA_FSTAT_ACCERR | FTFA_FSTAT_FPVIOL | FTFA_FSTAT_MGSTAT0)

// Launch the command already in FCCOB and wait for it, from RAM: nothing here may fetch from flash (no calls, the
// registers by address). Returns FSTAT, or 0 on a timeout (CCIF never came back)
__attribute__((section(".ramfunc"), noinline, long_call))
static uint8_t ftfa_run(uint32_t limit_cycles)
{
	FTFA_FSTAT = FTFA_FSTAT_CCIF;
	uint32_t start = DWT->CYCCNT;
	while ((FTFA_FSTAT & FTFA_FSTAT_CCIF) == 0) {
		if (DWT->CYCCNT - start > limit_cycles) {
			return 0;
		}
	}
	return FTFA_FSTAT;
}

static k22f_status_t command(uint8_t cmd, uint32_t addr, uint32_t word, uint32_t limit_ms)
{
	uint8_t fccob[8];
	k22f_ftfa_fccob(cmd, addr, word, fccob);
	uint8_t n = cmd == K22F_FTFA_CMD_PROGRAM_LONGWORD ? 8u : 4u;

	if (!k22f_clock_step_down(true)) {
		return K22F_ERR_TIMEOUT;
	}
	uint32_t limit = limit_ms * (SystemCoreClock / 1000u);
	uint32_t s = k22f_irq_save();
	uint8_t st = FTFA_FSTAT_CCIF;
	uint32_t w = 0;
	while ((FTFA_FSTAT & FTFA_FSTAT_CCIF) == 0 && ++w < 1000000u) {	// a command still running (none should be)
	}
	if ((FTFA_FSTAT & FTFA_FSTAT_CCIF) == 0) {
		st = 0;
	} else {
		FTFA_FSTAT = FTFA_FSTAT_ACCERR | FTFA_FSTAT_FPVIOL | FTFA_FSTAT_RDCOLERR;	// write 1 to clear
		for (uint8_t i = 0; i < n; i++) {
			FTFA_FCCOB(i) = fccob[i];
		}
		st = ftfa_run(limit);
		FMC_PFB0CR |= FMC_PFB0CR_INVALIDATE;
	}
	k22f_irq_restore(s);
	(void)k22f_clock_step_down(false);
	if (st == 0) {
		return K22F_ERR_TIMEOUT;
	}
	return (st & FSTAT_ERRORS) != 0 ? K22F_ERR_FLASH : K22F_OK;
}

k22f_status_t k22f_flash_erase_sector(uint32_t addr)
{
	if (!k22f_ftfa_erase_ok(addr)) {
		return K22F_ERR_ARG;
	}
	return command(K22F_FTFA_CMD_ERASE_SECTOR, addr, 0, ERASE_LIMIT_MS);
}

k22f_status_t k22f_flash_program(uint32_t addr, const void *data, uint32_t len)
{
	if (data == NULL || !k22f_ftfa_program_ok(addr, len)) {
		return K22F_ERR_ARG;
	}
	const uint8_t *p = data;
	for (uint32_t off = 0; off < len; off += 4u) {
		uint32_t word;
		memcpy(&word, p + off, 4);	// little-endian: the byte at addr is the word's low byte
		k22f_status_t st = command(K22F_FTFA_CMD_PROGRAM_LONGWORD, addr + off, word, PROGRAM_LIMIT_MS);
		if (st != K22F_OK) {
			return st;
		}
	}
	return K22F_OK;
}
