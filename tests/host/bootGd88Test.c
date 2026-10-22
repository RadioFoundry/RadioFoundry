// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The GD-88 first stages against a simulated UART and GPIO: built twice. A (boot/gd88/boot.c, -DSOC_B=0): UART1, the
// power latch, the RF serial chain cleared to all zero before anything else. B (boot/gd88b/boot.c, -DSOC_B=1): UART3,
// the transmitter pins off, the long window its application asks for with the STAY word, and that word cleared.
// The ROM protocol itself is boot/hrc7000/stage.c, tested in depth by bootDb25dTest.
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_HOST_TEST
#define BOOT_WINDOW	3

#if SOC_B
#define U	0x14090000u
#else
#define U	0x14040000u
#endif

static uint8_t ram[0x50000];
static uint32_t stay_word;	// RAM 0x4FFF8, as a word
static uint8_t rxq[8192];
static size_t rx_len, rx_pos;
static uint8_t txq[1 << 16];
static size_t tx_len;
static uint32_t lcr, dll, diplex0, gates, wdt_en = 1;
static uint32_t dr[3], ddr[3];	// GPIO A, B, C
static jmp_buf done;
static uint32_t jumped_to;
static int resumed;
static unsigned offers_seen;
static unsigned rx_after_offers;	// bytes become visible only once this many handshakes went out
// The serial chain as a shift register: bits shifted in on each clock rise, copied out on the latch rise
static uint64_t chain_shift, chain_out;
static unsigned chain_clocks;
static int chain_set;

static int bank(uint32_t a)
{
	switch (a & ~0xFu) {
	case 0x14020000u:
		return 0;
	case 0x14100000u:
		return 1;
	case 0x14110000u:
		return 2;
	default:
		return -1;
	}
}

static uint32_t rd(uint32_t a)
{
	int b = bank(a);
	if (b >= 0) {
		return (a & 0xFu) == 0 ? dr[b] : ddr[b];
	}
	switch (a) {
	case U:
		return rx_pos < rx_len ? rxq[rx_pos++] : 0;
	case U + 0x0Cu:
		return lcr;
	case U + 0x14u:
		return 0x40u | (rx_pos < rx_len && offers_seen >= rx_after_offers ? 1u : 0u);
	case U + 0x7Cu:
		return 2u;
	case 0x11000034u:
		return diplex0;
	case 0x0004FFF8u:
		return stay_word;
	default:
		return 0;
	}
}

static void wr(uint32_t a, uint32_t v)
{
	int b = bank(a);
	if (b == 1 && (a & 0xFu) == 0) {
		uint32_t was = dr[1];
		if ((v & 1u) && !(was & 1u)) {			// PTB0 rises: PTB1 shifts in
			chain_shift = chain_shift << 1 | ((v >> 1) & 1u);
			chain_clocks++;
		}
		if ((v & 8u) && !(was & 8u)) {			// PTB3 rises: to the outputs
			chain_out = chain_shift;
			chain_set = 1;
		}
	}
	if (b >= 0) {
		if ((a & 0xFu) == 0) {
			dr[b] = v;
		} else {
			ddr[b] = v;
		}
		return;
	}
	switch (a) {
	case U:
		if (lcr & 0x80u) {
			dll = v;
		} else if (tx_len < sizeof txq) {
			txq[tx_len++] = (uint8_t)v;
			if (tx_len >= 4 && memcmp(txq + tx_len - 4, "\x02\x24\x00\x03", 4) == 0) {
				offers_seen++;
			}
		}
		break;
	case U + 0x0Cu:
		lcr = v;
		break;
	case 0x1100002Cu:
		gates = v;
		break;
	case 0x11000034u:
		diplex0 = v;
		break;
	case 0x14010010u:
		wdt_en = v;
		break;
	case 0x0004FFF8u:
		stay_word = v;
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

#if SOC_B
#include "../../boot/gd88b/boot.c"
#define NAME "bootGd88bTest"
#else
#include "../../boot/gd88/boot.c"
#define NAME "bootGd88Test"
#endif

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void reset(uint32_t lcr0)
{
	memset(ram, 0, sizeof ram);
	rx_len = rx_pos = tx_len = 0;
	lcr = lcr0;
	dll = gates = 0;
	diplex0 = SOC_B ? 0 : 0x600u;
	memset(dr, 0, sizeof dr);
	memset(ddr, 0, sizeof ddr);
	wdt_en = 1;
	jumped_to = 0;
	resumed = 0;
	offers_seen = 0;
	rx_after_offers = 1;
	stay_word = 0;
	chain_shift = ~0ull;
	chain_out = ~0ull;
	chain_clocks = 0;
	chain_set = 0;
}

static void push(const void *p, size_t n)
{
	memcpy(rxq + rx_len, p, n);
	rx_len += n;
}

static void push_frame(uint8_t op, const uint8_t *payload, size_t n)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum += payload[i];
	}
	uint8_t hdr[4] = { 0x02, op, sum, 0x03 };
	push(hdr, 4);
	push(payload, n);
}

static void push_block(uint32_t addr, uint8_t fill)
{
	uint8_t p[8 + 256];
	p[0] = addr >> 24;
	p[1] = addr >> 16;
	p[2] = addr >> 8;
	p[3] = addr;
	p[4] = p[5] = p[6] = 0;
	p[7] = 0x40;
	memset(p + 8, fill, 256);
	push_frame(0x18, p, sizeof p);
}

static void push_start(uint32_t addr)
{
	uint8_t p[4] = { addr >> 24, addr >> 16, addr >> 8, addr };
	push_frame(0x1B, p, 4);
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

// The stub load on this SoC's UART
static void test_load(void)
{
	reset(3);
	push("\x02\x14\x00\x03", 4);
	push_block(0x10000, 0xA5);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK(ram[0x10000] == 0xA5 && ram[0x100FF] == 0xA5, "RAM content");
	CHECK(count("\x02\x05\x00\x03") == 2, "%zu acks, want 2", count("\x02\x05\x00\x03"));
	CHECK(wdt_en == 0, "watchdog left on before the jump");
}

// Nobody answers: BOOT_WINDOW offers, then the application
static void test_window(void)
{
	reset(3);
	run();
	CHECK(resumed, "the application was not started");
	CHECK(count("\x02\x24\x00\x03") == BOOT_WINDOW, "%zu handshakes, want %d", count("\x02\x24\x00\x03"), BOOT_WINDOW);
}

// A cold start: clocks, this UART's pins, 115200 at 36 MHz
static void test_cold_setup(void)
{
	reset(0);
	run();
	CHECK(resumed, "the application was not started");
	CHECK(dll == 19 && lcr == 3, "divisor %u LCR 0x%X", (unsigned)dll, (unsigned)lcr);
#if SOC_B
	CHECK((diplex0 & 0x6000u) == 0x6000u, "UART3 pins not given to UART3");
#else
	CHECK((diplex0 & 0x600u) == 0, "UART1 pins still GPIO");
#endif
	CHECK(gates == 0xFFFFFFFFu, "clocks not enabled");
}

#if SOC_B
// Left keyed by a soft reset: B's TX enables, TX tune and TX band pins come out low and driven, nothing else changes
static void test_tx_off(void)
{
	reset(3);
	const uint32_t a = 1u << 17, b = (1u << 20) | (1u << 21) | (1u << 6) | (1u << 3) | (1u << 10);
	dr[0] = a | 0x5u;
	ddr[0] = a | 0x5u;
	dr[1] = b | (1u << 13) | (1u << 22);	// PTB13 is an RF pair pin on B, PTB22 a switch that idles high
	ddr[1] = b | (1u << 13) | (1u << 22);
	push("\x02\x14\x00\x03", 4);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK((dr[0] & a) == 0 && (dr[1] & b) == 0, "transmitter pins left high: A 0x%08X B 0x%08X", (unsigned)dr[0],
	      (unsigned)dr[1]);
	CHECK((ddr[0] & a) == a && (ddr[1] & b) == b, "transmitter pins not outputs");
	CHECK((dr[0] & 0x5u) == 0x5u && (dr[1] & ((1u << 13) | (1u << 22))) == ((1u << 13) | (1u << 22)),
	      "unrelated pins changed");
}

// The application asked for the stub: the window runs past BOOT_WINDOW, the host answers late, and the word is cleared
static void test_stay(void)
{
	reset(3);
	stay_word = 0x53544159u;
	run();
	CHECK(resumed, "the application was not started after the long window");
	CHECK(count("\x02\x24\x00\x03") == 1200, "%zu handshakes, want 1200", count("\x02\x24\x00\x03"));
	CHECK(stay_word == 0, "STAY word left set");

	reset(3);
	stay_word = 0x53544159u;
	rx_after_offers = 50;		// the host answers through A after 2.5 s
	push("\x02\x14\x00\x03", 4);
	push_start(0x10000);
	run();
	CHECK(jumped_to == 0x10000, "jumped to 0x%X", (unsigned)jumped_to);
	CHECK(count("\x02\x24\x00\x03") == 50, "%zu handshakes, want 50", count("\x02\x24\x00\x03"));
	CHECK(stay_word == 0, "STAY word left set after the load");

	// Any other value: the normal window
	reset(3);
	stay_word = 0x12345678u;
	run();
	CHECK(resumed && count("\x02\x24\x00\x03") == BOOT_WINDOW, "%zu handshakes", count("\x02\x24\x00\x03"));
}
#else
// Left keyed by a soft reset: PTB2 (TX) goes low, the chain gets 64 zero bits and the latch, PTC20 goes low, the power
// latch high, before the first handshake
static void test_chain(void)
{
	reset(3);
	dr[1] = 1u << 2;
	ddr[1] = 1u << 2;
	dr[2] = (1u << 20) | 0x3u;
	ddr[2] = (1u << 20) | 0x3u;
	run();
	CHECK(chain_set && chain_out == 0, "chain not cleared (set %d, 0x%016llX)", chain_set, (unsigned long long)chain_out);
	CHECK(chain_clocks == 64, "%u clocks, want 64", chain_clocks);
	CHECK((dr[1] & 8u) && (ddr[1] & 0xBu) == 0xBu, "chain latch not left high or pins not outputs");
	CHECK((dr[2] & (1u << 20)) == 0 && (ddr[2] & (1u << 20)), "PTC20 not driven low");
	CHECK((dr[2] & 0x3u) == 0x3u, "unrelated pins changed");
	CHECK((dr[1] & (1u << 13)) && (ddr[1] & (1u << 13)), "power latch not driven high");
	CHECK((dr[1] & (1u << 2)) == 0 && (ddr[1] & (1u << 2)), "PTB2 (TX) not driven low");
}
#endif

int main(void)
{
	test_load();
	test_window();
	test_cold_setup();
#if SOC_B
	test_tx_off();
	test_stay();
#else
	test_chain();
#endif
	printf(NAME ": %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
