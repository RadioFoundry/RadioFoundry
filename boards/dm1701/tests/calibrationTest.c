// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-1701 calibration reader against synthetic flash: working copy, factory table in the security registers,
// blank chip, and that it never writes
#include <stdio.h>
#include <string.h>
#include "dm1701.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t working[512];
static uint8_t security[3][256];
static int writes;

static bool fake_init(void) { return true; }
static bool fake_read(uint32_t addr, void *buf, uint32_t len)
{
	if (addr != 0x010000 || len != 512) {
		return false;
	}
	memcpy(buf, working, len);
	return true;
}
static bool fake_write(uint32_t addr, const void *buf, uint32_t len) { writes++; return false; }
static bool fake_erase(uint32_t addr) { writes++; return false; }

hal_flash_t stm32_spi_flash = { .init = fake_init, .size = 16u << 20, .erase_size = 4096,
				.read = fake_read, .write = fake_write, .erase = fake_erase };

bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (reg < 1 || reg > 3 || offset + len > 256) {
		return false;
	}
	memcpy(buf, &security[reg - 1][offset], len);
	return true;
}

static void fill(uint8_t *t, uint16_t off, uint8_t n, uint8_t first, int8_t step)
{
	for (uint8_t i = 0; i < n; i++) {
		t[off + i] = (uint8_t)(first + step * i);
	}
}

// A factory-style table: no very-low rows, no marker
static void build_factory(uint8_t *t)
{
	memset(t, 0xFF, 512);
	t[0x009] = 130;			// UHF tune: +2
	t[0x00C] = 120;			// VHF tune: -8
	fill(t, 0x010, 9, 160, 2);	// UHF high
	fill(t, 0x019, 5, 120, 1);	// VHF high
	fill(t, 0x020, 9, 64, 1);	// UHF low
	fill(t, 0x029, 5, 60, 2);	// VHF low
	fill(t, 0x190, 9, 100, 1);	// UHF mid
	fill(t, 0x199, 5, 90, 1);	// VHF mid
	fill(t, 0x130, 9, 0x70, 1);	// UHF DMR I
	fill(t, 0x139, 5, 0x60, 1);
	fill(t, 0x140, 9, 0x50, 1);	// UHF DMR Q
	fill(t, 0x149, 5, 0x40, 1);
	fill(t, 0x170, 9, 0x90, -1);	// UHF FM I
	fill(t, 0x179, 5, 0xA0, -2);
	fill(t, 0x180, 9, 0x30, 2);	// UHF FM Q
	fill(t, 0x189, 5, 0x20, 2);
}

static void test_factory(void)
{
	uint8_t t[512];
	build_factory(t);
	memcpy(security[0], t, 256);
	memcpy(security[1], t + 256, 256);
	memset(working, 0xFF, sizeof(working));	// no working copy yet

	CHECK(dm1701_cal_load());
	dm1701_cal_point_t p;

	// UHF 400 MHz exactly: point 0. very low = 64 x 0.65 = 41.6 -> 42, high = 160 x 1.7 = 272 -> 255
	CHECK(dm1701_cal_at(400000000, &p) && p.valid);
	CHECK(p.power_dac[0] == 42 * 16);
	CHECK(p.power_dac[1] == 64 * 16);
	CHECK(p.power_dac[2] == 86 * 16);	// 100 x 0.86
	CHECK(p.power_dac[3] == 255 * 16);
	CHECK(p.dmr_i == 0x70 && p.dmr_q == 0x50 && p.fm_i == 0x90 && p.fm_q == 0x30);
	CHECK(p.ref_offset == 2);

	// Halfway between 430 and 440 MHz: low 67 -> 68, x16 midway = 1080
	CHECK(dm1701_cal_at(435000000, &p));
	CHECK(p.power_dac[1] == 1080);
	CHECK(p.fm_q == 0x30 + 7);		// 0x36 .. 0x38: 0x37

	// Past the last point (480 MHz): extrapolate with the last slope; 490 MHz = point 8 + one step
	CHECK(dm1701_cal_at(490000000, &p));
	CHECK(p.power_dac[1] == (64 + 9) * 16);

	// VHF: power grid starts at 135 MHz, gains at 136 MHz with 9.5 MHz steps
	CHECK(dm1701_cal_at(145000000, &p) && p.valid);
	CHECK(p.power_dac[1] == 62 * 16);	// point 1 of the low row
	CHECK(p.power_dac[0] == 38 * 16);	// round(62 x 0.61) = 37.8 -> 38
	CHECK(p.power_dac[3] == 157 * 16);	// round(121 x 1.3) = 157.3
	CHECK(p.dmr_i == 0x60 + 0);		// 145 MHz is 9/9.5 of the way to point 1 (0x61): 0x60.95 -> 0x60
	CHECK(p.ref_offset == -8);

	// Below the first point: clamped to point 0
	CHECK(dm1701_cal_at(130000000, &p));
	CHECK(p.power_dac[1] == 60 * 16);
	CHECK(writes == 0);
}

static void test_working(void)
{
	uint8_t t[512];
	build_factory(t);
	fill(t, 0x1A0, 9, 30, 1);	// very low rows present
	fill(t, 0x1A9, 5, 30, 1);
	static const uint8_t marker[8] = { 0x00, 0x25, 0x00, 0x40, 0x00, 0x45, 0x01, 0x40 };
	memcpy(&t[0x1B0], marker, 8);
	memcpy(working, t, 512);
	memset(security, 0xFF, sizeof(security));

	CHECK(dm1701_cal_load());
	dm1701_cal_point_t p;
	CHECK(dm1701_cal_at(400000000, &p) && p.valid);
	CHECK(p.power_dac[0] == 30 * 16 && p.power_dac[1] == 64 * 16 && p.power_dac[3] == 160 * 16);	// used as stored
	CHECK(writes == 0);
}

static void test_blank(void)
{
	memset(working, 0xFF, sizeof(working));
	memset(security, 0xFF, sizeof(security));
	CHECK(!dm1701_cal_load());
	dm1701_cal_point_t p;
	CHECK(!dm1701_cal_at(446000000, &p));
	CHECK(!p.valid && p.power_dac[3] == p.power_dac[0] && p.power_dac[0] < 1024);
	CHECK(p.fm_i == 0x80 && p.ref_offset == 0);

	// A table with low above high is refused
	uint8_t t[512];
	build_factory(t);
	fill(t, 0x020, 9, 255, 0);
	fill(t, 0x029, 5, 250, 0);
	memcpy(security[0], t, 256);
	memcpy(security[1], t + 256, 256);
	CHECK(!dm1701_cal_load());
	CHECK(writes == 0);
}

static void test_direct_tx_ref(void)
{
	int8_t v = 0;
	CHECK(dm1701_direct_tx_ref(461137500u, -49, &v) && v == -49 + DM1701_DIRECT_TX_REF_STEPS);
	CHECK(dm1701_direct_tx_ref(461137500u, 127 - DM1701_DIRECT_TX_REF_STEPS, &v) && v == 127);
	v = 5;
	CHECK(!dm1701_direct_tx_ref(461137500u, 128 - DM1701_DIRECT_TX_REF_STEPS, &v) && v == 5);	// overflow: idle path
	CHECK(!dm1701_direct_tx_ref(146520000u, 0, &v) && v == 5);	// VHF: idle path
}

int main(void)
{
	test_factory();
	test_working();
	test_blank();
	test_direct_tx_ref();
	printf(failures ? "# calibrationTest FAILED (%d)\n" : "# calibrationTest PASSED\n", failures);
	return failures ? 1 : 0;
}
