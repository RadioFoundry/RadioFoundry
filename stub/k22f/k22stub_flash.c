// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// K22F program flash: sector erase (FTFA command 0x09) and longword program (0x06), from the reference manual's FTFA
// chapter: wait for CCIF, clear the error flags, fill the command object, launch by writing CCIF, wait. The stub runs
// from SRAM, so the CPU keeps going while the flash is busy; the erase wait services USB meanwhile (nothing on that
// path reads the flash). The command object and the writable window are shared with RadioFoundry's own driver
// (platform/k22f/ftfa_math.h). Nothing here writes the flash configuration field, FPROT or the security byte.
#include "k22f.h"
#include "ftfa_math.h"
#include "k22stub_flash.h"
#include "stubrt.h"
#include "stubusb.h"

#define FSTAT_ERRORS	(FTFA_FSTAT_ACCERR | FTFA_FSTAT_FPVIOL | FTFA_FSTAT_MGSTAT0)
#define ERASE_MS	300u	// data sheet maximum 114 ms for a sector
#define PROGRAM_MS	5u	// a longword: 145 us maximum

static bool wait_ccif(uint32_t ms, bool poll_usb)
{
	uint32_t t0 = stub_ms();
	while ((FTFA_FSTAT & FTFA_FSTAT_CCIF) == 0) {
		if (poll_usb) {
			usb_poll();
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return true;
}

static bool command(uint8_t cmd, uint32_t addr, uint32_t word, uint32_t ms, bool poll_usb)
{
	// The controller refuses commands in HSRUN; RadioFoundry steps down to RUN before the jump, so this only catches a hand-over
	// that didn't
	if (SMC_PMSTAT & SMC_PMSTAT_HSRUN) {
		return false;
	}
	if (!wait_ccif(ms, false)) {
		return false;
	}
	uint8_t fccob[8];
	k22f_ftfa_fccob(cmd, addr, word, fccob);
	uint8_t n = cmd == K22F_FTFA_CMD_PROGRAM_LONGWORD ? 8u : 4u;
	FTFA_FSTAT = FTFA_FSTAT_ACCERR | FTFA_FSTAT_FPVIOL | FTFA_FSTAT_RDCOLERR;	// write 1 to clear
	for (uint8_t i = 0; i < n; i++) {
		FTFA_FCCOB(i) = fccob[i];
	}
	FTFA_FSTAT = FTFA_FSTAT_CCIF;	// launch
	bool done = wait_ccif(ms, poll_usb);
	FMC_PFB0CR |= FMC_PFB0CR_INVALIDATE;	// no stale line from the cache or the speculation buffer
	return done && (FTFA_FSTAT & FSTAT_ERRORS) == 0;
}

void k22flash_init(void)
{
	FMC_PFB0CR |= FMC_PFB0CR_INVALIDATE;
}

bool k22flash_erase(uint32_t addr)
{
	return command(K22F_FTFA_CMD_ERASE_SECTOR, addr, 0, ERASE_MS, true);
}

bool k22flash_program(uint32_t addr, const uint32_t *words, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (!command(K22F_FTFA_CMD_PROGRAM_LONGWORD, addr + 4u * i, words[i], PROGRAM_MS, false)) {
			return false;
		}
	}
	return true;
}
