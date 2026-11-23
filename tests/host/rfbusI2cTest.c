// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RF register bus on I2C (drivers/rf/rfbus/rfbus_i2c): the frames it hands the I2C master (register, then the
// value high byte first; a read as the register and two bytes back), the byte order of a read, a frame not
// acknowledged (counted, a read gives 0), the critical section around each frame, and the bus recovery at attach.
// The I2C master is a fake here: i2c_gpio's own timing has its test with the chip that uses it
#include <stdio.h>
#include <string.h>
#include "rf/rfbus/rfbus_i2c.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- fake I2C master ---

static uint8_t last_addr, last_w[8];
static size_t last_wn, last_rn;
static uint8_t reply[2];
static bool ack = true;
static int recovers, depth, max_depth, frames_outside;

bool i2c_gpio_write(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *data, size_t n)
{
	(void)bus;
	if (depth == 0) {
		frames_outside++;
	}
	last_addr = addr7;
	last_wn = n;
	memcpy(last_w, data, n);
	last_rn = 0;
	return ack;
}

bool i2c_gpio_write_read(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	(void)bus;
	if (depth == 0) {
		frames_outside++;
	}
	last_addr = addr7;
	last_wn = wn;
	memcpy(last_w, w, wn);
	last_rn = rn;
	if (ack) {
		memcpy(r, reply, rn);
	} else {
		memset(r, 0x5A, rn);	// whatever the line gave: the bus must not pass it on
	}
	return ack;
}

void i2c_gpio_recover(const i2c_gpio_t *bus)
{
	(void)bus;
	recovers++;
}

static uint32_t crit_enter(void)
{
	depth++;
	if (depth > max_depth) {
		max_depth = depth;
	}
	return 0x1234u;
}

static void crit_exit(uint32_t saved)
{
	CHECK(saved == 0x1234u);
	depth--;
}

static void no_delay(uint32_t ms) { (void)ms; }

static const i2c_gpio_t pins;	// never touched by the fake

int main(void)
{
	rfbus_i2c_t b;
	b.nacks = 99;
	rfbus_i2c_attach(&b, &pins, 0x71, no_delay);
	CHECK(recovers == 1 && b.nacks == 0 && b.crit_enter == NULL);

	// Without a critical section the frames go straight out
	rfbus_write(&b.bus, 0x30, 0xBFF1);
	CHECK(last_addr == 0x71 && last_wn == 3 && last_w[0] == 0x30 && last_w[1] == 0xBF && last_w[2] == 0xF1);
	CHECK(frames_outside == 1);

	b.crit_enter = crit_enter;
	b.crit_exit = crit_exit;
	reply[0] = 0x68;
	reply[1] = 0x18;
	CHECK(rfbus_read(&b.bus, 0x00) == 0x6818);
	CHECK(last_wn == 1 && last_w[0] == 0x00 && last_rn == 2);
	rfbus_write(&b.bus, 0x7F, 0x0001);
	CHECK(last_w[0] == 0x7F && last_w[1] == 0x00 && last_w[2] == 0x01);
	CHECK(frames_outside == 1 && max_depth == 1 && depth == 0);
	CHECK(b.nacks == 0);

	// No acknowledge: counted, and a read gives 0 rather than the line's bytes
	ack = false;
	rfbus_write(&b.bus, 0x03, 0x0000);
	CHECK(rfbus_read(&b.bus, 0x67) == 0);
	CHECK(b.nacks == 2 && depth == 0);
	ack = true;
	reply[0] = 0x01;
	reply[1] = 0x80;
	CHECK(rfbus_read(&b.bus, 0x67) == 0x0180 && b.nacks == 2);

	printf("# rfbusI2cTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
