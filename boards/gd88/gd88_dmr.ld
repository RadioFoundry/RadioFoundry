/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Radioddity GD-88 with DMR (CONFIG_DMR=y, boards/gd88/config.mk selects this script), SoC A or B (boards/gd88b
 * uses it too), HR_C7000 (CK803S), application behind the IAP loader and ODR's first stage (gd88.ld;
 * docs/spec/hardware-gd88.md 2, 7, 8).
 *
 * Program flash (XIP, 2 MB at 0x03000000): the loader below 0x0300C000 (never written), ODR's first stage
 * 0x0300C000..0x0300D000, the application from 0x0300D000, the codec slot 0x030E8000..0x03100000 (hardware-gd88.md 8:
 * the codec is merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): one region for ODR from 0x10000 (the vector table, .ramfunc: interrupt entry and the modem
 * slot engine, the zeroed .ram_lo state the core marks ODR_CPU_RAM and the interrupt stack, then data, bss with the RTOS
 * heap, and the start-up stack), and at the top the codec window 0x44E00..0x58000: the slot's code is copied there at
 * boot and its address words moved (docs/spec/codec-placement.md). The runtime stub entry (CONFIG_STUB_ENTRY) loads the
 * stub into the codec window (HRC_STUB_BASE in config.mk) after the codec is parked. The modem SRAM window the codec's
 * tables are copied to (A 0x18001204..0x18004324, B 0x18000B40..0x18003B10, hardware-gd88.md 8; the script reserves
 * their union) is reserved by not being in this script at all. tools/hrc7000_codec.py BOARDS "gd88" / "gd88b" keep the
 * same numbers. */

ENTRY(Reset_Handler)

__stack_top = 0x00044DF8;	/* just below the codec window */
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* The codec window (platform/hrc7000/codec_slot.h: the slot's code runs at its start) and the modem SRAM table
 * window, the union of A's and B's table addresses (tools/hrc7000_codec.py BOARDS "gd88", "gd88b") */
__codec_run_lo = 0x00044E00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x18000B40;
__codec_tab_hi = 0x18004324;

MEMORY
{
	FLASH  (rx)  : ORIGIN = 0x0300D000, LENGTH = 0x030E8000 - 0x0300D000
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00044E00 - 0x00010000	/* up to the codec window */
}

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the first stage jumps to the first byte */
		*(EXCLUDE_FILE(*hrc7000_dmr.o *dmr_glue.o) .text EXCLUDE_FILE(*hrc7000_dmr.o *dmr_glue.o) .text.*)
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

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the image (the first stage jumps there)")
	ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack and the codec window")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
}
