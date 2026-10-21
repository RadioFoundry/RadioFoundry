// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// drivers/flash/spi_nor against a model of a W25Q / GD25Q class chip at the SPI bus (the command set of the Winbond
// W25Q128JV and GigaDevice GD25Q128 data sheets: 03 read, 02 page program, 20 sector erase, 06 write enable,
// 05 / 35 / 15 status, 9F JEDEC ID, 48 security register read). The model keeps what the driver must get right:
// erase sets a 4 KB sector to 0xFF, programming only clears bits, a page program that runs past the end of its
// 256-byte page wraps to the start of the same page, program and erase need the write enable latch (and clear it),
// the chip stays busy for a while after each, addresses wrap at the chip's size (3-byte addresses reach 16 MB). It
// can fail the bus, refuse the write enable (write protected), stay busy forever and lose power in the middle of a
// program or an erase. Then core/storage and core/settings run on the driver as the boards wire it (16 MB and 1 MB
// parts), with power cuts at every program / erase of a settings save and a codeplug sector rewrite. The MD-9600's map
// and its board record sector run on it the same way. Every command any test sent is checked at the end: none may
// erase or program a security register or write a status register (the factory calibration lives there).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash/spi_nor/spi_nor.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "hal_time.h"
#include "store_md9600.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

static uint32_t delays_ms;
void hal_delay_ms(uint32_t ms) { delays_ms += ms; }
void hal_delay_us(uint32_t us) { (void)us; }
uint32_t hal_millis(void) { return 0; }

// ---- The chip model ----

static struct {
	uint8_t *mem;
	uint32_t size;		// the array (power of two); addresses wrap at it
	uint8_t id[3];
	uint8_t security[3][256];
	bool wel;		// write enable latch
	uint32_t busy;		// status polls left that still read busy
	uint32_t busy_polls;	// busy polls after a program / erase (UINT32_MAX: stuck)
	bool protect;		// write enable refused (WP / block protect)
	// The command in progress (chip select low)
	bool selected;
	uint32_t pos;		// bytes clocked in this command
	uint8_t cmd;
	uint32_t addr;
	uint8_t page[256];	// page program data, by offset in the page
	bool page_set[256];
	uint32_t data_len;	// page program bytes clocked in
	// Bookkeeping and faults
	uint32_t selects, deselects, outside;	// xfer while not selected
	uint32_t programs, erases;		// operations done
	uint32_t page_overruns;			// page programs longer than the room to the page end
	uint32_t violations;			// programmed a 1 over a 0
	uint32_t fail_xfer_at;			// fail the n-th xfer from now (0 = never)
	uint32_t ops_left;			// program / erase operations before the power goes (0 = not armed)
	bool cut;				// no power: every transfer fails
	uint32_t status_reads[3];
} chip;

// Every command byte seen, over all tests (chip_create doesn't clear it)
static uint32_t cmds_seen[256];

static void chip_create(uint32_t size, uint8_t mfr, uint8_t type, uint8_t cap)
{
	free(chip.mem);
	memset(&chip, 0, sizeof(chip));
	chip.mem = malloc(size);
	memset(chip.mem, 0xFF, size);
	chip.size = size;
	chip.id[0] = mfr;
	chip.id[1] = type;
	chip.id[2] = cap;
	chip.busy_polls = 2;
	for (int r = 0; r < 3; r++) {
		for (int i = 0; i < 256; i++) {
			chip.security[r][i] = (uint8_t)(r * 0x40 + i);
		}
	}
}

static void chip_power_on(void)
{
	chip.cut = false;
	chip.ops_left = 0;
	chip.wel = false;
	chip.busy = 0;
	chip.selected = false;
}

// One program or erase starts: false when the power goes in the middle of it
static bool chip_op_survives(void)
{
	if (chip.ops_left == 0) {
		return true;
	}
	if (--chip.ops_left == 0) {
		chip.cut = true;
		return false;
	}
	return true;
}

static void chip_execute(void)
{
	if (chip.cmd == 0x02 && chip.pos >= 4 && chip.wel && chip.busy == 0) {
		uint32_t base = (chip.addr % chip.size) & ~0xFFu;
		bool whole = chip_op_survives();
		unsigned done = 0;
		for (unsigned i = 0; i < 256; i++) {
			if (!chip.page_set[i]) {
				continue;
			}
			if (!whole && done++ >= chip.data_len / 2) {
				break;		// torn: only part of the page programmed
			}
			if ((chip.page[i] & ~chip.mem[base + i]) != 0) {
				chip.violations++;
			}
			chip.mem[base + i] &= chip.page[i];
		}
		chip.programs++;
		chip.wel = false;
		chip.busy = chip.busy_polls;
	} else if (chip.cmd == 0x20 && chip.pos == 4 && chip.wel && chip.busy == 0) {
		uint32_t base = (chip.addr % chip.size) & ~0xFFFu;
		if (chip_op_survives()) {
			memset(&chip.mem[base], 0xFF, 4096);
		} else {
			memset(&chip.mem[base], 0xFF, 1024);	// torn: part erased, the rest scrambled
			for (uint32_t i = 1024; i < 4096; i++) {
				chip.mem[base + i] ^= (uint8_t)(i * 29u);
			}
		}
		chip.erases++;
		chip.wel = false;
		chip.busy = chip.busy_polls;
	}
}

static void bus_select(bool active)
{
	if (active) {
		chip.selects++;
		chip.selected = true;
		chip.pos = 0;
		chip.data_len = 0;
		memset(chip.page_set, 0, sizeof(chip.page_set));
	} else {
		chip.deselects++;
		if (chip.selected && !chip.cut) {
			chip_execute();
		}
		chip.selected = false;
	}
}

static uint8_t chip_byte(uint8_t in)
{
	uint32_t p = chip.pos++;
	if (p == 0) {
		cmds_seen[in]++;
		chip.cmd = in;
		chip.addr = 0;
		if (in == 0x06 && !chip.protect && chip.busy == 0) {
			chip.wel = true;
		}
		return 0xFF;
	}
	switch (chip.cmd) {
	case 0x05:
	case 0x35:
	case 0x15: {
		int which = chip.cmd == 0x05 ? 0 : chip.cmd == 0x35 ? 1 : 2;
		chip.status_reads[which]++;
		if (which != 0) {
			return (uint8_t)(0x10 * which);
		}
		uint8_t st = (uint8_t)((chip.busy != 0 ? 0x01 : 0) | (chip.wel ? 0x02 : 0));
		if (chip.busy != 0 && chip.busy != UINT32_MAX) {
			chip.busy--;
		}
		return st;
	}
	case 0x9F:
		return p <= 3 ? chip.id[p - 1] : 0xFF;
	case 0x03:
	case 0x02:
	case 0x20:
	case 0x48:
		if (p <= 3) {
			chip.addr = (chip.addr << 8) | in;
			return 0xFF;
		}
		if (chip.cmd == 0x03) {
			return chip.busy != 0 ? 0xFF : chip.mem[chip.addr++ % chip.size];
		}
		if (chip.cmd == 0x48) {
			if (p == 4) {
				return 0xFF;	// dummy
			}
			uint32_t a = chip.addr++;
			unsigned reg = (a >> 12) & 0xF;
			return reg >= 1 && reg <= 3 ? chip.security[reg - 1][a & 0xFF] : 0xFF;
		}
		if (chip.cmd == 0x02) {
			uint32_t off = (chip.addr + chip.data_len) & 0xFFu;	// wraps inside the page
			if (chip.data_len == 256u - (chip.addr & 0xFFu)) {
				chip.page_overruns++;
			}
			chip.page[off] = in;
			chip.page_set[off] = true;
			chip.data_len++;
		}
		return 0xFF;
	default:
		return 0xFF;
	}
}

static bool bus_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (chip.fail_xfer_at != 0 && --chip.fail_xfer_at == 0) {
		return false;
	}
	if (chip.cut) {
		return false;
	}
	if (!chip.selected) {
		chip.outside++;
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		uint8_t out = chip_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = out;
		}
	}
	return true;
}

static const spi_bus_t bus = { .select = bus_select, .xfer = bus_xfer };

// ---- The driver ----

static void test_probe(void)
{
	spi_nor_t nor = { .bus = &bus };
	static const struct {
		uint8_t mfr, type, cap;
		uint32_t size;
		bool ok;
	} ids[] = {
		{ 0xEF, 0x40, 0x18, 16u << 20, true },	// W25Q128JV: 16 MB
		{ 0xC8, 0x40, 0x18, 16u << 20, true },	// GD25Q128
		{ 0xEF, 0x40, 0x17, 8u << 20, true },	// W25Q64: 8 MB
		{ 0xEF, 0x40, 0x14, 1u << 20, true },	// W25Q80: 1 MB
		{ 0xEF, 0x40, 0x10, 64u << 10, true },	// W25X05 class: 64 KB
		{ 0xEF, 0x40, 0x0F, 0, true },		// capacity code not known: size 0
		{ 0x85, 0x20, 0x01, 0, true },
		// Larger than 3-byte addresses reach: the driver can only address the first 16 MB (above that a read
		// or a program would wrap to the bottom of the chip, where the codeplug and the calibration are)
		{ 0xEF, 0x40, 0x19, 16u << 20, true },	// W25Q256: 32 MB
		{ 0xEF, 0x40, 0x20, 16u << 20, true },	// W25Q512: 64 MB (Winbond's code after 0x19 is 0x20)
		{ 0xEF, 0x40, 0x21, 16u << 20, true },	// W25Q01: 128 MB
		{ 0x00, 0x00, 0x00, 0, false },		// bus stuck low
		{ 0xFF, 0xFF, 0xFF, 0, false },		// nothing drives MISO
		{ 0x00, 0x00, 0x18, 0, false },
	};
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		chip_create(64u << 10, ids[i].mfr, ids[i].type, ids[i].cap);
		nor.size = 123;
		bool ok = spi_nor_probe(&nor);
		CHECK(ok == ids[i].ok);
		if (ok != ids[i].ok || nor.size != ids[i].size) {
			printf("FAIL probe %02X %02X %02X: ok %d size 0x%X (want %d 0x%X)\n", ids[i].mfr, ids[i].type,
			       ids[i].cap, ok, nor.size, ids[i].ok, ids[i].size);
			failures++;
		}
		if (ok) {
			CHECK(nor.manufacturer == ids[i].mfr && nor.device == ((ids[i].type << 8) | ids[i].cap));
		} else {
			CHECK(nor.manufacturer == 0 && nor.device == 0 && nor.size == 0);
		}
		CHECK(chip.selects == 1 && chip.deselects == 1 && chip.outside == 0);
	}
	// The bus fails: false, nothing filled in, chip select released
	chip_create(64u << 10, 0xEF, 0x40, 0x18);
	chip.fail_xfer_at = 1;
	CHECK(!spi_nor_probe(&nor) && nor.size == 0 && nor.manufacturer == 0);
	CHECK(chip.selects == chip.deselects && !chip.selected);
	chip.fail_xfer_at = 2;			// the command goes out, the ID doesn't come back
	CHECK(!spi_nor_probe(&nor) && nor.size == 0);
	CHECK(chip.selects == chip.deselects && !chip.selected);
}

static void test_read_program_erase(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t b[1024], r[1024];

	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	CHECK(spi_nor_probe(&nor) && nor.size == (1u << 20));
	// Zero lengths: no bus traffic
	uint32_t s0 = chip.selects;
	CHECK(spi_nor_read(&nor, 0x100, r, 0) && spi_nor_program(&nor, 0x100, b, 0) &&
	      spi_nor_read_security(&nor, 0x1000, r, 0));
	CHECK(chip.selects == s0);

	// A program across three pages (16 + 256 + 28 bytes): split at the page ends, each page after a write enable
	for (unsigned i = 0; i < sizeof(b); i++) {
		b[i] = (uint8_t)(i * 7u + 3u);
	}
	uint32_t p0 = chip.programs;
	CHECK(spi_nor_program(&nor, 0x30F0, b, 300));
	CHECK(chip.programs == p0 + 3 && chip.page_overruns == 0 && chip.violations == 0);
	CHECK(memcmp(&chip.mem[0x30F0], b, 300) == 0);
	CHECK(chip.mem[0x30EF] == 0xFF && chip.mem[0x30F0 + 300] == 0xFF && chip.mem[0x3000] == 0xFF);
	CHECK(!chip.wel && chip.busy == 0);	// waited until done
	// Read it back, also across the pages and in odd lengths
	memset(r, 0, sizeof(r));
	CHECK(spi_nor_read(&nor, 0x30F0, r, 300) && memcmp(r, b, 300) == 0);
	CHECK(spi_nor_read(&nor, 0x30F1, r, 1) && r[0] == b[1]);
	// A whole page, page aligned: one program
	p0 = chip.programs;
	CHECK(spi_nor_program(&nor, 0x5000, b, 256) && chip.programs == p0 + 1);
	// Programming again only clears bits (the driver doesn't erase): the caller's business
	uint8_t z = 0x0F;
	CHECK(spi_nor_program(&nor, 0x5000, &z, 1) && chip.mem[0x5000] == (b[0] & 0x0F));
	chip.violations = 0;			// (that one was on purpose)

	// Sector erase: the sector holding addr, aligned down; the neighbors untouched
	memset(&chip.mem[0x6FFF], 0x00, 0x1002);
	CHECK(spi_nor_erase_sector(&nor, 0x7ABC));
	CHECK(chip.mem[0x6FFF] == 0x00 && chip.mem[0x7000] == 0xFF && chip.mem[0x7FFF] == 0xFF && chip.mem[0x8000] == 0x00);
	CHECK(!chip.wel && chip.selects == chip.deselects && chip.outside == 0);

	// Busy waits: the erase may take long (1 ms polls, up to the data sheet's 400 ms and some), a program not
	delays_ms = 0;
	chip.busy_polls = 300;
	CHECK(spi_nor_erase_sector(&nor, 0x9000) && delays_ms >= 299 && delays_ms <= 300);
	delays_ms = 0;
	chip.busy_polls = 30;			// a program that doesn't finish within its limit
	CHECK(!spi_nor_program(&nor, 0xA000, b, 4));
	CHECK(delays_ms > 3 && delays_ms < 30);
	chip.busy_polls = UINT32_MAX;		// stuck busy: the erase gives up instead of hanging
	chip.busy = 0;
	delays_ms = 0;
	CHECK(!spi_nor_erase_sector(&nor, 0xB000));
	CHECK(delays_ms >= 400 && delays_ms <= 1001);
	chip.busy = 0;
	chip.busy_polls = 2;

	// Write protected: the write enable latch doesn't set, nothing is sent
	chip.protect = true;
	p0 = chip.programs;
	uint32_t e0 = chip.erases;
	CHECK(!spi_nor_program(&nor, 0xC000, b, 16) && !spi_nor_erase_sector(&nor, 0x1000));
	CHECK(chip.programs == p0 && chip.erases == e0 && chip.mem[0xC000] == 0xFF);
	chip.protect = false;

	// A bus failure in each transfer of a program: false, chip select always released
	for (uint32_t k = 1; k <= 8; k++) {
		chip.fail_xfer_at = k;
		bool ok = spi_nor_program(&nor, 0xD0F0, b, 32);	// two pages
		CHECK(!ok || chip.fail_xfer_at != 0);
		CHECK(chip.selects == chip.deselects && !chip.selected);
		chip.fail_xfer_at = 0;
		chip.busy = 0;
		chip.wel = false;
	}
	for (uint32_t k = 1; k <= 4; k++) {
		chip.fail_xfer_at = k;
		CHECK(!spi_nor_erase_sector(&nor, 0xE000) || chip.fail_xfer_at != 0);
		chip.fail_xfer_at = 0;
		chip.busy = 0;
		chip.wel = false;
	}
	chip.fail_xfer_at = 2;			// the read's data phase
	CHECK(!spi_nor_read(&nor, 0, r, 16));
	chip.fail_xfer_at = 0;
	CHECK(chip.selects == chip.deselects && chip.outside == 0);

	// Status registers 1..3; anything else refused without bus traffic
	uint8_t st = 0xAA;
	CHECK(spi_nor_status(&nor, 1, &st) && st == 0x00);
	CHECK(spi_nor_status(&nor, 2, &st) && st == 0x10);
	CHECK(spi_nor_status(&nor, 3, &st) && st == 0x20);
	s0 = chip.selects;
	CHECK(!spi_nor_status(&nor, 0, &st) && !spi_nor_status(&nor, 4, &st) && chip.selects == s0);

	// Security registers (W25Q: reg << 12 | offset, a dummy byte after the address)
	CHECK(spi_nor_read_security(&nor, (2u << 12) | 0x10, r, 4));
	CHECK(r[0] == 0x50 && r[1] == 0x51 && r[3] == 0x53);
	CHECK(spi_nor_read_security(&nor, (3u << 12) | 0xFE, r, 2) && r[0] == 0x7E && r[1] == 0x7F);
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

// ---- core/storage and core/settings on the driver, as a board wires them ----

static spi_nor_t board_nor = { .bus = &bus };
static bool board_ready;

static bool f_init(void)
{
	board_ready = spi_nor_probe(&board_nor) && board_nor.size != 0;
	return board_ready;
}
static bool in_range(uint32_t addr, uint32_t len)
{
	return board_ready && addr < board_nor.size && len <= board_nor.size - addr;
}
static bool f_read(uint32_t addr, void *buf, uint32_t len)
{
	return in_range(addr, len) && spi_nor_read(&board_nor, addr, buf, len);
}
static bool f_write(uint32_t addr, const void *buf, uint32_t len)
{
	return in_range(addr, len) && spi_nor_program(&board_nor, addr, buf, len);
}
static bool f_erase(uint32_t addr)
{
	return in_range(addr, 1) && spi_nor_erase_sector(&board_nor, addr);
}

// The DM-1701's map (docs/spec/settings-store.md 1)
static const hal_flash_map_t map16 = {
	.settings_addr = 0x011000u, .settings_size = 0x8000u, .journal_addr = 0x019000u,
	.openfw_eeprom = 0, .openfw_flash = 0x020000u, .openfw_end = 0xE00000u,
	.calibration_addr = 0x010000u, .calibration_size = 0x200u,
	.native_addr = 0xE00000u, .native_size = 0x100000u,
	.staging_addr = 0xF00000u, .staging_size = 0x40000u,
};

static hal_flash_t flash = {
	.init = f_init, .erase_size = SPI_NOR_SECTOR_SIZE, .read = f_read, .write = f_write, .erase = f_erase,
	.map = &map16,
};

const board_t board = { .name = "spinor", .flash = &flash };

static bool boot(void)
{
	chip_power_on();
	flash.size = flash.init() ? board_nor.size : 0;
	bool ok = storage_init();
	settings_init();
	return ok;
}

static void fill(uint8_t *b, uint8_t seed)
{
	for (unsigned i = 0; i < STORAGE_SECTOR; i++) {
		b[i] = (uint8_t)(seed ^ (i * 11u) ^ (i >> 5));
	}
}

static uint8_t cal[0x200];

static void put_calibration(void)
{
	for (unsigned i = 0; i < sizeof(cal); i++) {
		cal[i] = (uint8_t)(i * 3u + 1u);
	}
	memcpy(&chip.mem[map16.calibration_addr], cal, sizeof(cal));
}

static bool calibration_intact(void)
{
	return memcmp(&chip.mem[map16.calibration_addr], cal, sizeof(cal)) == 0;
}

static void test_store_16mb(void)
{
	uint8_t a[STORAGE_SECTOR], b[STORAGE_SECTOR];
	settings_t s;

	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	put_calibration();
	CHECK(boot() && flash.size == (16u << 20));
	// The top of the chip: a staging sector rewrite and a native area erase reach the last megabyte
	fill(a, 1);
	CHECK(storage_sector_write(0xFFF000u, a) && memcmp(&chip.mem[0xFFF000u], a, sizeof(a)) == 0);
	CHECK(chip.mem[0] == 0xFF && chip.mem[0xFFF] == 0xFF);	// no wrap to the bottom
	static const uint8_t magic[4] = { 'O', 'D', 'R', 'N' };
	CHECK(f_write(map16.native_addr, magic, 4) && f_write(map16.native_addr + 0xFF000u, a, 64));
	CHECK(storage_native_erase() == STORAGE_NATIVE_ERASED);
	CHECK(chip.mem[map16.native_addr] == 0xFF && chip.mem[map16.native_addr + 0xFF000u] == 0xFF);
	CHECK(!storage_sector_write(16u << 20, a));		// past the end
	// Settings round trip through the driver
	settings_begin()->squelch = 6;
	settings_end(1);
	CHECK(settings_flush());
	CHECK(boot());
	settings_read(&s);
	CHECK(s.squelch == 6);
	// A codeplug range across a sector end
	fill(b, 9);
	CHECK(storage_write(0x20FF0u, b, 64) && memcmp(&chip.mem[0x20FF0u], b, 64) == 0);
	CHECK(calibration_intact() && chip.violations == 0 && chip.page_overruns == 0);
}

// A 1 MB part with the 16 MB part's map: the areas past its end are refused, the low ones work, and nothing wraps
// onto the bottom of the chip
static void test_store_small_part(void)
{
	uint8_t a[STORAGE_SECTOR];

	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	put_calibration();
	uint8_t low[64];
	memset(low, 0x5A, sizeof(low));
	memcpy(&chip.mem[0x0], low, sizeof(low));		// the open-firmware "EEPROM" image's first bytes
	CHECK(boot() && flash.size == (1u << 20));
	fill(a, 4);
	CHECK(!storage_sector_write(0xF00000u, a));		// the staging area isn't there
	CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	CHECK(!storage_write(0xFFFFF0u, a, 32));
	CHECK(storage_sector_write(0x21000u, a) && memcmp(&chip.mem[0x21000u], a, sizeof(a)) == 0);
	settings_begin()->squelch = 2;
	settings_end(1);
	CHECK(settings_flush());
	CHECK(memcmp(&chip.mem[0x0], low, sizeof(low)) == 0 && calibration_intact());
	CHECK(chip.violations == 0);
}

// Power cuts at every program and erase of a settings save and of a codeplug sector rewrite, with the page
// programs and erases torn as the chip tears them: after the next boot the old or the new content, whole; the
// calibration copy untouched every time
static void test_store_cuts(void)
{
	uint8_t old[STORAGE_SECTOR], neu[STORAGE_SECTOR];
	const uint32_t target = 0x40000u;
	settings_t s;

	fill(old, 0x11);
	fill(neu, 0xC3);
	// How many operations a rewrite takes
	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	put_calibration();
	boot();
	storage_sector_write(target, old);
	uint32_t ops0 = chip.programs + chip.erases;
	storage_sector_write(target, neu);
	uint32_t total = chip.programs + chip.erases - ops0;
	uint8_t *snap = malloc(16u << 20);
	int old_seen = 0, new_seen = 0;
	for (uint32_t cut = 1; cut <= total; cut++) {
		chip_create(16u << 20, 0xEF, 0x40, 0x18);
		put_calibration();
		boot();
		storage_sector_write(target, old);
		chip.ops_left = cut;
		bool ok = storage_sector_write(target, neu);
		CHECK(!ok || !chip.cut);
		boot();
		if (memcmp(&chip.mem[target], neu, STORAGE_SECTOR) == 0) {
			new_seen++;
		} else if (memcmp(&chip.mem[target], old, STORAGE_SECTOR) == 0) {
			old_seen++;
		} else {
			printf("FAIL spi_nor journal: cut at operation %u of %u leaves a mixed sector\n", cut, total);
			failures++;
		}
		CHECK(calibration_intact());
	}
	CHECK(old_seen > 0 && new_seen > 0);

	// A settings save, at a sector change (the save that erases the next ring sector) and inside a sector
	for (int variant = 0; variant < 2; variant++) {
		uint32_t saves_before = variant == 0 ? 15 : 3;	// (slot 0 holds the ownership mark)
		uint32_t n_ops = 0;
		old_seen = new_seen = 0;
		// cut 0: no cut, counts the operations of the save; the last cut point is past them (no cut either)
		for (uint32_t cut = 0; cut == 0 || cut <= n_ops + 1; cut++) {
			chip_create(16u << 20, 0xEF, 0x40, 0x18);
			put_calibration();
			boot();
			for (uint32_t i = 1; i <= saves_before; i++) {
				settings_begin()->vfo_rx_hz = 430000000u + i;
				settings_end(i);
				settings_flush();
			}
			memcpy(snap, chip.mem, 16u << 20);
			ops0 = chip.programs + chip.erases;
			chip.ops_left = cut;
			settings_begin()->vfo_rx_hz = 145000000u;
			settings_end(100);
			settings_flush();
			if (cut == 0) {
				n_ops = chip.programs + chip.erases - ops0;
			}
			boot();
			settings_read(&s);
			if (s.vfo_rx_hz == 145000000u) {
				new_seen++;
			} else if (s.vfo_rx_hz == 430000000u + saves_before) {
				old_seen++;
			} else {
				printf("FAIL spi_nor settings: cut %u after %u saves loads %u\n", cut, saves_before, s.vfo_rx_hz);
				failures++;
			}
			CHECK(calibration_intact());
			// Nothing outside the ring changed
			CHECK(memcmp(snap, chip.mem, map16.settings_addr) == 0);
			CHECK(memcmp(snap + map16.journal_addr, chip.mem + map16.journal_addr,
				     (16u << 20) - map16.journal_addr) == 0);
		}
		CHECK(new_seen > 0 && old_seen > 0);
	}
	free(snap);
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

// ---- The MD-9600: its map under core/storage and core/settings, and the board record sector ----

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
}

static void test_md9600_map(void)
{
	const hal_flash_map_t *m = &md9600_flash_map;
	const uint32_t openfw_eeprom_len = 0x10000u;	// the 64 KB "EEPROM" image
	struct { uint32_t addr, len; } area[] = {
		{ m->settings_addr, m->settings_size }, { m->journal_addr, 2 * STORAGE_SECTOR },
		{ m->openfw_eeprom, openfw_eeprom_len }, { m->openfw_flash, m->openfw_end - m->openfw_flash },
		{ m->native_addr, m->native_size }, { m->staging_addr, m->staging_size },
		{ MD9600_STORE_REV_SECTOR, MD9600_STORE_SECTOR_SIZE },
	};
	const unsigned n = sizeof(area) / sizeof(area[0]);
	for (unsigned i = 0; i < n; i++) {
		CHECK(area[i].addr % STORAGE_SECTOR == 0 && area[i].len % STORAGE_SECTOR == 0);
		CHECK(area[i].addr + area[i].len <= MD9600_STORE_FULL_SIZE);
		for (unsigned j = i + 1; j < n; j++) {
			if (overlaps(area[i].addr, area[i].len, area[j].addr, area[j].len)) {
				printf("FAIL md9600 map: areas %u and %u overlap\n", i, j);
				failures++;
			}
		}
	}
	CHECK(m->settings_size >= 2 * STORAGE_SECTOR && m->calibration_size == 0);
}

static void md9600_create(void)
{
	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	flash.map = &md9600_flash_map;
}

static void test_md9600_store(void)
{
	uint8_t sec_before[3][256], a[STORAGE_SECTOR];
	settings_t s;

	md9600_create();
	memcpy(sec_before, chip.security, sizeof sec_before);
	CHECK(boot() && storage_ready());
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN);
	// Settings and a codeplug write under the board's map, then the board record beside them
	settings_begin()->squelch = 4;
	settings_end(1);
	CHECK(settings_flush());
	fill(a, 3);
	CHECK(storage_write(0x20FF0u, a, 64));
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4) && md9600_store_rev_load(&flash) == MD9600_REV_V4);
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V5) && md9600_store_rev_load(&flash) == MD9600_REV_V5);
	uint32_t ops = chip.programs + chip.erases;
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V5) && chip.programs + chip.erases == ops);	// same: no write
	CHECK(!md9600_store_rev_save(&flash, MD9600_REV_UNKNOWN) && !md9600_store_rev_save(&flash, MD9600_REV_COUNT));
	// Appended: V4 in the first slot, V5 in the second, the third free
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 5] == MD9600_REV_V4);
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 13] == MD9600_REV_V5);
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 16] == 0xFF);
	CHECK(boot());
	settings_read(&s);
	CHECK(s.squelch == 4 && memcmp(&chip.mem[0x20FF0u], a, 64) == 0);
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_V5);
	// The core never touched the record sector, and the record never touched the core's areas
	CHECK(storage_region() == STORAGE_REGION_OURS);
	// Another map or a smaller part: no record
	flash.map = &map16;
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN && !md9600_store_rev_save(&flash, MD9600_REV_V4));
	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	flash.map = &md9600_flash_map;
	boot();
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN && !md9600_store_rev_save(&flash, MD9600_REV_V4));
	CHECK(memcmp(sec_before, chip.security, sizeof sec_before) == 0);
	flash.map = &map16;
}

// A power cut at every operation of a revision change, with the sector part used and with it full: after the next
// boot the old revision or the new one, never another; nothing outside the record sector changes
static void test_md9600_rev_cuts(void)
{
	uint8_t *snap = malloc(16u << 20);
	for (int full = 0; full < 2; full++) {
		uint32_t n_ops = 0;
		int old_seen = 0, new_seen = 0, lost = 0;
		for (uint32_t cut = 0; cut == 0 || cut <= n_ops + 1; cut++) {
			md9600_create();
			boot();
			CHECK(md9600_store_rev_save(&flash, MD9600_REV_V1V3));
			if (full) {
				// Fill every slot after the first with the same record: the next change must erase
				uint8_t rec[MD9600_REV_RECORD_SIZE];
				md9600_rev_record_encode(MD9600_REV_V1V3, rec);
				for (uint32_t off = MD9600_REV_RECORD_SIZE; off < MD9600_STORE_SECTOR_SIZE; off += sizeof rec) {
					f_write(MD9600_STORE_REV_SECTOR + off, rec, sizeof rec);
				}
			} else {
				CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4));
				CHECK(md9600_store_rev_save(&flash, MD9600_REV_V1V3));
			}
			memcpy(snap, chip.mem, 16u << 20);
			uint32_t ops0 = chip.programs + chip.erases;
			chip.ops_left = cut;
			md9600_store_rev_save(&flash, MD9600_REV_V5);
			if (cut == 0) {
				n_ops = chip.programs + chip.erases - ops0;
			}
			boot();
			md9600_rev_t r = md9600_store_rev_load(&flash);
			if (r == MD9600_REV_V5) {
				new_seen++;
			} else if (r == MD9600_REV_V1V3) {
				old_seen++;
			} else if (r == MD9600_REV_UNKNOWN && full) {
				lost++;		// the erase of a full sector was cut: as a new radio (store_md9600.h)
			} else {
				printf("FAIL md9600 record: cut %u (%s sector) loads %d\n", cut, full ? "full" : "part used", r);
				failures++;
			}
			CHECK(memcmp(snap, chip.mem, MD9600_STORE_REV_SECTOR) == 0);
			CHECK(memcmp(snap + MD9600_STORE_REV_SECTOR + MD9600_STORE_SECTOR_SIZE,
				     chip.mem + MD9600_STORE_REV_SECTOR + MD9600_STORE_SECTOR_SIZE,
				     (16u << 20) - MD9600_STORE_REV_SECTOR - MD9600_STORE_SECTOR_SIZE) == 0);
			// A later save after the cut still works
			CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4) && md9600_store_rev_load(&flash) == MD9600_REV_V4);
		}
		CHECK(new_seen > 0 && (old_seen > 0 || lost > 0));
		CHECK(full || lost == 0);
	}
	free(snap);
	flash.map = &map16;
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

// Over every test: only the commands the driver is meant to send. None may erase (0x44) or program (0x42) a security
// register, write a status register (0x01, 0x31, 0x11: the security register lock bits), or erase more than a sector
static void test_commands_sent(void)
{
	static const uint8_t allowed[] = { 0x03, 0x02, 0x20, 0x06, 0x05, 0x35, 0x15, 0x9F, 0x48 };
	for (unsigned c = 0; c < 256; c++) {
		bool ok = false;
		for (unsigned i = 0; i < sizeof allowed; i++) {
			ok |= allowed[i] == c;
		}
		if (!ok && cmds_seen[c] != 0) {
			printf("FAIL spi_nor: command 0x%02X sent %u times\n", c, cmds_seen[c]);
			failures++;
		}
	}
	CHECK(cmds_seen[0x48] != 0 && cmds_seen[0x20] != 0);	// the tests did read security registers and erase
}

int main(void)
{
	test_probe();
	test_read_program_erase();
	test_store_16mb();
	test_store_small_part();
	test_store_cuts();
	test_md9600_map();
	test_md9600_store();
	test_md9600_rev_cuts();
	test_commands_sent();
	free(chip.mem);
	printf("# spiNorTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
