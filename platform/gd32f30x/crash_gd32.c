// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log on the GD32F30x (hal_crash.h, docs/spec/crash-log.md 2.4). The core is a Cortex-M4F, so the fault
// entry, the hang check and the task snapshot are the STM32F4's (platform/stm32f4/crash_stm32.c), copied rather than
// shared so neither platform's build reaches into the other. What differs is the store: the GD32F30x has no backup
// SRAM, and its backup registers (84 bytes, 16 bits each) don't hold the record. It lives at the top of main SRAM
// instead, in the .kept section (common.ld), which the start-up neither copies nor clears and the main stack starts
// below. Whether the vendor bootloader, which runs before the application, leaves those bytes alone is unverified:
// the record's CRC and the restart cause's magic make a store it overwrote read as no record and a power-on.
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
#define FRAME_FPU_BIT		0x10u		// EXC_RETURN bit 4 clear: the frame has the FPU registers too
#define XPSR_ALIGN_BIT		0x200u		// stacked xPSR bit 9: the CPU padded the frame by a word

// The record, then the restart cause's words at the very top: if something before the application does use the top
// of SRAM (a stack coming down from it), the words go first and the next start reads a power-on, which says so
typedef struct {
	crash_record_t record;
	crash_boot_t boot;
} kept_t;

static kept_t kept __attribute__((section(".kept"), used));
_Static_assert(sizeof(kept_t) == 272, "the .kept region of each board's memory.ld (common.ld)");

extern uint32_t _etext[];			// common.ld: the end of the code

uint32_t gd32_fault_stack[FAULT_STACK_WORDS] __attribute__((used, aligned(8)));	// only the entry's assembly uses it
_Static_assert(sizeof gd32_fault_stack == FAULT_STACK_BYTES, "the entry's stack top");

#if CONFIG_STUB_ENTRY
#include "../../stub/gd32f30x/gd32stub_header.h"
_Static_assert(GD32STUB_SOFT_MARK == CRASH_BOOT_SOFT, "the stub's RESET mark (gd32stub_header.h)");
#endif

crash_record_t *hal_crash_store(void)
{
	return &kept.record;	// plain SRAM: nothing to turn on
}

crash_boot_t *hal_crash_boot(void)
{
	return &kept.boot;
}

uint8_t hal_crash_arch(void)
{
	return CRASH_ARCH_CM4;
}

// All of SRAM, the kept area at its top included (no CCM on this family)
bool hal_crash_ram_addr(uint32_t a)
{
	return a >= GD32_SRAM_BASE && a < (uint32_t)(uintptr_t)(&kept + 1);
}

// The flash up to the end of our code: the vendor bootloader below the image is readable too, and the parts' flash
// sizes differ, so the linker's end of .text is the bound that never reads past the chip
bool hal_crash_code_addr(uint32_t a)
{
	return a >= GD32_FLASH_BASE && a < (uint32_t)(uintptr_t)_etext;
}

// The instruction before pc ends a kernel wait: the yield's ISB (portmacro.h), or the MSR BASEPRI of a critical
// section's exit, where a yield the section held back is taken
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
	// pxTopOfStack: what the PendSV handler (port.c) pushed, low to high r4-r11 and EXC_RETURN, s16-s31 if the task
	// used the FPU, then the CPU's frame (r0-r3, r12, lr, pc, xPSR, with s0-s15 and FPSCR after it for the FPU)
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

// RCU_RSTSCK (GD32F30x user manual, RCU). As on the STM32 parts, every reset drives the NRST pin, so EPRSTF alone is
// the pin. There is no brown-out flag: a power-down reset sets PORRSTF like a power-on. LPRSTF (a reset on entering
// standby or deep-sleep, by option byte) is never set up here and counts as none. The vendor bootloader runs first
// and is not known to clear or add to them (unverified)
uint8_t hal_crash_reset_flags(uint32_t *raw)
{
	const uint32_t f = RCU_RSTSCK;
	*raw = f;
	RCU_RSTSCK = f | RCU_RSTSCK_RSTFC;
	if (f & (RCU_RSTSCK_FWDGTRSTF | RCU_RSTSCK_WWDGTRSTF))
		return CRASH_RESET_WATCHDOG;
	if (f & RCU_RSTSCK_SWRSTF)
		return CRASH_RESET_SOFTWARE;
	if (f & RCU_RSTSCK_PORRSTF)
		return CRASH_RESET_POWER;
	if (f & RCU_RSTSCK_EPRSTF)
		return CRASH_RESET_PIN;
	return CRASH_RESET_UNKNOWN;
}

// Every caller has turned the transmitter off just before (crash_commit, the soft capture), so this is the reboot's
// mark and reset without its board_rf_safe: the restart then adds no frames under a capture on the idle task's small
// stack, which an assertion in the kernel runs on (make stack-check)
void hal_crash_restart(void)
{
	__disable_irq();
	hal_crash_soft_mark();
	NVIC_SystemReset();
}

// From the entry below: frame = the pushed r0-r3, r12, lr, pc, xPSR; hi = r4-r11 as they were
void gd32_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi) __attribute__((used, noreturn));
void gd32_fault(const uint32_t *frame, uint32_t exc_return, const uint32_t *hi)
{
	board_rf_safe();	// first: the capture and the restart take a while, and the start-up may not finish
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

#if CONFIG_WATCHDOG
// The hang check (core/app/watchdog.c), from SysTick (time.c), which runs at the kernel's priority, the lowest: when
// it runs no other handler is active, so it interrupted a task, whose frame the CPU pushed on the PSP. Only that frame
// is kept, as on the STM32F4: r4-r11 belong to the handler's callers by now, and without EXC_RETURN the frame's size
// is unknown, so sp is the frame's address and stack[] starts with the frame. The record is built in the fault stack,
// which runs nothing until a fault, rather than in 256 bytes of its own (RAM is short on these radios) or on the main
// stack, which every interrupt shares. A fault during the capture takes that stack over and keeps its own record
_Static_assert(sizeof(crash_record_t) <= sizeof gd32_fault_stack / 2, "the hang record leaves the fault entry room");

void hal_crash_hang(uint32_t ms)
{
	crash_record_t *const r = (crash_record_t *)gd32_fault_stack;	// the bottom: the entry pushes from the top
	__disable_irq();
	board_rf_safe();
	memset(r, 0, sizeof *r);
	r->arch = CRASH_ARCH_CM4;
	r->kind = CRASH_KIND_HANG;
	r->cause = ms;
	const uint32_t *frame = (const uint32_t *)__get_PSP();
	uint32_t f = (uint32_t)(uintptr_t)frame;
	if ((f & 3u) == 0 && hal_crash_ram_addr(f) && hal_crash_ram_addr(f + 31u)) {
		for (int k = 0; k < 4; k++)
			r->r[k] = frame[k];
		r->r[12] = frame[4];
		r->lr = frame[5];
		r->pc = frame[6];
		r->psr = frame[7];
	}
	r->sp = f;
	r->status[0] = SCB->CFSR;
	r->status[1] = SCB->HFSR;
	crash_commit(r);
}
#endif

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
		"movw	r2, #:lower16:gd32_fault_stack\n"
		"movt	r2, #:upper16:gd32_fault_stack\n"
		"add	r2, r2, #" STR(FAULT_STACK_BYTES) "\n"
		"mov	sp, r2\n"
		"push	{r4-r11}\n"
		"mov	r2, sp\n"
		"b	gd32_fault\n");
}

// Off unless SCB->SHCSR enables them (they then escalate no more): the same capture. The NMI too (the clock monitor,
// if it is ever turned on): the start-up's default handler would spin with the transmitter as it was
void NMI_Handler(void) __attribute__((alias("HardFault_Handler")));
void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

#endif
