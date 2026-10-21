// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's entry and main loop (docs/spec/stub-stm32f4.md). ODR copies the image to 0x20000000 and
// jumps to f4stub_entry with interrupts masked, every NVIC source off, the DMA controllers reset and r0 = the handoff
// flags (platform/stm32f4/stub_entry.c). The clocks, the GPIO levels and the USB core are as ODR left them. The stub
// never enables an interrupt: it polls the USB core and runs until a RESET request or a power cycle.
//
// Nothing here is a board fact. USB is the chip's OTG_FS core on PA11 / PA12 (the STM32F405's only full speed pins,
// set up by ODR), with ODR's serial number (the UID). The SPI flash wiring, the crystal and the bootloader's end
// come from the host's board record (CONFIG, f4stub.h); until then the stub reads the internal flash and writes
// nothing (stub/common/stubeng.h).
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "f4stub.h"
#include "stubeng.h"
#include "f4stub_flash.h"
#include "f4stub_spi.h"
#include "stubnor.h"
#include "stubrt.h"
#include "stubusb.h"

#define HANDOFF_USB_ADOPT	0x1u	// r0 bit 0: ODR's CDC-ACM device is configured (stub_entry.c)
#define HSE_DEFAULT_HZ		8000000u	// until a board record says otherwise: only the stub's own timing uses it

void stub_main(uint32_t flags) __attribute__((noreturn, used));

// First bytes of the image (the linker puts .entry at 0x20000000): set the stack, keep r0, go to C
__attribute__((naked, section(".entry"))) void f4stub_entry(void)
{
	__asm volatile(
		"ldr r1, =_estack\n\t"
		"mov sp, r1\n\t"
		"b stub_main\n\t"
		".ltorg\n\t");
}

// ---- the clock ODR left running (for the cycle counter's rate and the SPI prescaler)

static uint32_t hse_hz = HSE_DEFAULT_HZ;

static uint32_t hclk_hz(void)
{
	uint32_t sys = 16000000u;	// HSI
	uint32_t sws = RCC->CFGR & RCC_CFGR_SWS;
	if (sws == RCC_CFGR_SWS_HSE) {
		sys = hse_hz;
	} else if (sws == RCC_CFGR_SWS_PLL) {
		uint32_t cfg = RCC->PLLCFGR;
		uint32_t src = (cfg & RCC_PLLCFGR_PLLSRC) ? hse_hz : 16000000u;
		uint32_t m = cfg & RCC_PLLCFGR_PLLM;
		uint32_t n = (cfg & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
		uint32_t p = ((((cfg & RCC_PLLCFGR_PLLP) >> RCC_PLLCFGR_PLLP_Pos) + 1u) * 2u);
		if (m != 0) {
			sys = src / m * n / p;
		}
	}
	static const uint8_t shift[8] = { 1, 2, 3, 4, 6, 7, 8, 9 };	// HPRE 8..15
	uint32_t hpre = (RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos;
	return hpre >= 8u ? sys >> shift[hpre - 8u] : sys;
}

// An APB clock: HCLK / 1, 2, 4, 8 or 16 (PPREx 0xx, 100..111). APB2 clocks SPI1, APB1 SPI2 and SPI3
static uint32_t pclk_hz(uint32_t hclk, bool apb2)
{
	uint32_t ppre = apb2 ? (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos
			     : (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
	return ppre >= 4u ? hclk >> (ppre - 3u) : hclk;
}

// ---- the port the protocol engine drives

static void port_write(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	(void)usb_write(buf, n);
}

static void port_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

static bool port_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	return f4flash_erase(sector);
}

static bool port_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	return f4flash_program(addr, words, n);
}

static bool port_xread(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	return stubnor_read(addr, dst, n);
}

static bool port_xerase(void *ctx, uint32_t addr)
{
	(void)ctx;
	return stubnor_erase(addr);
}

static bool port_xprogram(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	return stubnor_program(addr, data, n);
}

// CONFIG: the board record decides the crystal (for the stub's timing) and the SPI flash wiring
static bool port_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	f4_board_t b;
	if (!f4_board_parse(rec, n, &b)) {
		return false;
	}
	hse_hz = b.hse_hz;
	stubrt_clock_rate(hclk_hz());
	out->app_base = b.app_base;
	out->xsize = f4spi_init(&b, pclk_hz(hclk_hz(), b.spi_bus == 1u), out->xid);
	return true;
}

static stubeng_port_t port = {
	.name = F4STUB_NAME,
	.version = F4STUB_VERSION,
	.magic = F4STUB_MAGIC,
	.peek = f4_peek_ranges,
	.npeek = sizeof(f4_peek_ranges) / sizeof(f4_peek_ranges[0]),
	.write = port_write,
	.read = port_read,
	.erase = port_erase,
	.program = port_program,
	.reset = stubrt_reset,
	.xread = port_xread,
	.xerase = port_xerase,
	.xprogram = port_xprogram,
	.configure = port_configure,
};
static stubeng_t engine;
static char serial[25];

void stub_main(uint32_t flags)
{
	stubrt_zero_bss();
	stubrt_clock_start(hclk_hz());

	f4flash_init();
	// ODR's USB serial number (boards/*/usb_*.c): the 96-bit UID as hex, lowest byte first. The same string keeps the
	// host's port name when the stub has to enumerate again
	stubrt_hex(serial, (const uint8_t *)UID_BASE, 12);
	port.flash = f4_flash(*(const volatile uint16_t *)FLASHSIZE_BASE);
	port.security = (uint8_t)(FLASH->OPTCR >> 8);
	if (usb_start((flags & HANDOFF_USB_ADOPT) != 0, 0, serial)) {
		port.flags |= F4_HELLO_USB_ADOPTED;
	}
	stubeng_init(&engine, &port);
	stubrt_serve(&engine);
}
