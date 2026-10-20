// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The bit-banged SPI master (drivers/bus/spi_gpio) against a model of a slave on fake pins, in all four modes: the
// slave takes MOSI on its sampling edge and puts its next bit out on the other edge, as the mode says; every clock
// edge is counted and the clock must end at its idle level
#include <stdio.h>
#include <string.h>
#include "bus/spi_gpio/spi_gpio.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static struct {
	bool cpol, cpha;
	bool sck, mosi, miso;
	uint8_t rx_shift, tx_byte;
	int bits;
	int edges;
	uint8_t got[16];
	int got_n;
	const uint8_t *reply;
	int reply_n, reply_i;
	int delays;
} s;

static void slave_next_out(void)
{
	uint8_t b = s.reply_i < s.reply_n ? s.reply[s.reply_i] : 0;
	s.miso = (b >> (7 - (s.bits % 8))) & 1u;
}

static void sck(bool high)
{
	if (high == s.sck) {
		return;
	}
	s.sck = high;
	s.edges++;
	bool leading = high != s.cpol;
	if (leading == !s.cpha) {
		s.rx_shift = (uint8_t)((s.rx_shift << 1) | (s.mosi ? 1u : 0u));	// the sampling edge
		s.bits++;
		if (s.bits % 8 == 0) {
			s.got[s.got_n++] = s.rx_shift;
			s.reply_i++;
		}
	} else {
		slave_next_out();	// the other edge: the next bit goes out
	}
}

static void mosi(bool high) { s.mosi = high; }
static bool miso(void) { return s.miso; }
static void delay(void) { s.delays++; }

static void reset(bool cpol, bool cpha, const uint8_t *reply, int n)
{
	memset(&s, 0, sizeof s);
	s.cpol = cpol;
	s.cpha = cpha;
	s.sck = cpol;
	s.reply = reply;
	s.reply_n = n;
	if (!cpha) {
		slave_next_out();		// CPHA 0: the first bit is out as soon as the slave is selected
	}
}

static void mode(bool cpol, bool cpha)
{
	static const uint8_t reply[3] = { 0xC2, 0x20, 0x16 };
	static const uint8_t cmd[3] = { 0x9F, 0x00, 0xA5 };
	spi_gpio_t b = { .sck = sck, .mosi = mosi, .miso = miso, .delay = delay, .cpol = cpol, .cpha = cpha };
	reset(cpol, cpha, reply, 3);
	spi_gpio_idle(&b);
	uint8_t in[3];
	spi_gpio_xfer(&b, cmd, in, 3);
	CHECK(s.got_n == 3);
	CHECK(memcmp(s.got, cmd, 3) == 0);
	CHECK(memcmp(in, reply, 3) == 0);
	CHECK(s.edges == 3 * 16);
	CHECK(s.sck == cpol);
	CHECK(s.delays == 3 * 16);
}

int main(void)
{
	mode(false, false);
	mode(false, true);
	mode(true, false);
	mode(true, true);

	// tx NULL sends 0xFF; rx NULL discards; no MISO reads 0; no delay hook is fine
	spi_gpio_t w = { .sck = sck, .mosi = mosi, .cpol = true, .cpha = true };
	reset(true, true, NULL, 0);
	spi_gpio_xfer(&w, NULL, NULL, 2);
	CHECK(s.got_n == 2 && s.got[0] == 0xFF && s.got[1] == 0xFF);
	CHECK(spi_gpio_byte(&w, 0x5A) == 0x00);
	CHECK(s.got[2] == 0x5A);
	CHECK(s.delays == 0);

	if (failures) {
		printf("spiGpioTest: %d failures\n", failures);
		return 1;
	}
	printf("spiGpioTest: all passed\n");
	return 0;
}
