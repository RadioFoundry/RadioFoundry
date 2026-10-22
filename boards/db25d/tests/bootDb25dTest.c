// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DB25-D first stage (boot/db25d/boot.c) against a simulated UART1: the stub load stubtool performs, frames that must
// be dropped, the window running out to the application, a dead UART, the UART set-up on a cold start, and the
// transmitter pins driven off before anything else.
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_HOST_TEST
#define BOOT_WINDOW	3

static uint8_t ram[0x50000];
static uint8_t rxq[8192];
static size_t rx_len, rx_pos;
static uint8_t txq[4096];
static size_t tx_len;
static uint32_t lcr, dll, diplex0, gates, gpioa_dr, gpioa_ddr, gpiob_dr, gpiob_ddr, wdt_en = 1;
static int uart_dead;
static jmp_buf done;
static uint32_t jumped_to;
static int resumed;
static unsigned rx_after_offers;	// bytes become visible only once this many handshakes went out
static unsigned offers_seen;

static uint32_t rd(uint32_t a)
{
	switch (a) {
	case 0x14040000u:
		return rx_pos < rx_len ? rxq[rx_pos++] : 0;
	case 0x1404000Cu:
		return lcr;
	case 0x14040014u:
		return 0x40u | (rx_pos < rx_len && offers_seen >= rx_after_offers ? 1u : 0u);
	case 0x1404007Cu:
		return uart_dead ? 0 : 2u;
	case 0x11000034u:
		return diplex0;
	case 0x14020000u:
		return gpioa_dr;
	case 0x14020004u:
		return gpioa_ddr;
	case 0x14100000u:
		return gpiob_dr;
	case 0x14100004u:
		return gpiob_ddr;
	default:
		return 0;
	}
}

static void wr(uint32_t a, uint32_t v)
{
	switch (a) {
	case 0x14040000u:
		if (lcr & 0x80u) {
			dll = v;
		} else if (tx_len < sizeof txq) {
			txq[tx_len++] = (uint8_t)v;
			if (tx_len >= 4 && memcmp(txq + tx_len - 4, "\x02\x24\x00\x03", 4) == 0) {
				offers_seen++;
			}
		}
		break;
	case 0x1404000Cu:
		lcr = v;
		break;
	case 0x1100002Cu:
		gates = v;
		break;
	case 0x11000034u:
		diplex0 = v;
		break;
	case 0x14020000u:
		gpioa_dr = v;
		break;
	case 0x14020004u:
		gpioa_ddr = v;
		break;
	case 0x14100000u:
		gpiob_dr = v;
		break;
	case 0x14100004u:
		gpiob_ddr = v;
		break;
	case 0x14010010u:
		wdt_en = v;
		break;
	default:
		break;
	}
}

static void ram_wr(uint32_t a, uint8_t v)
{
	if (a >= sizeof ram) {
		fprintf(stderr, "write outside RAM: 0x%X\n", (unsigned)a);
		exit(1);
	}
	ram[a] = v;
}

static void jump(uint32_t a)
{
	jumped_to = a;
	longjmp(done, 1);
}

void boot_resume(void)
{
	resumed = 1;
	longjmp(done, 1);
}

#include "../../../boot/db25d/boot.c"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void reset(uint32_t lcr0)
{
	memset(ram, 0, sizeof ram);
	rx_len = rx_pos = tx_len = 0;
	lcr = lcr0;
	dll = diplex0 = gates = gpioa_dr = gpioa_ddr = gpiob_dr = gpiob_ddr = 0;
	diplex0 = 0x600u;
	wdt_en = 1;
	uart_dead = 0;
	jumped_to = 0;
	resumed = 0;
	rx_after_offers = 1;
	offers_seen = 0;
}

static void push(const void *p, size_t n)
{
	memcpy(rxq + rx_len, p, n);
	rx_len += n;
}

// A ROM-protocol frame as stubtool's rom_cmd() builds it
static void push_frame(uint8_t op, const uint8_t *payload, size_t n, int corrupt)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum += payload[i];
	}
	uint8_t hdr[4] = { 0x02, op, (uint8_t)(sum + (corrupt ? 1 : 0)), 0x03 };
	push(hdr, 4);
	push(payload, n);
}

static void push_block(uint32_t addr, uint8_t fill, int corrupt)
{
	uint8_t p[8 + 256];
	p[0] = addr >> 24;
	p[1] = addr >> 16;
	p[2] = addr >> 8;
	p[3] = addr;
	p[4] = p[5] = p[6] = 0;
	p[7] = 0x40;
	memset(p + 8, fill, 256);
	push_frame(0x18, p, sizeof p, corrupt);
}

static void push_start(uint32_t addr)
{
	uint8_t p[4] = { addr >> 24, addr >> 16, addr >> 8, addr };
	push_frame(0x1B, p, 4, 0);
}

static size_t count(const char *pat)
{
	size_t n = 0;
	for (size_t i = 0; i + 4 <= tx_len; i++) {
		n += memcmp(txq + i, pat, 4) == 0;
	}
	return n;
}

static void run(void)
{
	if (setjmp(done) == 0) {
		boot_main();
	}
}

// What stubtool does: answer the handshake, two blocks, start
static void test_load(void)
{
	reset(3);
	push("\x02\x14\x00\x03", 4);
	push_block(0x10000, 0xA5, 0);
	push_block(0x10100, 0x5A, 0);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK(ram[0x10000] == 0xA5 && ram[0x100FF] == 0xA5 && ram[0x10100] == 0x5A && ram[0x101FF] == 0x5A, "RAM content");
	CHECK(count("\x02\x05\x00\x03") == 3, "%zu acks, want 3 (two blocks, the start)", count("\x02\x05\x00\x03"));
	CHECK(wdt_en == 0, "watchdog left on before the jump");
	CHECK((gpiob_dr & (1u << 13)) && (gpiob_ddr & (1u << 13)), "power latch not driven high");
	CHECK(dll == 0, "UART reprogrammed although the loader had set it up");
}

// A block with a bad sum, one outside RAM, one over this stage's stack and an unknown op: dropped without an ack
static void test_drops(void)
{
	reset(3);
	push("\x02\x14\x00\x03", 4);
	push_block(0x10000, 0x11, 1);
	push_block(0x60000, 0x22, 0);
	push_block(0x4E000, 0x33, 0);
	push_frame(0x42, (const uint8_t *)"\0\0\0\0", 4, 0);
	push_block(0x10000, 0x44, 0);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK(ram[0x10000] == 0x44, "the good block after the bad ones is stored (0x%02X)", ram[0x10000]);
	CHECK(ram[0x4E000] == 0, "a block over the stage's stack was stored");
	CHECK(count("\x02\x05\x00\x03") == 2, "%zu acks, want 2", count("\x02\x05\x00\x03"));
}

// Nobody answers: BOOT_WINDOW offers, then the application
static void test_window(void)
{
	reset(3);
	run();
	CHECK(resumed, "the application was not started");
	CHECK(count("\x02\x24\x00\x03") == BOOT_WINDOW, "%zu handshakes, want %d", count("\x02\x24\x00\x03"), BOOT_WINDOW);
}

// A UART that never takes a byte: straight on to the application
static void test_dead_uart(void)
{
	reset(3);
	uart_dead = 1;
	run();
	CHECK(resumed, "the application was not started");
	CHECK(tx_len == 0, "bytes sent on a dead UART");
}

// A cold start (UART untouched by the loader): clocks, pins and 115200 at the loader's 36 MHz APB
static void test_cold_setup(void)
{
	reset(0);
	run();
	CHECK(resumed, "the application was not started");
	CHECK(dll == 19, "divisor %u, want 19", (unsigned)dll);
	CHECK(lcr == 3, "LCR 0x%X, want 8N1", (unsigned)lcr);
	CHECK((diplex0 & 0x600u) == 0, "UART1 pins still GPIO");
	CHECK(gates == 0xFFFFFFFFu, "clocks not enabled");
}

// Left keyed by a soft reset mid-transmission (DB25-D, 2026-10-21): the TX supplies and PA selects come out low and
// driven, the receive supply high, and nothing else in the banks changes. The stage then waits for a host, so this must
// not depend on the window running out
static void test_tx_off(void)
{
	reset(3);
	const uint32_t tx_a = (1u << 26) | (1u << 27), rx_a = 1u << 23, tx_b = (1u << 2) | (1u << 3);
	gpioa_dr = tx_a | 0x5u;		// keyed, receive supply low, two unrelated pins high
	gpioa_ddr = tx_a | rx_a | 0x5u;
	gpiob_dr = tx_b | 0x10u;
	gpiob_ddr = tx_b | 0x10u;
	push("\x02\x14\x00\x03", 4);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK((gpioa_dr & tx_a) == 0 && (gpiob_dr & tx_b) == 0, "transmitter pins left high: A 0x%08X B 0x%08X",
	      (unsigned)gpioa_dr, (unsigned)gpiob_dr);
	CHECK((gpioa_ddr & (tx_a | rx_a)) == (tx_a | rx_a) && (gpiob_ddr & tx_b) == tx_b, "transmitter pins not outputs");
	CHECK(gpioa_dr & rx_a, "receive supply not high");
	CHECK((gpioa_dr & 0x5u) == 0x5u && (gpiob_dr & 0x10u), "unrelated pins changed");
	CHECK(gpiob_dr & (1u << 13), "power latch not driven high");
}

// A false start of the answer, then the real one
static void test_handshake_resync(void)
{
	reset(3);
	push("\x02\x02\x14\x00\x03", 5);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
}

int main(void)
{
	test_load();
	test_drops();
	test_window();
	test_dead_uart();
	test_cold_setup();
	test_handshake_resync();
	test_tx_off();
	printf("bootDb25dTest: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
