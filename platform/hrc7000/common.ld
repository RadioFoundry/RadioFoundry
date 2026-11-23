/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* HR_C7000 (CK803S) application layout with a codec slot, INCLUDEd by each board's memory.ld after its codec symbols
 * and its MEMORY (docs/spec/codec-placement.md).
 *
 * Program flash (XIP at 0x03000000): the factory loader below the image (never written), the application from
 * ORIGIN(FLASH) (board.mk APP_BASE, after the first stage when there is one), then the codec slot __codec_slot ..
 * __codec_slot_end: the stock codec code and tables are merged there on the user's machine and never linked into.
 *
 * SRAM: one region for RadioFoundry from ORIGIN(RAM) (the vector table, .ramfunc: interrupt entry and the modem slot engine, the
 * zeroed .ram_lo state the core marks RFOUNDRY_CPU_RAM and the interrupt stack, then data, bss, and the RTOS heap last
 * with the start-up stack in its top), and above it the codec window __codec_run_lo .. __codec_run_hi: the slot's code is copied there at
 * boot and its address words moved. No DMA on these radios, so any of it can hold any buffer. The modem SRAM window
 * the codec's tables are copied to, __codec_tab_lo .. __codec_tab_hi, is reserved by not being in the script at all.
 *
 * Modem SRAM (0x18000000..0x18008000, shared with the modem): the PCM buffers at the bottom, the codec's tables, and the
 * crash log's words at the top (platform.h HRC_CRASH_BOOT, HRC_CRASH_STORE). A board can give RadioFoundry a part of
 * it that none of those reach as the region MODEM_BSS (from __modem_bss_lo to __modem_bss_hi): .modem_bss, the cold
 * state the core marks RFOUNDRY_COLD_RAM, goes there and crt0 zeroes it. A board without such a part aliases MODEM_BSS
 * to RAM (REGION_ALIAS), and the same state is ordinary bss. */

ENTRY(Reset_Handler)

/* The start-up stack: main() until the scheduler starts (tasks have their own stacks, interrupts the interrupt stack).
 * It is the top of the RTOS heap (.rtos_heap below), so the tasks get its RAM afterwards: make stack-check checks main()
 * against this size, xPortStartScheduler that the tasks made before it stayed below (freertos/port.c) */
_Min_Stack_Size = 0x800;
_Irq_Stack_Size = 0x800;
/* The modem SRAM's fixed users (platform.h, hrc7000.h): the PCM buffers below, the restart cause's words and the crash
 * record above */
__modem_pcm_end = 0x18000140;
__modem_crash_lo = 0x18007EF0;	/* HRC_CRASH_BOOT */
PROVIDE(__modem_bss_lo = 0);	/* no modem SRAM part: MODEM_BSS is RAM */
PROVIDE(__modem_bss_hi = 0);

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the loader, or the first stage, jumps to the first byte */
		*(EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text.*)
		*(.flashfunc .flashfunc.*)	/* HRC_FLASHFUNC: task-side code in the files placed in RAM below */
		/* ... and the engine's set-up, start and stop: task-side, once per mode change; matched here before .ramfunc */
		*hrc7000_dmr.o(.text.hrc7000_dmr_init .text.hrc7000_dmr_run .text.hrc7000_dmr_halt)
		. = ALIGN(4);
	} >FLASH

	.rodata :
	{
		*(.rodata .rodata.* .srodata .srodata.*)
		*(.eh_frame*)
		. = ALIGN(4);
	} >FLASH

	.vectors (NOLOAD) :
	{
		KEEP(*(.vectors))
	} >RAM

	.ramfunc :
	{
		. = ALIGN(4);
		__ramfunc_start = .;
		*(.ramfunc .ramfunc.*)
		*hrc7000_dmr.o(.text .text.*)	/* the slot engine: every modem interrupt runs it (modem-hrc7000.md 9 #15) */
		*hrc7000_bs.o(.text .text.*)	/* ... and the split duplex BS outbound in its place */
		*dmr_glue.o(.text .text.*)
		. = ALIGN(4);
		__ramfunc_end = .;
	} >RAM AT>FLASH
	__ramfunc_load = LOADADDR(.ramfunc);

	.ram_lo (NOLOAD) :
	{
		. = ALIGN(8);
		__ram_lo_bss_start = .;
		*(.ram_lo .ram_lo.*)
		. = ALIGN(8);
		__irq_stack_bottom = .;	/* its canary words (irq.c) */
		. = . + _Irq_Stack_Size;
		__irq_stack_top = .;
		__ram_lo_bss_end = .;
	} >RAM

	.data :
	{
		. = ALIGN(4);
		__data_start = .;
		*(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
		__data_end = .;
	} >RAM AT>FLASH
	__data_load = LOADADDR(.data);
	/* the end of the .bin (the last section copied into it): the support info's image hash runs __app_base .. here */
	__image_end = __data_load + SIZEOF(.data);

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		__bss_start = .;
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} >RAM

	/* Cold state (RFOUNDRY_COLD_RAM): in the board's modem SRAM part, or after bss where MODEM_BSS is RAM */
	.modem_bss (NOLOAD) :
	{
		. = ALIGN(4);
		__modem_bss_start = .;
		*(.modem_bss .modem_bss.*)
		. = ALIGN(4);
		__modem_bss_end = .;
	} >MODEM_BSS

	/* The RTOS heap (platform/hrc7000/main.c ucHeap), not zeroed. heap_4 puts its end marker in the last 8 bytes; the
	 * start-up stack grows down from below it. Before, the stack had 2 KB of its own after .bss, dead once the
	 * scheduler ran */
	.rtos_heap (NOLOAD) :
	{
		. = ALIGN(8);
		__rtos_heap_start = .;
		KEEP(*(.rtos_heap))
		. = ALIGN(8);
		__rtos_heap_end = .;
		PROVIDE(end = .);
		PROVIDE(_end = .);
	} >RAM
	__stack_top = __rtos_heap_end - 8;
	__startup_stack_bottom = __stack_top - _Min_Stack_Size;

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the application (the loader jumps there)")
	ASSERT(ADDR(.vectors) == ORIGIN(RAM) && (ORIGIN(RAM) & 0x3FF) == 0, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= ORIGIN(RAM) + LENGTH(RAM), "RAM overflows into the codec window")
	/* Below the start-up stack the heap must hold what is made before the scheduler runs: the start, idle and timer
	 * tasks' stacks (2,560 + 576 + 1,024 B), their TCBs and the timer queue, about 4.6 KB. 4 KB here let a heap of just
	 * over 6 KB link and then fail xPortStartScheduler's check on the radio; 6 KB keeps that failure at link time */
	ASSERT(__startup_stack_bottom >= __rtos_heap_start + 0x1800,
		"the RTOS heap is too small: 6 KB below the start-up stack for the tasks made before the scheduler runs")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
	/* A modem SRAM part stays clear of the codec's tables (and whatever the codec keeps just past them: the board
	 * leaves a margin), the PCM buffers and the crash log's words */
	ASSERT(__modem_bss_lo == 0 || (__modem_bss_lo >= __codec_tab_hi && __modem_bss_lo >= __modem_pcm_end &&
		__modem_bss_hi <= __modem_crash_lo && __modem_bss_lo <= __modem_bss_hi),
		"the board's modem SRAM part overlaps the codec's tables, the PCM buffers or the crash log")
	ASSERT(__modem_bss_lo == 0 || (__modem_bss_start >= __modem_bss_lo && __modem_bss_end <= __modem_bss_hi),
		".modem_bss overflows the board's modem SRAM part")
}
