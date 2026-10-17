/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Radioddity DB25-D with DMR (CONFIG_DMR=y, boards/db25d/config.mk selects this script), HR_C7000 (CK803S), application
 * behind the vendor IAP loader and ODR's first stage (db25d.ld; docs/spec/hardware-db25d.md 2, 7).
 *
 * Program flash (XIP, 2 MB at 0x03000000): the loader 0x03000000..0x0300C000 (never written), ODR's first stage
 * 0x0300C000..0x0300D000, the application from 0x0300D000, the codec slot 0x030E8000..0x03100000 (hardware-db25d.md 8:
 * the stock codec code and tables are merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): the stock codec runs at its own RAM address, 0x1CEB0..0x2F9D0 (all three 018 builds the
 * recipe supports put it there), so that window is reserved. Below it (RAM_LO, 52,912 B): the vector table, code that
 * must not run from flash (.ramfunc: interrupt entry, the modem slot handlers), the zeroed section .ram_lo (the state
 * the core marks ODR_CPU_RAM; config.mk sets ODR_CPU_RAM_SECTION to ".ram_lo") and the interrupt stack. Above it (RAM,
 * 164,400 B): data, bss (the RTOS heap is inside it) and the start-up stack at the top. Both regions are checked at
 * the end. The runtime stub entry (CONFIG_STUB_ENTRY) loads its monitor into the codec window, 0x1D000..0x2F000
 * (HRC_STUB_BASE in config.mk), after the codec is parked; the DMR-less script db25d.ld keeps it at 0x30000. The
 * modem SRAM window the codec's tables are copied to, 0x180014AC..0x18004204, is reserved by not being in this script
 * at all. */

ENTRY(Reset_Handler)

__stack_top = 0x00057FF8;
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* Where a slot's codec may run (platform/hrc7000/vocoder.c checks the slot against them; tools/dm32_codec.py BOARDS
 * "db25d" keeps the same numbers): the RAM window below and the modem SRAM table window */
__codec_run_lo = 0x0001CEB0;
__codec_run_hi = 0x0002F9D0;
__codec_tab_lo = 0x180014AC;
__codec_tab_hi = 0x18004204;

MEMORY
{
	FLASH  (rx)  : ORIGIN = 0x0300D000, LENGTH = 0x030E8000 - 0x0300D000
	RAM_LO (rwx) : ORIGIN = 0x00010000, LENGTH = 0x0001CEB0 - 0x00010000
	RAM    (rwx) : ORIGIN = 0x0002F9D0, LENGTH = 0x00058000 - 0x0002F9D0
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
	} >RAM_LO

	.ramfunc :
	{
		. = ALIGN(4);
		__ramfunc_start = .;
		*(.ramfunc .ramfunc.*)
		*hrc7000_dmr.o(.text .text.*)	/* the slot engine: every modem interrupt runs it (modem-hrc7000.md 9 #15) */
		*dmr_glue.o(.text .text.*)
		. = ALIGN(4);
		__ramfunc_end = .;
	} >RAM_LO AT>FLASH
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
	} >RAM_LO

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
	ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM_LO (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack")
	ASSERT(__ram_lo_bss_end <= ORIGIN(RAM_LO) + LENGTH(RAM_LO), "RAM_LO overflows into the codec's RAM window: move ODR_CPU_RAM state out")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
}
