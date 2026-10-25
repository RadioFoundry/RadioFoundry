// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash stubs' Cortex-M runtime (stubrt.h). Architecture registers only: the Armv7-M addresses of DEMCR, the DWT
// cycle counter and AIRCR are the same on every Cortex-M3 / M4.
#include <stddef.h>
#include <stdint.h>
#include "stubrt.h"
#include "stubusb.h"

#define DEMCR		(*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_TRCENA	(1u << 24)
#define DWT_CTRL	(*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT	(*(volatile uint32_t *)0xE0001004u)
#define DWT_CYCCNTENA	1u
#define AIRCR		(*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_VECTKEY	0x05FA0000u
#define AIRCR_PRIGROUP	0x00000700u
#define AIRCR_SYSRESETREQ	0x00000004u

extern uint32_t _sbss, _ebss;

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

void stubrt_zero_bss(void)
{
	for (uint32_t *p = &_sbss; p < &_ebss; p++) {
		*p = 0;
	}
}

// ---- time: the cycle counter at the clock the target names

static uint32_t cycles_per_ms;
static uint32_t last_cyc, acc_cyc, now_ms;

void stubrt_clock_rate(uint32_t core_hz)
{
	(void)stub_ms();	// what passed so far, at the old rate
	cycles_per_ms = core_hz / 1000u;
	if (cycles_per_ms < 1000u) {
		cycles_per_ms = 16000u;
	}
}

void stubrt_clock_start(uint32_t core_hz)
{
	DEMCR |= DEMCR_TRCENA;
	DWT_CYCCNT = 0;
	DWT_CTRL |= DWT_CYCCNTENA;
	last_cyc = acc_cyc = now_ms = 0;
	cycles_per_ms = 16000u;
	stubrt_clock_rate(core_hz);
}

uint32_t stub_ms(void)
{
	stub_feed();
	uint32_t c = DWT_CYCCNT;
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

void stubrt_hex(char *out, const uint8_t *in, unsigned n)
{
	static const char hex[] = "0123456789ABCDEF";
	for (unsigned i = 0; i < n; i++) {
		out[2u * i] = hex[in[i] >> 4];
		out[2u * i + 1u] = hex[in[i] & 0x0Fu];
	}
	out[2u * n] = '\0';
}

void stubrt_reset(void *ctx)
{
	(void)ctx;
	usb_flush(200);		// the RESET reply leaves first
	stub_delay_ms(20);
	__asm volatile("dsb" ::: "memory");
	AIRCR = AIRCR_VECTKEY | (AIRCR & AIRCR_PRIGROUP) | AIRCR_SYSRESETREQ;
	__asm volatile("dsb" ::: "memory");
	for (;;) {
	}
}

void stubrt_serve(stubeng_t *e)
{
	uint32_t last_rx = stub_ms();
	for (;;) {
		uint8_t buf[64];
		usb_poll();
		uint32_t n = usb_read(buf, sizeof(buf));
		if (n > 0) {
			stubeng_feed(e, buf, n);
			last_rx = stub_ms();
		} else if (stub_ms() - last_rx > STUBRT_IDLE_MS) {
			stubeng_idle(e);
			last_rx = stub_ms();
		}
	}
}
