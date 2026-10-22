/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* HR_C7000 (CK803S) application layout with a codec slot, INCLUDEd by each board's memory.ld after its codec symbols
 * and its MEMORY (docs/spec/codec-placement.md).
 *
 * Program flash (XIP at 0x03000000): the factory loader below the image (never written), the application from
 * ORIGIN(FLASH) (board.mk APP_BASE, after the first stage when there is one), then the codec slot __codec_slot ..
 * __codec_slot_end: the stock codec code and tables are merged there on the user's machine and never linked into.
 *
 * SRAM: one region for ODR from ORIGIN(RAM) (the vector table, .ramfunc: interrupt entry and the modem slot engine, the
 * zeroed .ram_lo state the core marks ODR_CPU_RAM and the interrupt stack, then data, bss with the RTOS heap, and the
 * start-up stack), and above it the codec window __codec_run_lo .. __codec_run_hi: the slot's code is copied there at
 * boot and its address words moved. No DMA on these radios, so any of it can hold any buffer. The modem SRAM window
 * the codec's tables are copied to, __codec_tab_lo .. __codec_tab_hi, is reserved by not being in the script at all. */

ENTRY(Reset_Handler)

__stack_top = ORIGIN(RAM) + LENGTH(RAM) - 8;	/* just below the codec window */
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the loader, or the first stage, jumps to the first byte */
		*(EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text.*)
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

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		__bss_start = .;
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} >RAM

	.stack (NOLOAD) :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		. = . + _Min_Stack_Size;
	} >RAM

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the application (the loader jumps there)")
	ASSERT(ADDR(.vectors) == ORIGIN(RAM) && (ORIGIN(RAM) & 0x3FF) == 0, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack and the codec window")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
}
