/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-32UV, HR_C7000 (CK803S), application behind the Baofeng bootloader (docs/spec/hardware-dm32.md 2).
 *
 * Program flash (XIP, 1 MB at 0x03000000): bootloader 0x03000000..0x0300C000 (never written), the application from
 * 0x0300C000, the codec slot 0x030E8000..0x03100000 (docs/spec/codec-dm32.md "Recommended hosting": the stock codec
 * code and tables are merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): one region for ODR from 0x10000 (the vector table, .ramfunc: interrupt entry and the modem
 * slot engine, the zeroed .ram_lo state the core marks ODR_CPU_RAM and the interrupt stack, then data, bss with the RTOS
 * heap, and the start-up stack), and at the top the codec window 0x43A00..0x58000 (83,456 B, the 049 code): the slot's
 * code is copied there at boot and its address words moved (docs/spec/codec-placement.md). No DMA on this board, so
 * any of it can hold any buffer. The modem SRAM window the codec's tables are copied to, 0x18000E48..0x18003E28, is
 * reserved by not being in this script at all (046 puts its tables lowest, the 049 NRF and fanti builds 8 bytes above
 * 049; codec-dm32.md). */

ENTRY(Reset_Handler)

__stack_top = 0x000439F8;	/* just below the codec window */
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* The codec window (platform/hrc7000/codec_slot.h: the slot's code runs at its start; the size is the largest code
 * among the board's codec maps) and the modem SRAM table window, the union of the builds' stock table addresses */
__codec_run_lo = 0x00043A00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x18000E48;
__codec_tab_hi = 0x18003E28;

MEMORY
{
	FLASH  (rx)  : ORIGIN = 0x0300C000, LENGTH = 0x030E8000 - 0x0300C000
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00043A00 - 0x00010000	/* up to the codec window */
}

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the bootloader jumps to the first byte */
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

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the image (the bootloader jumps there)")
	ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack and the codec window")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
}
