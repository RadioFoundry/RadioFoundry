// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The CCM: 64 KB of zero wait state RAM on the CPU's data bus, off the bus the audio, ADC and display DMA use. No DMA
 * reaches it, so nothing there may be a DMA buffer: the DMA buffers are statics in main RAM. In link order
 * (common.ld):
 *
 *   .ccmdata  RFOUNDRY_CPU_DATA, hot tables, copied from flash
 *   .ccmbss   RFOUNDRY_CPU_RAM, zeroed
 *   .ccmram   RFOUNDRY_CPU_RAM_NOINIT, left as it is
 *   heap      the rest: the RTOS heap, and with it every task stack (so nothing on a stack or from pvPortMalloc may be
 *             a DMA buffer either)
 *
 * The heap grows and shrinks with the statics above it, so the CCM stays full; the link fails when it would get less
 * than _Min_Task_Heap.
 */

#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"

extern uint32_t _sccmdata[], _eccmdata[], _siccmdata[], _sccmbss[], _eccmbss[];
extern uint8_t _sccmheap[], _eccmheap[];

// From __libc_init_array, before main and before any interrupt (the CCM clock is on from reset), and before anything
// allocates
static void ccm_init(void)
{
	const uint32_t *src = _siccmdata;

	for (uint32_t *p = _sccmdata; p < _eccmdata; p++)
		*p = *src++;
	for (uint32_t *p = _sccmbss; p < _eccmbss; p++)
		*p = 0;

	HeapRegion_t heap[] = {
		{ _sccmheap, (size_t)(_eccmheap - _sccmheap) },
		{ NULL, 0 },
	};
	vPortDefineHeapRegions(heap);
}
__attribute__((section(".preinit_array"), used)) static void (*const ccm_init_entry)(void) = ccm_init;
