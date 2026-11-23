// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000 power-on clock step with each preset (drivers/modem/hrc6000; modem-hrc6000.md 1.2, 3 step 1): run from
// XTALI, the PLL words, then the PLL and the three dividers, in that order, before anything else is written
#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

typedef struct {
	uint8_t page, reg, val;
} wr_t;

static wr_t log_w[4096];
static int n_w;
static uint32_t waited_us;

static bool transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	if (rx != NULL) {
		memset(rx, 0, len);
	}
	if (hdr_len == 2 && (hdr[0] & 0x80u) == 0 && tx != NULL && len == 1 && n_w < 4096) {
		log_w[n_w++] = (wr_t){ hdr[0], hdr[1], tx[0] };
	}
	return true;
}

static void delay_ms(uint32_t ms) { waited_us += ms * 1000u; }
static void delay_us(uint32_t us) { waited_us += us; }

static const hrc_bus_t bus = { .transfer = transfer, .delay_ms = delay_ms, .delay_us = delay_us };

static void expect_clock(const uint8_t m, const uint8_t n)
{
	static const struct { uint8_t reg, val; } want_tail[] = { { 0x0A, 0x00 }, { 0xB9, 0x05 }, { 0xBA, 0x04 }, { 0xBB, 0x02 } };
	CHECK(n_w > 7);
	CHECK(log_w[0].page == 4 && log_w[0].reg == 0x0A && log_w[0].val == 0x80);
	CHECK(log_w[1].page == 4 && log_w[1].reg == 0x0B && log_w[1].val == m);
	CHECK(log_w[2].page == 4 && log_w[2].reg == 0x0C && log_w[2].val == n);
	for (int i = 0; i < 4; i++) {
		CHECK(log_w[3 + i].page == 4 && log_w[3 + i].reg == want_tail[i].reg && log_w[3 + i].val == want_tail[i].val);
	}
	CHECK(waited_us >= 500u);	// the PLL settles before the switch
}

int main(void)
{
	hrc_t dev;
	hrc6000_attach(&dev, &bus);

	n_w = 0;
	waited_us = 0;
	CHECK(hrc6000_power_on_init_clock(&dev, 7, &hrc6000_clock_12m288));
	expect_clock(0x40, 0x32);
	int n_12m = n_w;

	// The plain form is the 29.4912 MHz preset, and the rest of the sequence is the same length
	n_w = 0;
	waited_us = 0;
	CHECK(hrc6000_power_on_init(&dev, 7));
	expect_clock(0x28, 0x33);
	CHECK(n_w == n_12m);

	if (failures) {
		printf("hrc6000ClockTest: %d failures\n", failures);
		return 1;
	}
	printf("hrc6000ClockTest: all passed\n");
	return 0;
}
