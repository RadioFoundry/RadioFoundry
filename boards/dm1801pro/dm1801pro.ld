/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-1801 Pro, HR_C7000 (CK803S), application behind the Baofeng boot loader (docs/spec/hardware-dm1801pro.md 2).
 *
 * Program flash (XIP, 1 MB at 0x03000000): the boot loader 0x03000000..0x0300C000 (never written), the application
 * from 0x0300C000 to the top. No codec slot yet: DMR isn't built for this board, so the space for the codec's code
 * and tables is decided with the DMR bring-up.
 *
 * SRAM (288 KB at 0x10000): the vector table first (VBR, 1 KB aligned), code that must not run from flash
 * (.ramfunc), data, bss (the RTOS heap is inside it), the interrupt stack and the start-up stack at the top. No codec
 * window yet either, so nothing in SRAM is reserved. */

ENTRY(Reset_Handler)

__stack_top = 0x00057FF8;
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

MEMORY
{
	FLASH (rx)  : ORIGIN = 0x0300C000, LENGTH = 0x03100000 - 0x0300C000
	RAM   (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00058000 - 0x00010000
}

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the boot loader jumps to the first byte */
		*(.text .text.*)
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

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		__bss_start = .;
		*(.bss .bss.* .sbss .sbss.* COMMON)
		*(.ram_lo .ram_lo.*)
		. = ALIGN(4);
		__bss_end = .;
	} >RAM

	/* crt0 zeroes this range too; here it only holds the interrupt stack */
	.irqstack (NOLOAD) :
	{
		. = ALIGN(8);
		__ram_lo_bss_start = .;
		. = . + _Irq_Stack_Size;
		__irq_stack_top = .;
		__ram_lo_bss_end = .;
	} >RAM

	.stack (NOLOAD) :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		. = . + _Min_Stack_Size;
	} >RAM

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the image (the boot loader jumps there)")
	ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack")
}
