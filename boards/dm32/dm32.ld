/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-32UV, HR_C7000 (CK803S), application behind the Baofeng bootloader (docs/spec/hardware-dm32.md 2).
 *
 * Program flash (XIP, 1 MB at 0x03000000): bootloader 0x03000000..0x0300C000 (never written), the application from
 * 0x0300C000, the codec slot 0x030E8000..0x03100000 (docs/spec/codec-dm32.md "Recommended hosting": the stock codec
 * code and tables are merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): the stock codec runs at its own RAM address, so the union of the 049 and 046 code windows,
 * 0x16CDC..0x2F914, is reserved. Below it (RAM_LO, 27,868 B): the vector table, code that must not run from flash
 * (.ramfunc: interrupt entry, modem slot handlers), then the zeroed section .ram_lo: the state the core marks
 * ODR_CPU_RAM (board.mk sets ODR_CPU_RAM_SECTION to ".ram_lo": the radio task's state machine, APRS, the hotspot's
 * rings, the DMR data buffers) and the interrupt stack. The network's ODR_CPU_RAM buffers (TCP connections, the
 * network personality's packets and frame, the gateway's air bursts and frame) go to main RAM instead: RAM_LO can't
 * hold them as well, and main RAM has the room. Above the codec window (RAM): data, bss, the RTOS heap (inside bss) and
 * the start-up stack at the top. Both regions are checked at the end. No DMA on this board, so either region can
 * hold any buffer. The modem SRAM window the codec's tables are copied to, 0x18000E48..0x18003E28, is reserved by
 * not being in this script at all (the 049 NRF and fanti builds put their tables 8 bytes higher, codec-dm32.md). */

ENTRY(Reset_Handler)

__stack_top = 0x00057FF8;
_Min_Stack_Size = 0x800;	/* start-up only: tasks have their own stacks, interrupts the interrupt stack */
_Irq_Stack_Size = 0x800;

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* Where a slot's codec may run (platform/hrc7000/vocoder.c checks the slot against them): the RAM window below and the
 * modem SRAM table window */
__codec_run_lo = 0x00016CDC;
__codec_run_hi = 0x0002F914;
__codec_tab_lo = 0x18000E48;
__codec_tab_hi = 0x18003E28;

MEMORY
{
	FLASH  (rx)  : ORIGIN = 0x0300C000, LENGTH = 0x030E8000 - 0x0300C000
	RAM_LO (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00016CDC - 0x00010000
	RAM    (rwx) : ORIGIN = 0x0002F914, LENGTH = 0x00058000 - 0x0002F914
}

SECTIONS
{
	.text :
	{
		KEEP(*(.entry))	/* crt0: the bootloader jumps to the first byte */
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
		*(EXCLUDE_FILE(*core/net/*.o *hostlink_net*.o *ipGateway.o) .ram_lo EXCLUDE_FILE(*core/net/*.o *hostlink_net*.o *ipGateway.o) .ram_lo.*)
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
		*(.ram_lo .ram_lo.*)	/* what RAM_LO leaves out (the network's buffers): zeroed with .bss */
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

	ASSERT(Reset_Handler == ORIGIN(FLASH), "crt0 must be the first byte of the image (the bootloader jumps there)") ASSERT(ADDR(.vectors) == 0x00010000, "the vector table must start RAM_LO (1 KB aligned for VBR)")
	ASSERT(. <= __stack_top, "RAM overflows into the start-up stack")
	ASSERT(__ram_lo_bss_end <= ORIGIN(RAM_LO) + LENGTH(RAM_LO), "RAM_LO overflows into the codec's RAM window: move ODR_CPU_RAM state out (dm32.ld .ram_lo)")
	ASSERT(__data_load + SIZEOF(.data) <= __codec_slot, "the image runs into the codec slot")
}
