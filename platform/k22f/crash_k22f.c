// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log on the K22F (hal_crash.h, docs/spec/crash-log.md 2.1): the same Cortex-M4 capture as the STM32F4's.
// The fault entry takes the exception frame from the stack the CPU pushed it on (MSP or a task's PSP), moves to a stack
// of its own and fills the record with the frame, r4-r11 and the fault status registers. The K22F has no backup RAM,
// so the record lives in a RAM section that start-up doesn't clear (.crash_noinit, boards/<radio>/<radio>.ld): a
// system reset keeps SRAM, but the radio's bootloader runs in between and might use that RAM (unverified). The
// record's CRC tells: an overwritten record is ignored at the next start, never shown wrong.
#include "config.h"

#if CONFIG_CRASH_LOG
#include <string.h>
#include "platform.h"
#include "board.h"
#include "crash/crash.h"

#define FAULT_STACK_WORDS	256
#define FAULT_STACK_BYTES	1024		// the same, for the entry's assembly
#define STR_(x)			#x
#define STR(x)			STR_(x)
#define FLASH_LO		0x00004000u	// the application (the bootloader below is never code of ours)
#define FLASH_HI		K22F_FLASH_SIZE
#define FRAME_FPU_BIT		0x10u		// EXC_RETURN bit 4 clear: the frame has the FPU registers too
#define XPSR_ALIGN_BIT		0x200u		// stacked xPSR bit 9: the CPU padded the frame by a word

uint32_t k22f_fault_stack[FAULT_STACK_WORDS] __attribute__((used, aligned(8)));	// only the entry's assembly uses it
_Static_assert(sizeof k22f_fault_stack == FAULT_STACK_BYTES, "the entry's stack top");

static crash_record_t record __attribute__((section(".crash_noinit"), used));

crash_record_t *hal_crash_store(void)
{
	return &record;
}

uint8_t hal_crash_arch(void)
{
	return CRASH_ARCH_CM4;
}

bool hal_crash_ram_addr(uint32_t a)
{
	return a >= K22F_SRAM_L && a < K22F_SRAM_END;
}

bool hal_crash_code_addr(uint32_t a)
{
	return a >= FLASH_LO && a < FLASH_HI;
}

// The instruction before pc ends a kernel wait: the yield's ISB (portmacro.h), or the MSR BASEPRI of a critical
// section's exit, where a yield the section held back is taken (the same port as the STM32F4's)
static bool after_yield(uint32_t pc)
{
	pc &= ~1u;
	if (!hal_crash_code_addr(pc - 4u))
		return false;
	const volatile uint16_t *h = (const volatile uint16_t *)(uintptr_t)(pc - 4u);
	return (h[0] == 0xF3BFu && h[1] == 0x8F6Fu) || ((h[0] & 0xFFF0u) == 0xF380u && h[1] == 0x8811u);
}

uint32_t hal_crash_task_where(const void *task)
{
	// pxTopOfStack: what the PendSV handler pushed, low to high r4-r11 and EXC_RETURN, s16-s31 if the task used the
	// FPU, then the CPU's frame (r0-r3, r12, lr, pc, xPSR, with s0-s15 and FPSCR after it for the FPU)
	const uint32_t f = *(const uint32_t *)task;
	if ((f & 3u) || !hal_crash_ram_addr(f) || !hal_crash_ram_addr(f + 35u))
		return 0;
	const uint32_t exc_return = ((const uint32_t *)(uintptr_t)f)[8];
	const uint32_t hw = f + 36u + ((exc_return & FRAME_FPU_BIT) ? 0u : 64u);
	if (!hal_crash_ram_addr(hw + 31u))
		return 0;
	const uint32_t *frame = (const uint32_t *)(uintptr_t)hw;
	const uint32_t pc = frame[6];
	if (!after_yield(pc))
		return pc & ~1u;	// an interrupt took it while ready
	const uint32_t above = hw + ((exc_return & FRAME_FPU_BIT) ? 32u : 104u) + ((frame[7] & XPSR_ALIGN_BIT) ? 4u : 0u);
	const uint32_t ret = crash_stack_return(above, CRASH_TASK_SCAN_WORDS);
	return ((ret != 0 ? ret : pc) & ~1u) | 1u;
}

// The restart cause's words (hal_crash.h), in the same kept RAM as the record
static crash_boot_t boot_words __attribute__((section(".crash_noinit"), used));

crash_boot_t *hal_crash_boot(void)
{
	return &boot_words;
}

// RCM_SRS0 / SRS1 (reference manual chapter 17): the sources of the last reset, kept until the next one, so nothing
// to clear. A power-on also sets the low voltage flag, so power before brown-out. The bootloader runs first and is
// not known to add to them
uint8_t hal_crash_reset_flags(uint32_t *raw)
{
	const uint32_t srs0 = RCM_SRS0, srs1 = RCM_SRS1;
	*raw = srs0 | (srs1 << 8);
	if (srs0 & 0x20u)		// WDOG
		return CRASH_RESET_WATCHDOG;
	if (srs1 & 0x04u)		// SW (SYSRESETREQ)
		return CRASH_RESET_SOFTWARE;
	if (srs0 & 0x80u)		// POR
		return CRASH_RESET_POWER;
	if (srs0 & 0x02u)		// LVD
		return CRASH_RESET_BROWNOUT;
	if (srs0 & 0x40u)		// PIN
		return CRASH_RESET_PIN;
	return CRASH_RESET_UNKNOWN;
}

void hal_crash_restart(void)
{
	k22f_reset();
}

// From the entry below: frame = the pushed r0-r3, r12, lr, pc, xPSR; hi = r4-r11 as they were
void k22f_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi) __attribute__((used, noreturn));
void k22f_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi)
{
	board_rf_safe();	// transmitter off first (docs/spec/crash-log.md 8)
	crash_record_t r;
	memset(&r, 0, sizeof r);
	r.arch = CRASH_ARCH_CM4;
	r.kind = CRASH_KIND_FAULT;
	r.cause = __get_IPSR() & 0x1FFu;	// 3 HardFault; 4-6 when the configurable faults are enabled
	uint32_t f = (uint32_t)(uintptr_t)frame;
	if ((f & 3u) == 0 && hal_crash_ram_addr(f) && hal_crash_ram_addr(f + 31u)) {
		for (int k = 0; k < 4; k++)
			r.r[k] = frame[k];
		r.r[12] = frame[4];
		r.lr = frame[5];
		r.pc = frame[6];
		r.psr = frame[7];
		r.sp = f + ((exc_return & FRAME_FPU_BIT) ? 32u : 104u) + ((r.psr & XPSR_ALIGN_BIT) ? 4u : 0u);
		if (r.psr & 0x1FFu)
			r.flags |= CRASH_F_ISR;	// an exception number in the stacked IPSR: an interrupt handler was running
	} else {
		r.sp = f;			// the CPU couldn't push the frame there (a stacking error: see CFSR)
	}
	for (int k = 0; k < 8; k++)
		r.r[4 + k] = hi[k];
	r.r[13] = exc_return;
	r.status[0] = SCB->CFSR;
	r.status[1] = SCB->HFSR;
	r.status[2] = SCB->MMFAR;
	r.status[3] = SCB->BFAR;
	crash_commit(&r);
}

// Every fault. Interrupts off, then the stack the frame is on (EXC_RETURN bit 2: PSP, a task), our own stack, and
// r4-r11 pushed there for the C part
__attribute__((naked, used)) void HardFault_Handler(void)
{
	__asm volatile(
		"cpsid	i\n"
		"tst	lr, #4\n"
		"ite	eq\n"
		"mrseq	r0, msp\n"
		"mrsne	r0, psp\n"
		"mov	r1, lr\n"
		"movw	r2, #:lower16:k22f_fault_stack\n"
		"movt	r2, #:upper16:k22f_fault_stack\n"
		"add	r2, r2, #" STR(FAULT_STACK_BYTES) "\n"
		"mov	sp, r2\n"
		"push	{r4-r11}\n"
		"mov	r2, sp\n"
		"b	k22f_fault\n");
}

// Off unless SCB->SHCSR enables them (they then escalate no more): the same capture
void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

#endif
