// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Vector table and PIC (docs/spec/hardware-dm32.md 4). The CPU takes vector = PIC source + 32 from the table at VBR.
// Every user vector goes to the common entry (freertos/portasm.S), which acks the PIC and calls hrc_irq_table[source];
// the CPU exceptions go to hrc_fault_entry. The table is in RAM, 1 KB aligned (VBR), filled at start-up.
#include "config.h"
#include "platform.h"
#include "hal_watchdog.h"
#if CONFIG_FAULT_TRACE
#include "crash/crash.h"
#endif
#if CONFIG_CRASH_LOG
#include "hal_crash.h"
#if CONFIG_WATCHDOG
#include "FreeRTOS.h"
#include "task.h"
#endif
#endif

#define VECTORS		128u	// 32 exceptions + 64 PIC sources, padded

extern void hrc_irq_entry(void);
extern void hrc_fault_entry(void);

static uint32_t vectors[VECTORS] __attribute__((section(".vectors"), aligned(1024)));
hrc_irq_fn hrc_irq_table[PIC_SOURCES];
volatile uint32_t hrc_fault_info[3];	// vector, EPC, EPSR of the last CPU exception (written by hrc_fault)

// The fault path's own stack (portasm.S hrc_fault_entry): the faulting one may be the problem, and the interrupt
// stack holds the frame when the fault was in a handler. The crash capture needs the room for its record
#define FAULT_STACK_WORDS	(CONFIG_CRASH_LOG ? 256u : CONFIG_FAULT_TRACE ? 64u : 32u)
static uint32_t fault_stack[FAULT_STACK_WORDS] __attribute__((aligned(8)));
uint32_t *hrc_fault_sp = &fault_stack[FAULT_STACK_WORDS];	// in RAM (.data): the entry reads it without the flash

void hrc_irq_enable(uint8_t source, bool on)
{
	if (source >= PIC_SOURCES) {
		return;
	}
	uint32_t psr = hrc_irq_save();
	volatile uint32_t *mask = source < 32 ? &PIC_MASK : &PIC_MASK_1;
	uint32_t bit = 1u << (source & 31u);
	*mask = on ? (*mask & ~bit) : (*mask | bit);
	hrc_irq_restore(psr);
}

// A source with no handler fired: mask it so it can't storm. Counted, with the last source, for the debug link
// (hal_system_t.irq_unhandled): a source that went quiet this way stays quiet until it is enabled again
static void unhandled(void);
static uint32_t unhandled_count;
static uint8_t unhandled_source;

static uint8_t current_source(void)
{
	uint32_t psr;
	__asm__ volatile ("mfcr %0, psr" : "=r"(psr));
	return (uint8_t)(((psr >> 16) & 0xFFu) - 32u);
}

static void unhandled(void)
{
	uint8_t source = current_source();
	unhandled_count++;
	unhandled_source = source;
	hrc_irq_enable(source, false);
}

void hrc_irq_unhandled(uint32_t *count, uint32_t *last_source)
{
	uint32_t psr = hrc_irq_save();	// a pair from the same interrupt
	*count = unhandled_count;
	*last_source = unhandled_source;
	hrc_irq_restore(psr);
}

void hrc_irq_register(uint8_t source, hrc_irq_fn fn)
{
	if (source >= PIC_SOURCES) {
		return;
	}
	hrc_irq_enable(source, false);
	hrc_irq_table[source] = fn != NULL ? fn : unhandled;
}

uint32_t hrc_modem_irqs_mask(void)
{
	uint32_t psr = hrc_irq_save();
	uint32_t saved = (PIC_MASK & 0xE0000000u) | (PIC_MASK_1 & 0x1u);
	PIC_MASK |= 0xE0000000u;	// sources 29, 30, 31
	PIC_MASK_1 |= 0x1u;		// source 32
	hrc_irq_restore(psr);
	return saved;
}

void hrc_modem_irqs_restore(uint32_t saved)
{
	uint32_t psr = hrc_irq_save();
	PIC_MASK = (PIC_MASK & ~0xE0000000u) | (saved & 0xE0000000u);
	PIC_MASK_1 = (PIC_MASK_1 & ~0x1u) | (saved & 0x1u);
	hrc_irq_restore(psr);
}

#if CONFIG_FAULT_TRACE
// The fault trace (CONFIG_FAULT_TRACE; core/crash/crash_trace.c, docs/spec/crash-log.md 2.3): the registers the entry
// saved (r0-r13, r15 = lr, EPSR, EPC) on the host UART, one line a second, then the restart. Timed on the free-running
// clock timer, which counts with interrupts off. The three seconds fit the watchdog's timeout (watchdog.c), which the
// trace restarts first: the idle task may not have fed it for a while before the fault
#define FAULT_TRACE_GAP		HRC_APB_HZ		// timer counts between lines: a second
#define FAULT_TRACE_DRAIN	(HRC_APB_HZ / 20u)	// after the last: the UART's FIFO empties before the restart

static void fault_trace(const uint32_t *frame, uint32_t vector)
{
#if CONFIG_WATCHDOG
	hal_watchdog_feed();
#endif
	crash_trace_t t = {
		.vector = vector, .pc = frame[16], .psr = frame[15], .lr = frame[14],
		.sp = (uint32_t)(uintptr_t)frame + 68u, .r = frame, .nr = 14,
	};
	for (uint8_t n = 1; n <= CRASH_TRACE_LINES; n++) {
		crash_trace_line(&t, n, hrc_host_uart_put_polled);
		uint32_t wait = n < CRASH_TRACE_LINES ? FAULT_TRACE_GAP : FAULT_TRACE_DRAIN;
		uint32_t t0 = hrc_cycles();
		while (hrc_cycles() - t0 < wait) {
		}
	}
}
#endif

// Last stop of a CPU exception (fault), from hrc_fault_entry: frame = r0..r13, r15, EPSR, EPC as they were, on the
// faulting stack. The transmitter goes off first, then the fault trace (CONFIG_FAULT_TRACE); with the crash log the
// record is kept and the radio restarts; without it the radio restarts too, rather than stay crashed (the debugger
// still sees hrc_fault_info if the restart fails)
void hrc_fault(const uint32_t *frame, uint32_t vector) __attribute__((used, noreturn));
void hrc_fault(const uint32_t *frame, uint32_t vector)
{
	__asm__ volatile ("psrclr ie");
	hrc_rf_safe();	// first: whatever follows (the capture, the restart, a hang) must not keep the PA keyed
	hrc_fault_info[0] = vector;
	hrc_fault_info[1] = frame[16];
	hrc_fault_info[2] = frame[15];
#if CONFIG_FAULT_TRACE
	fault_trace(frame, vector);
#endif
#if CONFIG_CRASH_LOG
	hrc_crash_frame(frame, CRASH_KIND_FAULT, vector);	// keeps the record and restarts
#endif
	hrc_soft_reset();	// without the crash log: restart too, rather than stay crashed
}

#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
// The hang check (core/app/watchdog.c), from the tick interrupt. hrc_irq_entry saved the interrupted task's registers
// in a frame like hrc_fault's and wrote its address to the task's TCB, whose first word is the saved stack pointer
// (pxTopOfStack). Interrupts don't nest, so that task is the one that kept the idle task out. The fault trace marks a
// hang with vector 0
void hal_crash_hang(uint32_t ms)
{
	__asm__ volatile ("psrclr ie");
	hrc_rf_safe();
	const uint32_t *frame = *(const uint32_t *const *)xTaskGetCurrentTaskHandle();
#if CONFIG_FAULT_TRACE
	fault_trace(frame, 0);
#endif
	hrc_crash_frame(frame, CRASH_KIND_HANG, ms);
}
#endif

void hrc_irq_init(void)
{
	// PIC: everything masked, level triggered, high active, old requests cleared (hardware-dm32.md 4.1; both banks,
	// unlike the old port, which wrote PIC_MASK twice)
	PIC_MASK = 0xFFFFFFFFu;
	PIC_MASK_1 = 0xFFFFFFFFu;
	PIC_MODE = 0;
	PIC_MODE_1 = 0;
	PIC_PO = 0xFFFFFFFFu;
	PIC_PO_1 = 0xFFFFFFFFu;
	PIC_INT_ST = 0xFFFFFFFFu;
	PIC_INT_ST_1 = 0xFFFFFFFFu;

	for (uint32_t i = 0; i < PIC_SOURCES; i++) {
		hrc_irq_table[i] = unhandled;
	}
	for (uint32_t v = 0; v < VECTORS; v++) {
		vectors[v] = v < 32 ? (uint32_t)hrc_fault_entry : (uint32_t)hrc_irq_entry;
	}
	hrc_barrier();
	__asm__ volatile ("mtcr %0, vbr" : : "r"((uint32_t)vectors) : "memory");
}
