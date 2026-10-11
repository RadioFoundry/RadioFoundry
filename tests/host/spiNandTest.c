// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// drivers/flash/spi_nand against a model of an SPI NAND with on-die ECC at the SPI bus (the commands the driver uses:
// FF reset, 9F ID after a dummy byte, 0F get feature, 13 page read into the cache, 03 read from the cache after a
// two-byte column and a dummy byte). The model stays busy for a while after a reset and a page read, sets the ECC
// status per page, and can fail the bus, stay busy forever or float MISO. The geometry is small so every page fits.
#include <stdio.h>
#include <string.h>
#include "flash/spi_nand/spi_nand.h"
#include "hal_time.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t delay_us;
void hal_delay_ms(uint32_t ms) { delay_us += ms * 1000u; }
void hal_delay_us(uint32_t us) { delay_us += us; }

#define PAGE		64u
#define SPARE		8u
#define PPB		4u
#define BLOCKS		8u
#define PAGES		(PPB * BLOCKS)

static struct {
	uint8_t array[PAGES][PAGE + SPARE];
	uint8_t ecc[PAGES];		// SPI_NAND_ECC_* a read of the page reports
	uint8_t cache[PAGE + SPARE];
	uint8_t status;
	uint32_t busy_polls;		// status reads left with OIP set
	uint32_t busy_after;		// polls a reset or page read stays busy
	bool stuck_busy, bus_fail, floating;
	uint8_t id[2];
	// The transaction in progress
	bool selected;
	uint8_t hdr[4];
	uint32_t n;			// bytes clocked in this transaction
	// Counts
	uint32_t resets, page_reads;
} chip;

static void start_busy(void)
{
	chip.status |= SPI_NAND_STATUS_OIP;
	chip.busy_polls = chip.busy_after;
}

static void bus_select(bool active)
{
	if (active) {
		CHECK(!chip.selected);
		chip.n = 0;
	} else if (chip.selected) {
		// Commands that act when chip select goes high
		if (chip.n == 1 && chip.hdr[0] == 0xFF) {
			chip.resets++;
			chip.status = 0;
			start_busy();
		} else if (chip.n == 4 && chip.hdr[0] == 0x13) {
			uint32_t page = (uint32_t)chip.hdr[1] << 16 | chip.hdr[2] << 8 | chip.hdr[3];
			CHECK(page < PAGES);
			CHECK((chip.status & SPI_NAND_STATUS_OIP) == 0);
			chip.page_reads++;
			memcpy(chip.cache, chip.array[page % PAGES], sizeof(chip.cache));
			chip.status = (uint8_t)(chip.ecc[page % PAGES] << SPI_NAND_STATUS_ECC_SHIFT);
			start_busy();
		}
	}
	chip.selected = active;
}

// One byte in, one out
static uint8_t clock_byte(uint8_t in)
{
	uint32_t i = chip.n++;
	if (i < sizeof(chip.hdr)) {
		chip.hdr[i] = in;
	}
	if (chip.floating) {
		return 0xFF;
	}
	switch (chip.hdr[0]) {
	case 0x9F:
		return i >= 2 && i < 4 ? chip.id[i - 2] : 0x00;	// opcode, dummy, then the ID
	case 0x0F:
		if (i == 2 && chip.hdr[1] == SPI_NAND_FEATURE_STATUS) {
			uint8_t st = chip.status;
			if (chip.busy_polls > 0 && !chip.stuck_busy) {
				chip.busy_polls--;
			}
			if (chip.busy_polls == 0 && !chip.stuck_busy) {
				chip.status &= (uint8_t)~SPI_NAND_STATUS_OIP;
			}
			return st;
		}
		return 0x00;
	case 0x03:
		if (i >= 4) {
			uint32_t col = (uint32_t)(chip.hdr[1] & 0x1F) << 8 | chip.hdr[2];
			uint32_t at = col + (i - 4);
			CHECK((chip.status & SPI_NAND_STATUS_OIP) == 0);
			return at < sizeof(chip.cache) ? chip.cache[at] : 0xFF;
		}
		return 0x00;
	default:
		return 0x00;
	}
}

static bool bus_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	CHECK(chip.selected);
	if (chip.bus_fail) {
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		uint8_t out = clock_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = out;
		}
	}
	return true;
}

static const spi_bus_t bus = { .select = bus_select, .xfer = bus_xfer };

static spi_nand_t fresh(void)
{
	spi_nand_t n = { .bus = &bus, .page_size = PAGE, .spare_size = SPARE, .pages_per_block = PPB, .blocks = BLOCKS };
	return n;
}

static void chip_reset(void)
{
	memset(&chip, 0, sizeof(chip));
	for (uint32_t p = 0; p < PAGES; p++) {
		for (uint32_t c = 0; c < PAGE; c++) {
			chip.array[p][c] = (uint8_t)(p * 7u + c);
		}
		memset(chip.array[p] + PAGE, 0xFF, SPARE);
	}
	chip.busy_after = 3;
	chip.id[0] = 0x98;
	chip.id[1] = 0xEB;
}

static void test_probe(void)
{
	chip_reset();
	spi_nand_t n = fresh();
	CHECK(spi_nand_probe(&n));
	CHECK(n.manufacturer == 0x98 && n.device == 0xEB && chip.resets == 1);
	CHECK(n.cached == SPI_NAND_NO_PAGE && n.corrected == 0);
	CHECK(spi_nand_size(&n) == PAGE * PPB * BLOCKS);

	chip_reset();
	chip.stuck_busy = true;
	n = fresh();
	delay_us = 0;
	CHECK(!spi_nand_probe(&n));
	CHECK(delay_us > 0 && delay_us < 20000);	// gives up in bounded time

	chip_reset();
	chip.floating = true;	// MISO high: status reads busy forever, then FF FF
	n = fresh();
	CHECK(!spi_nand_probe(&n) && n.manufacturer == 0);

	chip_reset();
	chip.id[0] = 0;
	chip.id[1] = 0;
	n = fresh();
	CHECK(!spi_nand_probe(&n));

	chip_reset();
	chip.bus_fail = true;
	n = fresh();
	CHECK(!spi_nand_probe(&n));
}

static void test_read(void)
{
	chip_reset();
	spi_nand_t n = fresh();
	CHECK(spi_nand_probe(&n));

	// Across three pages, from the middle of one
	uint8_t buf[PAGE * 3];
	uint32_t addr = 5 * PAGE + 10;
	CHECK(spi_nand_read(&n, addr, buf, sizeof(buf)));
	bool same = true;
	for (uint32_t i = 0; i < sizeof(buf); i++) {
		uint32_t a = addr + i;
		same = same && buf[i] == chip.array[a / PAGE][a % PAGE];
	}
	CHECK(same && chip.page_reads == 4);
	// The last page is still in the cache: no new page read
	CHECK(spi_nand_read(&n, 8 * PAGE + 1, buf, 4) && chip.page_reads == 4 && buf[0] == chip.array[8][1]);
	CHECK(spi_nand_read(&n, 0, buf, 0) && chip.page_reads == 4);

	// Bounds: the main area only, nothing past the end
	CHECK(spi_nand_read(&n, PAGES * PAGE - 4, buf, 4));
	CHECK(!spi_nand_read(&n, PAGES * PAGE - 4, buf, 5));
	CHECK(!spi_nand_read(&n, PAGES * PAGE + 1, buf, 0));

	// Spare bytes through read_page
	chip.array[2][PAGE + 3] = 0x5A;
	n.cached = SPI_NAND_NO_PAGE;
	CHECK(spi_nand_read_page(&n, 2, PAGE + 3, buf, 1) && buf[0] == 0x5A);
	CHECK(spi_nand_read_page(&n, 2, PAGE, buf, SPARE));
	CHECK(!spi_nand_read_page(&n, 2, PAGE, buf, SPARE + 1));
	CHECK(!spi_nand_read_page(&n, 2, PAGE + SPARE + 1, buf, 0));
	CHECK(!spi_nand_read_page(&n, PAGES, 0, buf, 1));

	// A page read that never finishes
	chip.stuck_busy = true;
	CHECK(!spi_nand_read(&n, 20 * PAGE, buf, 1));
	chip.stuck_busy = false;
	chip.status = 0;
	chip.busy_polls = 0;
	CHECK(spi_nand_read(&n, 20 * PAGE, buf, 1) && buf[0] == chip.array[20][0]);
}

static void test_ecc(void)
{
	chip_reset();
	spi_nand_t n = fresh();
	CHECK(spi_nand_probe(&n));
	uint8_t buf[4];

	chip.ecc[3] = SPI_NAND_ECC_CORRECTED;
	chip.ecc[4] = SPI_NAND_ECC_CORRECTED_MANY;
	CHECK(spi_nand_read(&n, 3 * PAGE, buf, 4) && n.ecc == SPI_NAND_ECC_CORRECTED && n.corrected == 1);
	CHECK(spi_nand_read(&n, 4 * PAGE, buf, 4) && n.ecc == SPI_NAND_ECC_CORRECTED_MANY && n.corrected == 2);
	CHECK(spi_nand_read(&n, 5 * PAGE, buf, 4) && n.ecc == SPI_NAND_ECC_OK && n.corrected == 2);

	// Uncorrectable: the read fails and the page isn't kept, so the next try reads the array again
	chip.ecc[6] = SPI_NAND_ECC_FAILED;
	uint32_t before = chip.page_reads;
	CHECK(!spi_nand_read(&n, 6 * PAGE, buf, 4) && n.ecc == SPI_NAND_ECC_FAILED);
	CHECK(n.cached == SPI_NAND_NO_PAGE);
	chip.ecc[6] = SPI_NAND_ECC_OK;
	CHECK(spi_nand_read(&n, 6 * PAGE, buf, 4) && chip.page_reads == before + 2);
	// A read that spans into a bad page fails as a whole
	chip.ecc[8] = SPI_NAND_ECC_FAILED;
	uint8_t span[PAGE + 4];
	CHECK(!spi_nand_read(&n, 7 * PAGE + 4, span, sizeof(span)));
}

static void test_bad_blocks(void)
{
	chip_reset();
	spi_nand_t n = fresh();
	CHECK(spi_nand_probe(&n));
	bool bad = true;

	CHECK(spi_nand_block_bad(&n, 0, &bad) && !bad);
	chip.array[2 * PPB][PAGE] = 0x00;		// factory mark on block 2
	chip.array[3 * PPB + 1][PAGE] = 0x00;		// a mark on block 3's second page doesn't count
	CHECK(spi_nand_block_bad(&n, 2, &bad) && bad);
	CHECK(spi_nand_block_bad(&n, 3, &bad) && !bad);
	// The mark is read whatever ECC says: a marked page that fails ECC is bad, an unmarked one isn't
	chip.ecc[4 * PPB] = SPI_NAND_ECC_FAILED;
	chip.array[4 * PPB][PAGE] = 0x00;
	CHECK(spi_nand_block_bad(&n, 4, &bad) && bad);
	chip.ecc[5 * PPB] = SPI_NAND_ECC_FAILED;
	CHECK(spi_nand_block_bad(&n, 5, &bad) && !bad);
	CHECK(n.cached == SPI_NAND_NO_PAGE);
	CHECK(!spi_nand_block_bad(&n, BLOCKS, &bad));
	chip.bus_fail = true;
	n.cached = SPI_NAND_NO_PAGE;
	CHECK(!spi_nand_block_bad(&n, 6, &bad));
}

int main(void)
{
	test_probe();
	test_read();
	test_ecc();
	test_bad_blocks();
	if (failures) {
		printf("spiNandTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("spiNandTest: all passed\n");
	return 0;
}
