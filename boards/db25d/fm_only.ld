/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Radioddity DB25-D without DMR (CONFIG_DMR=n, config.mk selects this script; memory.ld is the DMR build's), HR_C7000
 * (CK803S), application behind the vendor IAP loader (docs/spec/hardware-db25d.md 2).
 *
 * Program flash (XIP at 0x03000000): the loader 0x03000000..0x0300C000 (never written), then the application region
 * up to 0x0310C000, where the stock firmware's data area starts; the stock calibration copy at 0x031FE000 is far above
 * it. The loader jumps to 0x0300C000. RadioFoundry's first 4 KB sector there is the first stage (docs/spec/hardware-db25d.md 7):
 * it answers a stub load on the PC cable as the serial boot ROM does on UART0, then jumps here, to 0x0300D000
 * (__app_start: board.mk APP_BASE + BOOT_SIZE).
 *
 * SRAM (288 KB at 0x10000): the vector table first (VBR, 1 KB aligned), code that must not run from flash
 * (.ramfunc), data, bss, the interrupt stack, and the RTOS heap last with the start-up stack in its top. No codec
 * window: without DMR nothing is reserved for the stock codec's RAM. */

ENTRY(Reset_Handler)

/* The start-up stack: the top of the RTOS heap, as platform/hrc7000/common.ld sets it out */
_Min_Stack_Size = 0x800;
_Irq_Stack_Size = 0x800;

MEMORY
{
	FLASH (rx)  : ORIGIN = __app_start, LENGTH = 0x0310C000 - __app_start
	RAM   (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00058000 - 0x00010000
}

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the first stage jumps to the first byte */
		*(.text .text.*)
		*(.flashfunc .flashfunc.*)
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
		. = ALIGN(4);
		__ramfunc_end = .;
	} >RAM AT>FLASH
	__ramfunc_load = LOADADDR(.ramfunc);

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
		*(.ram_lo .ram_lo.*)
		*(.modem_bss .modem_bss.*)	/* the cold state (RFOUNDRY_COLD_RAM): ordinary bss here */
		. = ALIGN(4);
		__bss_end = .;
	} >RAM
	__modem_bss_start = __bss_end;	/* crt0's zeroing of it: empty, bss covers it */
	__modem_bss_end = __bss_end;

	/* crt0 zeroes this range too; here it only holds the interrupt stack */
	.irqstack (NOLOAD) :
	{
		. = ALIGN(8);
		__ram_lo_bss_start = .;
		__irq_stack_bottom = .;	/* its canary words (irq.c) */
		. = . + _Irq_Stack_Size;
		__irq_stack_top = .;
		__ram_lo_bss_end = .;
	} >RAM

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

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the image (the loader jumps there)")
	ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= ORIGIN(RAM) + LENGTH(RAM), "RAM overflows")
	/* 6 KB below the start-up stack for the tasks made before the scheduler (platform/hrc7000/common.ld says why) */
	ASSERT(__startup_stack_bottom >= __rtos_heap_start + 0x1800,
		"the RTOS heap is too small: 6 KB below the start-up stack for the tasks made before the scheduler runs")
	ASSERT(. <= 0x00030000, "RAM reaches the stub's region (HRC_STUB_BASE in board.mk)")
}
