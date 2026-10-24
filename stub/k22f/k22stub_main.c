// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F flash stub's entry and glue (docs/spec/stub-k22f.md). ODR copies the image to SRAM_L's base and jumps to
// k22stub_entry with interrupts masked, every NVIC source off, DMA and I2S stopped, the clock in RUN, the USB
// controller parked, r0 = the hand-over flags, r1 = the USB data toggles and r2 = the core clock
// (platform/k22f/stub_entry.c, platform/k22f/stub_handoff.h). The pins keep the levels ODR left (the power hold
// among them). The stub never enables an interrupt: it polls the USB controller and runs until a RESET request or a
// power cycle.
//
// Nothing here is a board fact. USB is the chip's full-speed controller on its dedicated pins with ODR's serial number
// (the unique ID). The SPI flash wiring, the writable flash range and the SPI flash's write floor come from the host's
// board record (CONFIG, k22stub.h); until then the stub reads the internal flash and writes nothing
// (stub/common/stubeng.h).
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "k22f.h"
#include "k22stub.h"
#include "k22stub_flash.h"
#include "k22stub_spi.h"
#include "stub_handoff.h"
#include "stubeng.h"
#include "stubnor.h"
#include "stubrt.h"
#include "stubusb.h"

#define FEI_HZ		20971520u	// the core clock out of reset (FLL on the slow internal reference)
#define SIM_LO		0x40047000u	// the SIM's registers: read a word at a time
#define SIM_HI		0x40049000u

void stub_main(uint32_t flags, uint32_t toggles, uint32_t core_hz) __attribute__((noreturn, used));

// First bytes of the image (the linker puts .entry at SRAM_L's base): set the stack, keep r0..r2, go to C. r3 holds
// the stack address, so the hand-over's three registers reach stub_main as its arguments
__attribute__((naked, section(".entry"))) void k22stub_entry(void)
{
	__asm volatile(
		"ldr r3, =_estack\n\t"
		"mov sp, r3\n\t"
		"b stub_main\n\t"
		".ltorg\n\t");
}

// The watchdog (stubrt.h): ODR's start-up switches the K22F's WDOG off (platform/k22f/startup.c) and nothing turns it
// on again, so there is nothing to feed
void stub_feed(void)
{
}

// ---- the port the protocol engine drives

static void port_write(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	(void)usb_write(buf, n);
}

// Memory and flash byte by byte; the SIM's 32-bit registers a word at a time, the bytes taken from the word
static void port_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	if (addr >= SIM_LO && addr < SIM_HI) {
		uint8_t *d = dst;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t a = addr + i;
			uint32_t w = *(const volatile uint32_t *)(uintptr_t)(a & ~3u);
			d[i] = (uint8_t)(w >> (8u * (a & 3u)));
		}
		return;
	}
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

static bool port_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	return k22flash_erase(K22_FLASH_BASE + sector * K22_SECTOR);
}

static bool port_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	return k22flash_program(addr, words, n);
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

// CONFIG: the board record decides the writable flash range, the SPI flash wiring and its write floor
static bool port_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	k22_board_t b;
	if (!k22_board_parse(rec, n, &b)) {
		return false;
	}
	out->app_base = b.app_base;
	out->app_end = b.app_end;
	out->xfloor = b.xfloor;
	out->xsize = k22spi_init(&b, out->xid);
	return true;
}

static stubeng_port_t port = {
	.name = K22STUB_NAME,
	.version = K22STUB_VERSION,
	.magic = K22STUB_MAGIC,
	.peek = k22_peek_ranges,
	.npeek = sizeof(k22_peek_ranges) / sizeof(k22_peek_ranges[0]),
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

void stub_main(uint32_t flags, uint32_t toggles, uint32_t core_hz)
{
	stubrt_zero_bss();
	stubrt_clock_start(core_hz >= 1000000u && core_hz <= 120000000u ? core_hz : FEI_HZ);

	k22flash_init();
	// ODR's USB serial number (boards/gd77/usb_gd77.c): the low 96 bits of the unique ID, words high to low, each
	// high nibble first. The same string keeps the host's port name when the stub has to enumerate again
	const uint32_t uid[3] = { SIM_UIDMH, SIM_UIDML, SIM_UIDL };
	uint8_t be[12];
	for (unsigned i = 0; i < 12u; i++) {
		be[i] = (uint8_t)(uid[i / 4u] >> (24u - 8u * (i % 4u)));
	}
	stubrt_hex(serial, be, 12);
	port.flash = k22_flash(k22_flash_kb(SIM_FCFG1_PFSIZE(SIM_FCFG1)));
	port.security = FTFA_FSEC;
	if (usb_start((flags & K22F_STUB_USB_ADOPT) != 0, toggles, serial)) {
		port.flags |= K22_HELLO_USB_ADOPTED;
	}
	stubeng_init(&engine, &port);
	stubrt_serve(&engine);
}
