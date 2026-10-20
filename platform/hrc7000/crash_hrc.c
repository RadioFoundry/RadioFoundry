// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash log on the HR_C7000 (hal_crash.h, docs/spec/crash-log.md 2.2). hrc_fault_entry (portasm.S) saves every
// register below the faulting stack pointer and calls hrc_fault (irq.c) on a stack of its own, which hands the frame
// here. The CK803S has no fault address register: the vector, EPC and EPSR say what and where. The record is kept at
// HRC_CRASH_STORE in the modem's SRAM, which neither the chip's soft reset nor the Baofeng bootloader is known to
// touch (unverified on hardware: crash-log.md 5); the radio restarts through the same soft reset as a reboot.
#include "config.h"

#if CONFIG_CRASH_LOG
#include <string.h>
#include "platform.h"
#include "crash/crash.h"

#define SRAM_LO		0x00010000u
#define SRAM_HI		0x00058000u
#define MODEM_SRAM_LO	0x18000000u
#define MODEM_SRAM_HI	0x18008000u
#define XIP_LO		0x03000000u
#define XIP_HI		0x04000000u	// the flash controller's window; the chips fitted are 1 MB or more

extern char __ramfunc_start[], __ramfunc_end[];

crash_record_t *hal_crash_store(void)
{
	return (crash_record_t *)HRC_CRASH_STORE;
}

uint8_t hal_crash_arch(void)
{
	return CRASH_ARCH_CK803;
}

bool hal_crash_ram_addr(uint32_t a)
{
	return (a >= SRAM_LO && a < SRAM_HI) || (a >= MODEM_SRAM_LO && a < MODEM_SRAM_HI);
}

bool hal_crash_code_addr(uint32_t a)
{
	return (a >= XIP_LO && a < XIP_HI) ||
		(a >= (uint32_t)(uintptr_t)__ramfunc_start && a < (uint32_t)(uintptr_t)__ramfunc_end);
}

void hal_crash_restart(void)
{
	hrc_soft_reset();
}

void hrc_crash_fault(const uint32_t *frame, uint32_t vector)
{
	crash_record_t r;
	memset(&r, 0, sizeof r);
	r.arch = CRASH_ARCH_CK803;
	r.kind = CRASH_KIND_FAULT;
	r.cause = vector;	// 1 misaligned access, 2 access error, 3 divide by zero, 4 illegal instruction, ...
	for (int k = 0; k < 14; k++)
		r.r[k] = frame[k];
	r.lr = frame[14];
	r.psr = frame[15];
	r.pc = frame[16];
	r.sp = (uint32_t)(uintptr_t)frame + 68u;	// before the entry's frame
	uint32_t psr;
	__asm__ volatile ("mfcr %0, psr" : "=r"(psr));
	r.status[0] = psr;
	if ((r.psr >> 16) & 0xFFu)
		r.flags |= CRASH_F_ISR;	// a vector number in EPSR: an interrupt (or another exception) was being handled
	crash_commit(&r);
}

#endif
