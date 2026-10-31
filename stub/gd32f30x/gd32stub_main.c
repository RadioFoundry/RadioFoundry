// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x flash stub's entry and main loop (docs/spec/stub-gd32f30x.md). RadioFoundry copies the image to 0x20000000
// and jumps to gd32stub_entry with interrupts masked, every NVIC source off, the DMA channels stopped and the entry
// registers of gd32stub_header.h (r0 flags, r1 the core clock, r2 the restart-cause word). The clocks, the GPIO levels
// and the USB core are as RadioFoundry left them. The stub never enables an interrupt: it polls the USB core and runs
// until a RESET request or a power cycle.
//
// Nothing here is a board fact. USB is the chip's USBD core on PA11 / PA12 (its only pins), with RadioFoundry's serial
// number (the UID). The bootloader's end comes from the host's board record (CONFIG, gd32stub.h); until then the stub
// reads the internal flash and writes nothing (stub/common/stubeng.h).
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gd32f30x.h"
#include "gd32stub.h"
#include "stubeng.h"
#include "gd32stub_flash.h"
#include "gd32stub_usb.h"

#define IDLE_MS			200u	// a request that stops arriving for this long is dropped (as the debug link's)
#define CLOCK_DEFAULT_HZ	8000000u	// IRC8M, if RadioFoundry passed no clock

extern uint32_t _sbss, _ebss, _estack;
void stub_main(uint32_t flags, uint32_t hclk, uint32_t soft_mark) __attribute__((noreturn, used));

// First bytes of the image (the linker puts .entry at 0x20000000): set the stack, keep r0-r2, go to C
__attribute__((naked, section(".entry"))) void gd32stub_entry(void)
{
	__asm volatile(
		"ldr r3, =_estack\n\t"
		"mov sp, r3\n\t"
		"b stub_main\n\t"
		".ltorg\n\t");
}

// ---- freestanding C library pieces the compiler may call

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n-- > 0) {
		*d++ = *s++;
	}
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d < s) {
		while (n-- > 0) {
			*d++ = *s++;
		}
	} else {
		while (n-- > 0) {
			d[n] = s[n];
		}
	}
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x - *y;
		}
	}
	return 0;
}

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n-- > 0) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

// ---- time: the cycle counter at the clock RadioFoundry left running

static uint32_t cycles_per_ms;
static uint32_t last_cyc, acc_cyc, now_ms;
static uint32_t soft_mark_addr;

// The image header (gd32stub_header.h), at GD32STUB_HDR_OFFSET by the linker script. RadioFoundry reads it before the copy
// and the jump (docs/spec/stub-gd32f30x.md 3)
_Static_assert(sizeof(gd32stub_header_t) == GD32STUB_HDR_SIZE, "the image header is 16 bytes");
__attribute__((section(".stub_header"), used)) const gd32stub_header_t gd32stub_header = {
	.magic = GD32STUB_HDR_MAGIC,
	.layout = GD32STUB_HDR_LAYOUT,
	.size = GD32STUB_HDR_SIZE,
	.version = GD32STUB_VERSION,
	.flags = GD32STUB_FLAG_FEEDS_FWDGT,
};

// The free watchdog: RadioFoundry started it (CONFIG_WATCHDOG) and nothing but a reset stops it, so the stub feeds it
// everywhere it can spend time. A write to its control register while it isn't running does nothing
void stub_feed(void)
{
	FWDGT_CTL = 0xAAAAu;
}

static void port_feed(void *ctx)
{
	(void)ctx;
	stub_feed();
}

// Every loop that waits reads the time here (the main loop, the flash busy waits, the USB waits, the delays), so it
// feeds the watchdog too
uint32_t stub_ms(void)
{
	stub_feed();
	uint32_t c = DWT->CYCCNT;
	acc_cyc += c - last_cyc;
	last_cyc = c;
	now_ms += acc_cyc / cycles_per_ms;
	acc_cyc %= cycles_per_ms;
	return now_ms;
}

void stub_delay_ms(uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while (stub_ms() - t0 < ms) {
	}
}

// ---- the port the protocol engine drives

static stubeng_port_t port;

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
	return gd32flash_erase(stubeng_sector_addr(&port.flash, sector));
}

static bool port_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	return gd32flash_program(addr, words, n);
}

// CONFIG: the board record gives the bootloader's end; there is no SPI flash for the stub on this target
static bool port_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	gd32_board_t b;
	if (!gd32_board_parse(rec, n, &b)) {
		return false;
	}
	out->app_base = b.app_base;
	return true;
}

static void port_reset(void *ctx)
{
	(void)ctx;
	usb_flush(200);		// the RESET reply leaves the packet buffer first
	stub_delay_ms(20);
	if (soft_mark_addr != 0) {
		*(volatile uint32_t *)(uintptr_t)soft_mark_addr = GD32STUB_SOFT_MARK;	// asked for: not the watchdog
	}
	__DSB();
	NVIC_SystemReset();
}

static stubeng_t engine;
static char serial[25];

// RadioFoundry's USB serial number (platform/gd32f30x/usb.c): the 96-bit UID as hex, lowest byte first. The same string
// keeps the host's port name when the stub has to enumerate again
static void make_serial(void)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)GD32_UID_BASE;
	for (unsigned i = 0; i < 12u; i++) {
		serial[2u * i] = hex[uid[i] >> 4];
		serial[2u * i + 1u] = hex[uid[i] & 0x0Fu];
	}
	serial[24] = '\0';
}

// The soft mark goes above the stub's own RAM (the kept words lie at the top of SRAM on every board), never into it
static bool soft_mark_ok(uint32_t a)
{
	return a % 4u == 0 && a >= (uint32_t)(uintptr_t)&_estack && a < GD32_SRAM_BASE + 0x18000u;
}

void stub_main(uint32_t flags, uint32_t hclk, uint32_t soft_mark)
{
	stub_feed();			// first: RadioFoundry stretched the watchdog for the copy and the jump, nothing more
	for (uint32_t *p = &_sbss; p < &_ebss; p++) {
		*p = 0;
	}
	soft_mark_addr = soft_mark_ok(soft_mark) ? soft_mark : 0;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	cycles_per_ms = (hclk != 0 ? hclk : CLOCK_DEFAULT_HZ) / 1000u;

	uint16_t flash_kb = GD32_FLASH_SIZE_KB;
	gd32flash_init(flash_kb);
	make_serial();
	port.name = GD32STUB_NAME;
	port.version = GD32STUB_VERSION;
	port.magic = GD32STUB_MAGIC;
	port.flash = gd32_flash(flash_kb);
	port.peek = gd32_peek_ranges;
	port.npeek = sizeof(gd32_peek_ranges) / sizeof(gd32_peek_ranges[0]);
	port.write = port_write;
	port.read = port_read;
	port.erase = port_erase;
	port.program = port_program;
	port.reset = port_reset;
	port.configure = port_configure;
	port.feed = port_feed;
	if (usb_start((flags & GD32STUB_ENTRY_USB_ADOPT) != 0, serial)) {
		port.flags |= GD32_HELLO_USB_ADOPTED;
	}
	stubeng_init(&engine, &port);

	uint32_t last_rx = stub_ms();
	for (;;) {
		uint8_t buf[64];
		usb_poll();
		uint32_t n = usb_read(buf, sizeof(buf));
		if (n > 0) {
			stubeng_feed(&engine, buf, n);
			last_rx = stub_ms();
		} else if (stub_ms() - last_rx > IDLE_MS) {
			stubeng_idle(&engine);
			last_rx = stub_ms();
		}
	}
}
