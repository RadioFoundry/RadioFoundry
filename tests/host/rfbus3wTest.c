// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 3-wire bit-banged bus (drivers/rf/rfbus): frames on a model chip that samples the data line on each rising clock
// edge and answers reads on the falling ones, in the normal and the fast timing. Time moves only in the bus's waits,
// the worst case for a fast MCU: the fast bus must still leave 1 us between the address's last falling edge and the
// next rising one (the BK4819 class's tNXT), and must wait nowhere else in a write
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../../drivers/rf/rfbus/rfbus.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static struct {
	bool cs, clk, data, driven;
	uint32_t now_ns;		// advanced by the waits only
	uint32_t waits;			// delay_us calls
	uint32_t bits, shift;		// rising edges and what they sampled, this frame
	uint32_t fall8_ns;		// the 8th bit's falling edge
	uint32_t gap_ns;		// from it to the 9th rising edge
	uint16_t reply;			// what the chip answers a read with
	bool reading;
	uint32_t frames;
	uint32_t last;			// the last frame's 24 bits (a read's: the address only)
} m;

static void cs(bool v)
{
	if (!v && m.cs) {
		m.bits = 0;
		m.shift = 0;
		m.reading = false;
		m.gap_ns = 0;
	} else if (v && !m.cs) {
		m.frames++;
		m.last = m.shift;
	}
	m.cs = v;
}

static void clk(bool v)
{
	if (!m.cs) {
		if (v && !m.clk) {
			if (m.bits == 8) {
				m.gap_ns = m.now_ns - m.fall8_ns;
			}
			if (!m.reading) {
				m.shift = m.shift << 1 | (m.data ? 1u : 0u);
			}
			m.bits++;
			if (m.bits == 8 && (m.shift & 0x80u) != 0) {
				m.reading = true;
			}
		} else if (!v && m.clk) {
			if (m.bits == 8) {
				m.fall8_ns = m.now_ns;
			}
			if (m.reading && m.bits >= 8 && m.bits < 24) {
				m.data = ((m.reply >> (23u - m.bits)) & 1u) != 0;	// the chip drives the next bit
			}
		}
	}
	m.clk = v;
}

static void data_out(bool v) { m.data = v; m.driven = true; }
static void data_release(void) { m.driven = false; }
static bool data_in(void) { return m.data; }
static void delay_us(uint32_t us) { m.now_ns += us * 1000u; m.waits++; }

static void run(bool fast)
{
	rfbus_3w_pins_t pins = { cs, clk, data_out, data_release, data_in, delay_us, NULL, NULL, NULL, fast };
	rfbus_3w_t b;
	m.cs = true;
	m.frames = 0;
	rfbus_3w_attach(&b, &pins);

	m.waits = 0;
	rfbus_write(&b.bus, 0x30, 0xA5C3);
	CHECK(m.frames == 1);
	CHECK(m.last == (0x30u << 16 | 0xA5C3u));
	CHECK(m.gap_ns >= 1000u);
	CHECK(m.waits == (fast ? 1u : 48u));
	CHECK(b.bus.writes == 1);

	rfbus_write(&b.bus, 0xFF, 0x0001);	// a write never sets the read flag
	CHECK(m.last == (0x7Fu << 16 | 0x0001u));

	m.reply = 0x5A3C;
	uint16_t v = rfbus_read(&b.bus, 0x44);
	CHECK(v == 0x5A3C);
	CHECK((m.last & 0xFFu) == (0x44u | 0x80u));
	CHECK(m.gap_ns >= 1000u);
	CHECK(!m.driven);
}

int main(void)
{
	run(false);
	run(true);
	if (failures) {
		printf("rfbus3wTest: %d failures\n", failures);
		return 1;
	}
	printf("rfbus3wTest: all passed\n");
	return 0;
}
