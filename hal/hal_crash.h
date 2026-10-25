// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Crash capture (CONFIG_CRASH_LOG, docs/spec/crash-log.md): the record a platform's fault handler fills, and the few
// things the platform provides for it. The record is kept in memory a reset leaves alone, so the next start can show
// and send it. The layout is fixed (CRASH_VERSION): tools/crash_decode.py reads it.
#ifndef HAL_CRASH_H
#define HAL_CRASH_H

#include <stdbool.h>
#include <stdint.h>

#define CRASH_MAGIC		0x48535243u	// "CRSH"
#define CRASH_VERSION		2
#define CRASH_STACK_WORDS	16
#define CRASH_TASK_LEN		8
#define CRASH_TASKS		8	// other tasks in the snapshot: the record then fills the HR_C7000's 256-byte store
#define CRASH_TAG_LEN		4	// the first letters of each one's name

enum { CRASH_ARCH_CM4 = 1, CRASH_ARCH_CK803 = 2 };

enum {
	CRASH_KIND_FAULT = 1,	// a CPU exception: cause is the exception number
	CRASH_KIND_STACK = 2,	// FreeRTOS found a task's stack overflowed: task names it
	CRASH_KIND_HEAP = 3,	// FreeRTOS ran out of heap
	CRASH_KIND_ASSERT = 4,	// configASSERT failed: cause is the line, pc the caller
	CRASH_KIND_HANG = 5,	// the watchdog's hang check: cause is the ms since the last feed, pc the interrupted task's
};

// crash_record_t.flags
#define CRASH_F_ISR		0x01u	// it happened in an interrupt handler (task is then empty)
#define CRASH_F_DIRTY		0x02u	// the build had local changes (its commit isn't the whole story)
#define CRASH_F_STACK		0x04u	// stack[] holds the words at sp (sp pointed into RAM)
#define CRASH_F_SEEN		0x08u	// shown on the screen or dismissed: not news any more
#define CRASH_F_SENT		0x10u	// the report went out over the air

// Fixed-width members, no padding (256 bytes)
// One task other than the running one, as the crash found it (crash_record_t.tasks)
typedef struct {
	char name[CRASH_TAG_LEN];	// the first letters of its name, NUL padded, not always terminated
	uint32_t where;			// bit 0 set: it was waiting in the kernel, and the rest is the return address into
					// the code that called the wait. Clear: an interrupt took it while ready, the rest
					// is its pc. 0: its saved context couldn't be read
} crash_task_t;

typedef struct {
	uint32_t magic;			// CRASH_MAGIC
	uint16_t version;		// CRASH_VERSION
	uint16_t size;			// sizeof(crash_record_t)
	uint8_t arch;			// CRASH_ARCH_*
	uint8_t kind;			// CRASH_KIND_*
	uint8_t flags;			// CRASH_F_*
	uint8_t count;			// crashes in a row of this build that nobody looked at (1 = the first)
	uint32_t cause;			// FAULT: CM4 the active exception (IPSR), CK803 the vector; ASSERT: the line
	uint32_t pc;			// where it happened
	uint32_t lr;			// the return address at the time
	uint32_t sp;			// the stack pointer before the exception
	uint32_t psr;			// CM4 the stacked xPSR, CK803 EPSR
	uint32_t r[14];			// r0..r13 (CM4: r0..r12, r[13] = EXC_RETURN)
	uint32_t status[4];		// CM4: CFSR, HFSR, MMFAR, BFAR; CK803: PSR in the handler, the rest 0
	uint32_t uptime_ms;		// time since start
	uint32_t build;			// the first 8 hex digits of the firmware's commit
	char task[CRASH_TASK_LEN];	// the running task's name, NUL padded
	uint32_t stack[CRASH_STACK_WORDS];
	uint8_t ntasks;			// entries in tasks
	uint8_t more_tasks;		// other tasks that didn't fit
	uint16_t reserved;		// 0
	crash_task_t tasks[CRASH_TASKS];	// in the task table's order (core/app/task_stack.c), not the idle task
	uint32_t crc;			// CRC-32 of everything before it
} crash_record_t;

_Static_assert(sizeof(crash_record_t) == 256, "the record's layout (docs/spec/crash-log.md 3) and the HR_C7000 store");

// Why the radio restarted (docs/spec/crash-log.md 1.3), also when there is no record: a few words next to the record,
// in the same kept memory. The start-up stamps them, the tick keeps the uptime in them, and every restart software
// asks for marks them first (hal_crash_soft_mark). What the next start finds there, with the chip's own reset flags
// where it has them, is the cause
#define CRASH_BOOT_MAGIC	0x544F4F42u	// "BOOT"
#define CRASH_BOOT_SOFT		0x54464F53u	// "SOFT"

typedef struct {
	uint32_t magic;			// CRASH_BOOT_MAGIC: the memory has kept its contents since the last start
	uint32_t soft;			// CRASH_BOOT_SOFT: software asked for the reset that followed
	uint32_t uptime_ms;		// how long the last run lasted, to the tick (the hang check's tick writes it)
	uint32_t warm;			// starts since the memory last lost its contents
} crash_boot_t;

enum {
	CRASH_RESET_UNKNOWN = 0,
	CRASH_RESET_POWER = 1,		// the supply came up: the kept memory was lost, or the chip's power-on flag
	CRASH_RESET_SOFTWARE = 2,	// a reboot, a crash's restart, the boot stub's reset
	CRASH_RESET_WATCHDOG = 3,	// the chip's watchdog flag
	CRASH_RESET_BROWNOUT = 4,	// the chip's brown-out flag
	CRASH_RESET_PIN = 5,		// the chip's reset pin flag and no other
	CRASH_RESET_WARM = 6,		// memory kept, not asked for, and a chip without reset flags: the watchdog, the reset
					// pin, or a supply dip too short to lose memory
};

// Provided by the platform:

// The record's home, in memory a reset doesn't clear and the bootloader doesn't use (STM32F4: backup SRAM; HR_C7000:
// the top of the modem's SRAM). The first call turns on its access. NULL: the platform has none
crash_record_t *hal_crash_store(void);
// CRASH_ARCH_*
uint8_t hal_crash_arch(void);
// Readable RAM (the stack copy) / code (the report's return address candidates)
bool hal_crash_ram_addr(uint32_t addr);
bool hal_crash_code_addr(uint32_t addr);
// A task that isn't running (its handle): crash_task_t.where from the context its last switch saved, which the
// TCB's first word points at. 0 when that can't be read
uint32_t hal_crash_task_where(const void *task);
// Restart the radio after a capture: interrupts are off; does not return
void hal_crash_restart(void) __attribute__((noreturn));
// The hang check (core/app/watchdog.c), from the RTOS tick interrupt: the transmitter off, the interrupted task's
// registers into a CRASH_KIND_HANG record (cause: ms, the time since the last feed), then crash_commit. Does not return
void hal_crash_hang(uint32_t ms) __attribute__((noreturn));
// The restart cause's words, next to the record. The first call turns on its access, as hal_crash_store's does
crash_boot_t *hal_crash_boot(void);
// The chip's reset flags as a CRASH_RESET_* (CRASH_RESET_UNKNOWN on a chip without them), *raw the register as read.
// Clears them for the next start: call it once, at start-up
uint8_t hal_crash_reset_flags(uint32_t *raw);

// Every restart software asks for calls this first, so the next start doesn't take it for the watchdog's
static inline void hal_crash_soft_mark(void)
{
	hal_crash_boot()->soft = CRASH_BOOT_SOFT;
}

#endif
