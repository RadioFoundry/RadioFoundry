// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MD-380's bit-banged RF bus (boards/md380/rfbus_md380.c, built against the real pins.h and a stub platform.h)
// against fake chips wired as the hardware is (hardware-md380.md 4.2, 5.2): an HR_C5000 on C13 / E5 / E3 / E2 and a
// SKY72310 on E4 / E5 / D11. The wiring below is written out here, not taken from pins.h, so a wrong pin in pins.h or a
// wrong edge in the bit-bang code fails the test (issue #3: the clock on PE3, MISO on PE4, the SKY clock never toggled).
// This checks the code against the documented wiring, not the wiring itself: the HIL test (test_md380_rfbus.py) does that
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "rfbus_md380.h"
#include "modem/hrc5000/hrc5000.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

GPIO_TypeDef fake_ports[5];
int fake_irq_depth;

// --- The documented wiring: port letter, pin number ---
enum { PA, PB, PC, PD, PE };
typedef struct {
	int port, pin;
} wire_t;
static const wire_t C5000_CS = { PE, 2 }, C5000_CLK = { PC, 13 }, C5000_MOSI = { PE, 5 }, C5000_MISO = { PE, 3 };
static const wire_t SKY_CS = { PD, 11 }, SKY_CLK = { PE, 4 }, SKY_DATA = { PE, 5 };
static const wire_t VCO_SEL = { PA, 9 };

static bool level[5][16];
static int errors;		// protocol errors the fake chips flagged
static const char *first_error;

#define FLAG(msg) do { if (errors++ == 0) { first_error = msg; } } while (0)

static bool lv(wire_t w) { return level[w.port][w.pin]; }
static bool is(wire_t w, int port, int pin) { return w.port == port && w.pin == pin; }

// --- Fake HR_C5000 configuration port ---
static struct {
	uint8_t regs[8][256];
	int nbits;		// falling edges since CS fell
	uint8_t shift;
	uint8_t hdr[2];
	uint8_t out;		// the byte being shifted out on MISO
	bool read;
	int edges;		// every edge on its CS and clock
	int frames, bad_frames;
} c5000;

static void c5000_miso(bool bit) { level[C5000_MISO.port][C5000_MISO.pin] = bit; }

static uint8_t c5000_next_out(int byte)
{
	if (!c5000.read || byte < 2) {
		return 0;
	}
	return c5000.regs[c5000.hdr[0] & 7u][(uint8_t)(c5000.hdr[1] + byte - 2)];
}

static void c5000_edge(wire_t w, bool v)
{
	c5000.edges++;
	bool cs_low = !lv(C5000_CS);
	if (is(w, C5000_CS.port, C5000_CS.pin)) {
		if (!v) {
			if (lv(C5000_CLK)) {
				FLAG("C5000: CS fell with the clock high");
			}
			c5000.nbits = 0;
			c5000.read = false;
			c5000_miso(false);
		} else {
			if (c5000.nbits % 8 != 0 || c5000.nbits < 24) {
				c5000.bad_frames++;
				FLAG("C5000: a frame that is not whole bytes or has no data");
			}
			if (lv(C5000_CLK)) {
				FLAG("C5000: CS rose with the clock high");
			}
			c5000.frames++;
			c5000_miso(false);
		}
		return;
	}
	// the clock
	if (!cs_low) {
		FLAG("C5000: clock edge while CS is high");
		return;
	}
	if (v) {
		return;
	}
	// falling edge: sample MOSI, then drive the next MISO bit
	c5000.shift = (uint8_t)((c5000.shift << 1) | (lv(C5000_MOSI) ? 1u : 0u));
	int i = c5000.nbits % 8, byte = c5000.nbits / 8;
	c5000.nbits++;
	if (i == 7) {
		if (byte < 2) {
			c5000.hdr[byte] = c5000.shift;
			if (byte == 1) {
				c5000.read = (c5000.hdr[0] & 0x80u) != 0;
			}
		} else if (!c5000.read) {
			c5000.regs[c5000.hdr[0] & 7u][(uint8_t)(c5000.hdr[1] + byte - 2)] = c5000.shift;
		}
	}
	// the bit driven in answer to this edge is bit 7-i of the byte that edge belongs to
	if (i == 0) {
		c5000.out = c5000_next_out(byte);
	}
	c5000_miso(((c5000.out >> (7 - i)) & 1u) != 0);
}

// --- Fake SKY72310 ---
static struct {
	int nbits;
	uint16_t word;
	uint16_t words[64];
	int nwords;
	int edges;		// every edge on its CS and clock
} sky;

static void sky_edge(wire_t w, bool v)
{
	sky.edges++;
	bool cs_low = !lv(SKY_CS);
	if (is(w, SKY_CS.port, SKY_CS.pin)) {
		if (!v) {
			if (!lv(SKY_CLK)) {
				FLAG("SKY: CS fell with the clock low");
			}
			sky.nbits = 0;
			sky.word = 0;
		} else {
			if (sky.nbits != 16) {
				FLAG("SKY: a word that is not 16 bits");
			} else if (sky.nwords < 64) {
				sky.words[sky.nwords++] = sky.word;
			}
			if (!lv(SKY_CLK)) {
				FLAG("SKY: CS rose with the clock low");
			}
		}
		return;
	}
	if (!cs_low) {
		FLAG("SKY: clock edge while CS is high");
		return;
	}
	if (v) {
		sky.word = (uint16_t)((sky.word << 1) | (lv(SKY_DATA) ? 1u : 0u));
		sky.nbits++;
	}
}

// Data must not move while the SKY's clock is low and its CS is low
static void sky_data_edge(void)
{
	if (!lv(SKY_CS) && !lv(SKY_CLK)) {
		FLAG("SKY: data changed while the clock was low");
	}
}

// --- The fake GPIO ---
static int wire_port(GPIO_TypeDef *port)
{
	return (int)(port - fake_ports);
}

void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool v)
{
	int p = wire_port(port), n = -1;
	for (int i = 0; i < 16; i++) {
		if (pins == (1u << i)) {
			n = i;
		}
	}
	if (n < 0) {
		FLAG("a write with a pin mask that is not one pin");
		return;
	}
	if (p == C5000_MISO.port && n == C5000_MISO.pin) {
		FLAG("the MCU drove the C5000's MISO output");
		return;
	}
	wire_t w = { p, n };
	if (!(is(w, C5000_CS.port, C5000_CS.pin) || is(w, C5000_CLK.port, C5000_CLK.pin) ||
	      is(w, C5000_MOSI.port, C5000_MOSI.pin) || is(w, SKY_CS.port, SKY_CS.pin) ||
	      is(w, SKY_CLK.port, SKY_CLK.pin) || is(w, VCO_SEL.port, VCO_SEL.pin))) {
		FLAG("a write to a pin that has nothing on the bus");
	}
	if (level[p][n] == v) {
		return;
	}
	if (is(w, C5000_MOSI.port, C5000_MOSI.pin)) {	// the data line is shared by both chips
		sky_data_edge();
	}
	level[p][n] = v;
	if (is(w, C5000_CS.port, C5000_CS.pin) || is(w, C5000_CLK.port, C5000_CLK.pin)) {
		c5000_edge(w, v);
	}
	if (is(w, SKY_CS.port, SKY_CS.pin) || is(w, SKY_CLK.port, SKY_CLK.pin)) {
		sky_edge(w, v);
	}
}

bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	for (int i = 0; i < 16; i++) {
		if (pin == (1u << i)) {
			return level[wire_port(port)][i];
		}
	}
	FLAG("a read with a pin mask that is not one pin");
	return false;
}

// --- Helpers ---
static void bus_reset(void)
{
	memset(level, 0, sizeof(level));
	memset(&c5000, 0, sizeof(c5000));
	memset(&sky, 0, sizeof(sky));
	errors = 0;
	first_error = NULL;
	// The levels board_init leaves: both chip selects high, the C5000's clock low, the SKY's clock high
	level[C5000_CS.port][C5000_CS.pin] = true;
	level[SKY_CS.port][SKY_CS.pin] = true;
	level[SKY_CLK.port][SKY_CLK.pin] = true;
}

static void check_idle(const char *what)
{
	if (lv(C5000_CLK) || !lv(SKY_CLK) || !lv(C5000_CS) || !lv(SKY_CS)) {
		printf("FAIL idle levels after %s: C13=%d E4=%d E2=%d D11=%d\n", what, lv(C5000_CLK), lv(SKY_CLK),
		       lv(C5000_CS), lv(SKY_CS));
		failures++;
	}
}

static void check_clean(const char *what)
{
	if (errors != 0) {
		printf("FAIL %s: %d protocol errors, first: %s\n", what, errors, first_error);
		failures++;
		errors = 0;
	}
}

static bool xfer(const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	return md380_rfbus_modem_transfer(NULL, hdr, hdr_len, tx, rx, len);
}

// --- Tests ---
static void test_c5000_registers(void)
{
	bus_reset();
	static const struct { uint8_t page, reg, val; } rows[] = {
		{ 0, 0x10, 0x80 }, { 0, 0x00, 0xFF }, { 0, 0xA1, 0x55 }, { 0, 0x37, 0xAA }, { 0, 0xFF, 0x01 },
		{ 1, 0x10, 0x69 }, { 1, 0x52, 0x08 }, { 1, 0x7F, 0xCC }, { 1, 0x00, 0x00 }, { 1, 0xE1, 0x96 },
	};
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		uint8_t hdr[2] = { rows[i].page, rows[i].reg };
		uint8_t val = rows[i].val;
		CHECK(xfer(hdr, 2, &val, NULL, 1));
		CHECK(c5000.regs[rows[i].page][rows[i].reg] == rows[i].val);
		check_idle("a C5000 write");
	}
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		uint8_t hdr[2] = { (uint8_t)(0x80u | rows[i].page), rows[i].reg };
		uint8_t got = 0x5A;
		CHECK(xfer(hdr, 2, NULL, &got, 1));
		if (got != rows[i].val) {
			printf("FAIL read page %u reg 0x%02X: got 0x%02X, want 0x%02X\n", rows[i].page, rows[i].reg, got,
			       rows[i].val);
			failures++;
		}
		check_idle("a C5000 read");
	}
	// A burst: write 6 bytes, read them back, next to registers that must stay as they were
	uint8_t burst[6] = { 0x01, 0x80, 0xFE, 0x7F, 0x00, 0xFF }, back[6] = { 0 }, hdr[2] = { 1, 0x30 };
	CHECK(xfer(hdr, 2, burst, NULL, 6));
	hdr[0] = 0x81;
	CHECK(xfer(hdr, 2, NULL, back, 6));
	CHECK(memcmp(burst, back, 6) == 0);
	CHECK(c5000.regs[1][0x2F] == 0 && c5000.regs[1][0x36] == 0);
	// The whole byte range of one register, to see every bit position on both lines
	for (int v = 0; v < 256; v++) {
		uint8_t h[2] = { 0, 0x20 }, val = (uint8_t)v, got = 0;
		CHECK(xfer(h, 2, &val, NULL, 1));
		h[0] = 0x80;
		CHECK(xfer(h, 2, NULL, &got, 1));
		if (got != v) {
			printf("FAIL register round trip of 0x%02X gave 0x%02X\n", v, got);
			failures++;
			break;
		}
	}
	check_clean("C5000 registers");
	CHECK(c5000.bad_frames == 0);
	CHECK(sky.edges == 0);	// the C5000's traffic never clocked the SKY
	CHECK(sky.nwords == 0);
}

static void test_c5000_driver(void)
{
	bus_reset();
	static const hrc_bus_t bus = { .transfer = md380_rfbus_modem_transfer };
	hrc5000_t chip;
	hrc5000_attach(&chip, &bus, false);
	hrc_t *dev = hrc5000_hrc(&chip);
	uint8_t v = 0;
	CHECK(hrc_write(dev, 4, 0x10, 0x80));
	CHECK(hrc_read(dev, 4, 0x10, &v));
	CHECK(v == 0x80);
	CHECK(c5000.regs[0][0x10] == 0x80);	// the C5000's configuration page is 0 on the wire
	CHECK(hrc_write(dev, 1, 0x52, 0x08));
	CHECK(hrc_read(dev, 1, 0x52, &v));
	CHECK(v == 0x08);
	CHECK(c5000.regs[1][0x52] == 0x08);
	static const uint8_t blk[] = { 0x69, 0x69, 0x96, 0x96, 0x96, 0x99, 0x99, 0x99 };
	uint8_t back[sizeof(blk)];
	CHECK(hrc_write_burst(dev, 1, 0x10, blk, sizeof(blk)));
	CHECK(hrc_read_burst(dev, 1, 0x10, back, sizeof(back)));
	CHECK(memcmp(blk, back, sizeof(blk)) == 0);
	check_idle("the driver");
	check_clean("the driver");
	CHECK(sky.edges == 0);
}

static void test_pll_words(void)
{
	bus_reset();
	static const uint16_t words[] = { 0x8000, 0x73D0, 0x4FFF + 3, 0xAAAA, 0x5555, 0x0000, 0xFFFF, 0x0001, 0x1234 };
	int n = (int)(sizeof(words) / sizeof(words[0]));
	for (int i = 0; i < n; i++) {
		CHECK(md380_rfbus_pll_write(NULL, words[i]));
		check_idle("a SKY word");
		if (sky.nwords != i + 1 || sky.words[i] != words[i]) {
			printf("FAIL SKY word %d: want 0x%04X, got %d words, last 0x%04X\n", i, words[i], sky.nwords,
			       sky.nwords > 0 ? sky.words[sky.nwords - 1] : 0);
			failures++;
			break;
		}
	}
	CHECK(sky.nwords == n);
	check_clean("SKY words");
	CHECK(c5000.edges == 0);	// the SKY's traffic never clocked the C5000
	CHECK(c5000.frames == 0);
}

static void test_interleaved(void)
{
	// The shared data line: a C5000 transaction between two SKY words disturbs neither
	bus_reset();
	uint8_t hdr[2] = { 0, 0x10 }, val = 0xA5, got = 0;
	CHECK(md380_rfbus_pll_write(NULL, 0xAAAA));
	CHECK(xfer(hdr, 2, &val, NULL, 1));
	CHECK(md380_rfbus_pll_write(NULL, 0x5555));
	hdr[0] = 0x80;
	CHECK(xfer(hdr, 2, NULL, &got, 1));
	CHECK(got == 0xA5);
	CHECK(sky.nwords == 2 && sky.words[0] == 0xAAAA && sky.words[1] == 0x5555);
	CHECK(c5000.frames == 2);
	check_idle("interleaved traffic");
	check_clean("interleaved traffic");
}

static void test_lock_and_vco(void)
{
	bus_reset();
	CHECK(fake_irq_depth == 0);	// every lock was unlocked
	md380_rfbus_vco_select(false);
	CHECK(lv(VCO_SEL));		// the receive VCO: high
	md380_rfbus_vco_select(true);
	CHECK(!lv(VCO_SEL));		// the transmit VCO: low
	md380_rfbus_vco_select(false);
	CHECK(lv(VCO_SEL));
	check_clean("the VCO select");
	CHECK(c5000.edges == 0 && sky.edges == 0);
	// Lock detect: PD10, high = locked
	level[PD][10] = true;
	CHECK(md380_rfbus_pll_locked(NULL));
	level[PD][10] = false;
	CHECK(!md380_rfbus_pll_locked(NULL));
}

int main(void)
{
	test_c5000_registers();
	test_c5000_driver();
	test_pll_words();
	test_interleaved();
	test_lock_and_vco();
	printf("md380RfbusTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
