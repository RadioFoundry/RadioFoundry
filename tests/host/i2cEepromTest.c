// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// drivers/flash/i2c_eeprom against a model of a 24-series EEPROM at the I2C bus (the Microchip 24LC512 data sheet).
// The model keeps what the driver must get right: a write sets the address pointer from two bytes, data past the end
// of the page wraps to the start of the same page, the write cycle starts at the STOP and lasts a while, during which
// the device doesn't acknowledge its address, a read continues from the address pointer and wraps at the end of the
// memory. It can also fail the bus or stay busy forever.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash/i2c_eeprom/i2c_eeprom.h"
#include "hal_time.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- Time: the model's write cycle runs on this clock ----

static uint32_t now_ms;
void hal_delay_ms(uint32_t ms) { now_ms += ms; }
void hal_delay_us(uint32_t us) { (void)us; }
uint32_t hal_millis(void) { return now_ms; }

// ---- The device model ----

static struct {
	uint8_t mem[0x10000];
	uint32_t size;
	uint16_t page;
	uint8_t addr7;
	uint16_t ptr;			// address pointer
	uint32_t busy_until;		// write cycle end (ms); UINT32_MAX = stuck
	uint32_t cycle_ms;		// write cycle length
	uint32_t page_writes;		// write cycles started
	uint32_t wraps;			// data bytes that wrapped inside a page
	uint32_t nacks_busy;		// address not acknowledged during a write cycle
	uint32_t transactions, max_read;
	uint32_t fail_at;		// fail the n-th transaction from now (0 = never)
} dev;

static void dev_create(uint32_t size, uint16_t page)
{
	memset(&dev, 0, sizeof(dev));
	memset(dev.mem, 0xFF, sizeof(dev.mem));
	dev.size = size;
	dev.page = page;
	dev.addr7 = 0x50;
	dev.cycle_ms = 3;
}

static bool dev_busy(void)
{
	return dev.busy_until == UINT32_MAX || now_ms < dev.busy_until;
}

// Address phase: true when the device acknowledges
static bool dev_address(uint8_t addr7)
{
	dev.transactions++;
	if (dev.fail_at != 0 && --dev.fail_at == 0) {
		return false;
	}
	if (addr7 != dev.addr7) {
		return false;
	}
	if (dev_busy()) {
		dev.nacks_busy++;
		return false;
	}
	return true;
}

static bool bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n)
{
	(void)ctx;
	if (!dev_address(addr7)) {
		return false;
	}
	if (n < 2) {
		if (n == 1) {
			dev.ptr = (uint16_t)((data[0] << 8) | (dev.ptr & 0xFF));
		}
		return true;	// address only (a poll or a probe): no write cycle
	}
	dev.ptr = (uint16_t)(((data[0] << 8) | data[1]) % dev.size);
	if (n == 2) {
		return true;	// a dummy write sets the pointer only
	}
	uint32_t base = dev.ptr & ~(uint32_t)(dev.page - 1u);
	uint32_t off = dev.ptr & (dev.page - 1u);
	for (size_t i = 2; i < n; i++) {
		if (i - 2 >= dev.page - (dev.ptr & (dev.page - 1u))) {
			dev.wraps++;
		}
		dev.mem[base + off] = data[i];
		off = (off + 1u) & (dev.page - 1u);
	}
	dev.ptr = (uint16_t)(base + off);
	dev.page_writes++;
	dev.busy_until = dev.busy_until == UINT32_MAX ? UINT32_MAX : now_ms + dev.cycle_ms;
	return true;
}

static bool bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	(void)ctx;
	if (!dev_address(addr7)) {
		return false;
	}
	if (wn == 2) {
		dev.ptr = (uint16_t)(((w[0] << 8) | w[1]) % dev.size);
	}
	if (rn > dev.max_read) {
		dev.max_read = (uint32_t)rn;
	}
	for (size_t i = 0; i < rn; i++) {
		r[i] = dev.mem[dev.ptr];
		dev.ptr = (uint16_t)((dev.ptr + 1u) % dev.size);
	}
	return true;
}

static const i2c_bus_t bus = { .write = bus_write, .write_read = bus_write_read, .ctx = NULL };

static void fill(uint8_t *b, uint32_t n, uint8_t seed)
{
	for (uint32_t i = 0; i < n; i++) {
		b[i] = (uint8_t)(i * 31u + seed);
	}
}

// ---- Tests ----

static void test_probe(void)
{
	i2c_eeprom_t e = { .bus = &bus, .addr7 = 0x50, .size = 0x10000, .page = 64, .write_ms = 5 };
	dev_create(0x10000, 64);
	CHECK(i2c_eeprom_probe(&e));
	CHECK(dev.page_writes == 0);	// the probe writes nothing
	e.addr7 = 0x51;
	CHECK(!i2c_eeprom_probe(&e));
}

static void test_read_write(bool ack_poll)
{
	i2c_eeprom_t e = { .bus = &bus, .addr7 = 0x50, .size = 0x10000, .page = 64, .write_ms = 5, .ack_poll = ack_poll };
	uint8_t b[600], r[600];

	dev_create(0x10000, 64);
	fill(b, sizeof(b), 7);
	// Across pages: 0x30 into a page, 600 bytes = 16 + 9 x 64 + 8: eleven page writes, none wraps
	uint32_t t0 = now_ms;
	CHECK(i2c_eeprom_write(&e, 0x1230, b, sizeof(b)));
	CHECK(dev.page_writes == 11 && dev.wraps == 0);
	CHECK(memcmp(&dev.mem[0x1230], b, sizeof(b)) == 0);
	CHECK(dev.mem[0x122F] == 0xFF && dev.mem[0x1230 + sizeof(b)] == 0xFF);
	CHECK(now_ms - t0 >= 11u * dev.cycle_ms);	// each page waited out
	CHECK(ack_poll ? dev.nacks_busy > 0 : dev.nacks_busy == 0);
	// Read it back, across page boundaries, in one go and in odd pieces
	memset(r, 0, sizeof(r));
	CHECK(i2c_eeprom_read(&e, 0x1230, r, sizeof(r)) && memcmp(r, b, sizeof(r)) == 0);
	CHECK(i2c_eeprom_read(&e, 0x123F, r, 3) && memcmp(r, &b[0xF], 3) == 0);
	// A read straight after a write: the cycle is over, the device answers
	CHECK(i2c_eeprom_write(&e, 0x0000, b, 1) && i2c_eeprom_read(&e, 0x0000, r, 1) && r[0] == b[0]);
	// One byte at the last address; the range check at the end
	CHECK(i2c_eeprom_write(&e, 0xFFFF, b, 1) && dev.mem[0xFFFF] == b[0]);
	CHECK(!i2c_eeprom_write(&e, 0xFFFF, b, 2) && !i2c_eeprom_read(&e, 0xFFF0, r, 0x11));
	CHECK(i2c_eeprom_read(&e, 0x10000, r, 0) && i2c_eeprom_write(&e, 0x10000, b, 0));
	CHECK(!i2c_eeprom_read(&e, 0x10001, r, 0));
}

static void test_chunked_reads(void)
{
	i2c_eeprom_t e = { .bus = &bus, .addr7 = 0x50, .size = 0x10000, .page = 64, .write_ms = 5, .read_chunk = 64 };
	uint8_t b[300], r[300];
	dev_create(0x10000, 64);
	fill(b, sizeof(b), 3);
	memcpy(&dev.mem[0x7C00], b, sizeof(b));
	uint32_t t0 = dev.transactions;
	CHECK(i2c_eeprom_read(&e, 0x7C00, r, sizeof(r)) && memcmp(r, b, sizeof(r)) == 0);
	CHECK(dev.max_read == 64 && dev.transactions - t0 == 5);
}

// A smaller part, a larger page, bad configurations
static void test_geometry(void)
{
	uint8_t b[300];
	fill(b, sizeof(b), 9);
	i2c_eeprom_t e = { .bus = &bus, .addr7 = 0x50, .size = 0x8000, .page = 128, .write_ms = 5 };
	dev_create(0x8000, 128);
	CHECK(i2c_eeprom_write(&e, 0x7F00 + 100, b, 156));
	CHECK(dev.page_writes == 2 && dev.wraps == 0 && memcmp(&dev.mem[0x7F64], b, 156) == 0);
	CHECK(!i2c_eeprom_write(&e, 0x8000, b, 1));
	// Pages that aren't a power of two or too large, a size over 64 KB: refused before the bus
	uint32_t t0 = dev.transactions;
	e.page = 96;
	CHECK(!i2c_eeprom_write(&e, 0, b, 1));
	e.page = 256;
	CHECK(!i2c_eeprom_write(&e, 0, b, 1));
	e.page = 0;
	CHECK(!i2c_eeprom_write(&e, 0, b, 1));
	e.page = 64;
	e.size = 0x20000;
	CHECK(!i2c_eeprom_write(&e, 0, b, 1) && !i2c_eeprom_read(&e, 0, b, 1));
	CHECK(dev.transactions == t0);
}

// The bus fails, the device never finishes its write cycle
static void test_faults(void)
{
	i2c_eeprom_t e = { .bus = &bus, .addr7 = 0x50, .size = 0x10000, .page = 64, .write_ms = 5, .ack_poll = true };
	uint8_t b[128], r[16];
	fill(b, sizeof(b), 1);
	dev_create(0x10000, 64);
	e.ack_poll = false;
	dev.fail_at = 2;		// the second page's write
	CHECK(!i2c_eeprom_write(&e, 0x100, b, 128));
	CHECK(memcmp(&dev.mem[0x100], b, 64) == 0 && dev.mem[0x140] == 0xFF);
	e.ack_poll = true;
	now_ms += 10;
	dev.fail_at = 1;
	CHECK(!i2c_eeprom_read(&e, 0x100, r, sizeof(r)));
	// Stuck busy: the poll gives up after 2 x write_ms
	dev_create(0x10000, 64);
	dev.busy_until = 0;
	uint32_t t0 = now_ms;
	dev.cycle_ms = 1000000;
	CHECK(!i2c_eeprom_write(&e, 0, b, 1));
	CHECK(now_ms - t0 <= 2u * e.write_ms);
	// Without polling the driver only waits; the next access finds the device still busy and fails
	e.ack_poll = false;
	CHECK(!i2c_eeprom_read(&e, 0, r, 1));
}

int main(void)
{
	test_probe();
	test_read_write(false);
	test_read_write(true);
	test_chunked_reads();
	test_geometry();
	test_faults();
	printf("# i2cEepromTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
