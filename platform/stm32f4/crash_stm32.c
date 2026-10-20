// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash log on the STM32F4 (hal_crash.h, docs/spec/crash-log.md 2.1). The fault entry takes the exception frame
// from the stack the CPU pushed it on (MSP or a task's PSP), moves to a stack of its own (the old one may be the
// problem) and fills the record with the frame, r4-r11 and the fault status registers. The record lives in the 4 KB
// backup SRAM: a system reset leaves the backup domain alone, and the stock bootloader, which runs in main RAM before
// the application, doesn't look at it (hardware-dm1701.md 15.4: the RTC backup register the ROM DFU entry uses
// survives it the same way). It is lost when the supply goes (no backup battery confirmed on these boards).
#include "config.h"

#if CONFIG_CRASH_LOG
#include <string.h>
#include "platform.h"
#include "crash/crash.h"

#define FAULT_STACK_WORDS	256
#define FAULT_STACK_BYTES	1024		// the same, for the entry's assembly
#define STR_(x)			#x
#define STR(x)			STR_(x)
#define FLASH_LO		0x08000000u
#define FLASH_HI		0x08100000u	// 1 MB (STM32F405xG)
#define SRAM_LO			0x20000000u
#define SRAM_HI			0x20020000u
#define CCM_LO			0x10000000u
#define CCM_HI			0x10010000u
#define FRAME_FPU_BIT		0x10u		// EXC_RETURN bit 4 clear: the frame has the FPU registers too
#define XPSR_ALIGN_BIT		0x200u		// stacked xPSR bit 9: the CPU padded the frame by a word

uint32_t stm32_fault_stack[FAULT_STACK_WORDS] __attribute__((used, aligned(8)));	// only the entry's assembly uses it
_Static_assert(sizeof stm32_fault_stack == FAULT_STACK_BYTES, "the entry's stack top");

crash_record_t *hal_crash_store(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	(void)RCC->APB1ENR;
	PWR->CR |= PWR_CR_DBP;			// backup domain writable (sysboot.c sets the same bit)
	RCC->AHB1ENR |= RCC_AHB1ENR_BKPSRAMEN;
	(void)RCC->AHB1ENR;
	return (crash_record_t *)BKPSRAM_BASE;
}

uint8_t hal_crash_arch(void)
{
	return CRASH_ARCH_CM4;
}

bool hal_crash_ram_addr(uint32_t a)
{
	return (a >= SRAM_LO && a < SRAM_HI) || (a >= CCM_LO && a < CCM_HI);
}

bool hal_crash_code_addr(uint32_t a)
{
	return a >= FLASH_LO && a < FLASH_HI;
}

void hal_crash_restart(void)
{
	stm32_system.reboot(HAL_REBOOT_NORMAL);	// sysboot.c: interrupts off, NVIC_SystemReset (one copy of it in flash)
	for (;;) {
	}
}

// From the entry below: frame = the pushed r0-r3, r12, lr, pc, xPSR; hi = r4-r11 as they were
void stm32_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi) __attribute__((used, noreturn));
void stm32_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi)
{
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
		"movw	r2, #:lower16:stm32_fault_stack\n"
		"movt	r2, #:upper16:stm32_fault_stack\n"
		"add	r2, r2, #" STR(FAULT_STACK_BYTES) "\n"
		"mov	sp, r2\n"
		"push	{r4-r11}\n"
		"mov	r2, sp\n"
		"b	stm32_fault\n");
}

// Off unless SCB->SHCSR enables them (they then escalate no more): the same capture
void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

#endif
