// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's entry and main loop (docs/spec/stub-stm32f4.md). ODR copies the image to 0x20000000 and
// jumps to f4stub_entry with interrupts masked, every NVIC source off, the DMA controllers reset and r0 = the handoff
// flags (platform/stm32f4/stub_entry.c). The clocks, the GPIO levels and the USB core are as ODR left them. The stub
// never enables an interrupt: it polls the USB core and runs until a RESET request or a power cycle.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "f4stub.h"
#include "f4stub_flash.h"
#include "f4stub_spi.h"
#include "f4stub_usb.h"

#define HANDOFF_USB_ADOPT	0x1u	// r0 bit 0: ODR's CDC-ACM device is configured (stub_entry.c)
#define IDLE_MS			200u	// a request that stops arriving for this long is dropped (as the debug link's)
#define HSE_HZ			8000000u	// both radios (hardware-dm1701.md 1.2, hardware-md380.md 1.2)

extern uint32_t _sbss, _ebss;
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

// ---- time: the cycle counter at the clock ODR left running

static uint32_t cycles_per_ms;
static uint32_t last_cyc, acc_cyc, now_ms;

static uint32_t hclk_hz(void)
{
	uint32_t sys = 16000000u;	// HSI
	uint32_t sws = RCC->CFGR & RCC_CFGR_SWS;
	if (sws == RCC_CFGR_SWS_HSE) {
		sys = HSE_HZ;
	} else if (sws == RCC_CFGR_SWS_PLL) {
		uint32_t cfg = RCC->PLLCFGR;
		uint32_t src = (cfg & RCC_PLLCFGR_PLLSRC) ? HSE_HZ : 16000000u;
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

// APB2 (SPI1's clock): HCLK / 1, 2, 4, 8 or 16 (PPRE2 0xx, 100..111)
static uint32_t pclk2_hz(uint32_t hclk)
{
	uint32_t ppre2 = (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos;
	return ppre2 >= 4u ? hclk >> (ppre2 - 3u) : hclk;
}

// The image header (f4stub_header.h), at F4STUB_HDR_OFFSET by the linker script. ODR reads it before the jump: while its
// IWDG runs it enters only a stub whose header says it feeds the IWDG (docs/spec/watchdog.md 3.1)
_Static_assert(sizeof(f4stub_header_t) == F4STUB_HDR_SIZE, "the image header is 16 bytes");
__attribute__((section(".stub_header"), used)) const f4stub_header_t f4stub_header = {
	.magic = F4STUB_HDR_MAGIC,
	.layout = F4STUB_HDR_LAYOUT,
	.size = F4STUB_HDR_SIZE,
	.version = F4STUB_VERSION,
	.flags = F4STUB_FLAG_FEEDS_IWDG,
};

// The IWDG: ODR started it (CONFIG_WATCHDOG) and nothing but a reset stops it, so the stub feeds it everywhere it can
// spend time. A write to KR while it isn't running does nothing

void stub_feed(void)
{
	IWDG->KR = 0xAAAAu;
}

static void port_feed(void *ctx)
{
	(void)ctx;
	stub_feed();
}

// Every loop that waits reads the time here (the main loop, the flash erase and program busy waits, the SPI flash's
// status polls, the USB waits, the delays), so it feeds the IWDG too
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
	return f4spi_read(addr, dst, n);
}

static bool port_xerase(void *ctx, uint32_t addr)
{
	(void)ctx;
	return f4spi_erase(addr);
}

static bool port_xprogram(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	return f4spi_program(addr, data, n);
}

static void port_reset(void *ctx)
{
	(void)ctx;
	usb_flush(200);		// the RESET reply leaves the FIFO first
	stub_delay_ms(20);
	NVIC_SystemReset();
}

static f4stub_port_t port = {
	.write = port_write,
	.read = port_read,
	.erase = port_erase,
	.program = port_program,
	.reset = port_reset,
	.xread = port_xread,
	.xerase = port_xerase,
	.xprogram = port_xprogram,
	.feed = port_feed,
};
static f4stub_t engine;
static char serial[25];

// ODR's USB serial number (boards/*/usb_*.c): the 96-bit UID as hex, lowest byte first. The same string keeps the
// host's port name when the stub has to enumerate again
static void make_serial(void)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)UID_BASE;
	for (unsigned i = 0; i < 12u; i++) {
		serial[2u * i] = hex[uid[i] >> 4];
		serial[2u * i + 1u] = hex[uid[i] & 0x0Fu];
	}
	serial[24] = '\0';
}

void stub_main(uint32_t flags)
{
	stub_feed();			// first: ODR stretched the IWDG for the copy and the jump, nothing more
	for (uint32_t *p = &_sbss; p < &_ebss; p++) {
		*p = 0;
	}
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	uint32_t hclk = hclk_hz();
	cycles_per_ms = hclk / 1000u;
	if (cycles_per_ms == 0) {
		cycles_per_ms = 16000u;
	}

	f4flash_init();
	port.xsize = f4spi_init(pclk2_hz(hclk), port.xid);
	make_serial();
	port.flash_kb = *(const volatile uint16_t *)FLASHSIZE_BASE;
	port.rdp = (uint8_t)(FLASH->OPTCR >> 8);
	if (usb_start((flags & HANDOFF_USB_ADOPT) != 0, serial)) {
		port.flags |= F4_HELLO_USB_ADOPTED;
	}
	f4stub_init(&engine, &port);

	uint32_t last_rx = stub_ms();
	for (;;) {
		uint8_t buf[64];
		usb_poll();
		uint32_t n = usb_read(buf, sizeof(buf));
		if (n > 0) {
			f4stub_feed(&engine, buf, n);
			last_rx = stub_ms();
		} else if (stub_ms() - last_rx > IDLE_MS) {
			f4stub_idle(&engine);
			last_rx = stub_ms();
		}
	}
}
