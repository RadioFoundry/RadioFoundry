// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// drivers/flash/spi_nand against a model of an SPI NAND with on-die ECC at the SPI bus (the commands the driver uses:
// FF reset, 9F ID after a dummy byte, 0F / 1F get / set feature, 13 page read into the cache, 03 read from the cache
// after a two-byte column and a dummy byte, 06 write enable, 02 / 84 program load (02 clears the cache first), 10
// program execute, D8 block erase). The model stays busy for a while after a reset, a page read, a program and an
// erase, sets the ECC status per page, refuses program and erase without write enable or while the block lock is on
// (status fail bits), counts a page programmed twice or out of order, and can fail the bus, stay busy forever or float
// MISO. The geometry is small so every page fits.
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
	uint8_t lock, config;		// features 0xA0, 0xB0
	uint8_t id_page[PAGE];		// what page 0 reads with IDR_E (0xB0 bit 6) set
	uint16_t next[BLOCKS];		// the next page that may be programmed in each block
	bool worn[BLOCKS];		// programs and erases there report failure
	uint32_t violations;		// programs twice or out of order
	uint32_t programs, erases;
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
		// While busy only status reads and a reset may go in
		if (chip.n > 0 && chip.hdr[0] != 0x0F && chip.hdr[0] != 0xFF && (chip.status & SPI_NAND_STATUS_OIP)) {
			chip.violations++;
		}
		// Commands that act when chip select goes high
		if (chip.n == 1 && chip.hdr[0] == 0xFF) {
			chip.resets++;
			chip.status = 0;
			start_busy();
		} else if (chip.n == 3 && chip.hdr[0] == 0x1F) {
			if (chip.hdr[1] == SPI_NAND_FEATURE_LOCK) {
				chip.lock = chip.hdr[2];
			} else if (chip.hdr[1] == SPI_NAND_FEATURE_CONFIG) {
				chip.config = chip.hdr[2];
			}
		} else if (chip.n == 1 && chip.hdr[0] == 0x06) {
			chip.status |= SPI_NAND_STATUS_WEL;
		} else if (chip.n == 4 && (chip.hdr[0] == 0x10 || chip.hdr[0] == 0xD8)) {
			uint32_t row = (uint32_t)chip.hdr[1] << 16 | chip.hdr[2] << 8 | chip.hdr[3];
			uint32_t b = row / PPB;
			CHECK(row < PAGES && (chip.status & SPI_NAND_STATUS_OIP) == 0);
			bool ok = (chip.status & SPI_NAND_STATUS_WEL) && chip.lock == 0 && !chip.worn[b % BLOCKS];
			uint8_t fail = chip.hdr[0] == 0x10 ? SPI_NAND_STATUS_P_FAIL : SPI_NAND_STATUS_E_FAIL;
			chip.status = (uint8_t)(ok ? 0 : fail);
			if (ok && chip.hdr[0] == 0x10) {
				chip.programs++;
				if (row % PPB != chip.next[b]) {
					chip.violations++;
				}
				chip.next[b] = (uint16_t)(row % PPB + 1u);
				for (uint32_t c = 0; c < PAGE + SPARE; c++) {
					chip.array[row][c] &= chip.cache[c];
				}
			} else if (ok) {
				chip.erases++;
				chip.next[b] = 0;
				memset(chip.array[b * PPB], 0xFF, sizeof(chip.array[0]) * PPB);
			}
			start_busy();
		} else if (chip.n == 4 && chip.hdr[0] == 0x13) {
			uint32_t page = (uint32_t)chip.hdr[1] << 16 | chip.hdr[2] << 8 | chip.hdr[3];
			CHECK(page < PAGES);
			CHECK((chip.status & SPI_NAND_STATUS_OIP) == 0);
			chip.page_reads++;
			if (chip.config & 0x40u) {
				CHECK(page == 0);	// the ID page; 1 would be the parameter page
				memset(chip.cache, 0xFF, sizeof(chip.cache));
				memcpy(chip.cache, chip.id_page, sizeof(chip.id_page));
			} else {
				memcpy(chip.cache, chip.array[page % PAGES], sizeof(chip.cache));
			}
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
	case 0x02:
	case 0x84:
		if (i == 1 && chip.hdr[0] == 0x02) {
			memset(chip.cache, 0xFF, sizeof(chip.cache));	// 02 clears the buffer
		}
		if (i >= 3) {
			uint32_t col = (uint32_t)(chip.hdr[1] & 0x0F) << 8 | chip.hdr[2];
			uint32_t at = col + (i - 3);
			CHECK(at < sizeof(chip.cache));
			if (at < sizeof(chip.cache)) {
				chip.cache[at] = in;
			}
		}
		return 0x00;
	case 0x0F:
		if (i == 2 && chip.hdr[1] == SPI_NAND_FEATURE_LOCK) {
			return chip.lock;
		}
		if (i == 2 && chip.hdr[1] == SPI_NAND_FEATURE_CONFIG) {
			return chip.config;
		}
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
	chip.lock = 0x38;		// every block locked at power-up
	chip.config = 0x12;		// ECC on, high speed read on
	for (uint32_t b = 0; b < BLOCKS; b++) {
		chip.next[b] = PPB;	// the pattern above counts as programmed: an erase comes first
	}
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

// The DB25-D's settings: ECC on and high speed read off (feature 0xB0), every block locked between operations
static spi_nand_t writable(uint16_t first, uint16_t count)
{
	spi_nand_t n = fresh();
	n.config = 0x10;
	n.lock_all = 0x38;
	n.write_first = first;
	n.write_blocks = count;
	return n;
}

static void test_program(void)
{
	chip_reset();
	spi_nand_t n = writable(2, 4);
	CHECK(spi_nand_probe(&n));
	CHECK(chip.config == 0x10 && chip.lock == 0x38);

	// Erase, then a page from an empty buffer: the chunks where they belong, 0xFF elsewhere; locked again after
	CHECK(spi_nand_erase(&n, 2) == SPI_NAND_DONE && chip.erases == 1 && chip.lock == 0x38);
	const uint8_t data[5] = { 1, 2, 3, 4, 5 }, meta[3] = { 0xA1, 0xA2, 0xA3 };
	spi_nand_chunk_t c[2] = { { 10, sizeof(data), data }, { PAGE + 2, sizeof(meta), meta } };
	uint32_t page = 2 * PPB;
	CHECK(spi_nand_program(&n, page, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_DONE);
	CHECK(chip.lock == 0x38 && (chip.status & SPI_NAND_STATUS_WEL) == 0);
	CHECK(memcmp(&chip.array[page][10], data, sizeof(data)) == 0 && chip.array[page][9] == 0xFF &&
	      chip.array[page][15] == 0xFF && memcmp(&chip.array[page][PAGE + 2], meta, sizeof(meta)) == 0 &&
	      chip.array[page][PAGE] == 0xFF);
	uint8_t buf[PAGE + SPARE];
	CHECK(spi_nand_read_page(&n, page, 0, buf, sizeof(buf)) && memcmp(buf, chip.array[page], sizeof(buf)) == 0);

	// No chunk: a page of 0xFF still takes its program
	CHECK(spi_nand_program(&n, page + 1, SPI_NAND_NO_PAGE, NULL, 0) == SPI_NAND_DONE && chip.programs == 2);

	// An internal data move: the source's bytes with one chunk changed, programmed elsewhere
	const uint8_t patch[2] = { 0x00, 0x11 };
	spi_nand_chunk_t m = { 11, sizeof(patch), patch };
	CHECK(spi_nand_program(&n, page + 2, page, &m, 1) == SPI_NAND_DONE);
	CHECK(chip.array[page + 2][10] == 1 && chip.array[page + 2][11] == 0x00 && chip.array[page + 2][12] == 0x11 &&
	      chip.array[page + 2][13] == 4 && memcmp(&chip.array[page + 2][PAGE + 2], meta, sizeof(meta)) == 0);
	// The page read before is no longer taken as cached
	CHECK(spi_nand_read_page(&n, page + 2, 11, buf, 2) && buf[0] == 0x00 && buf[1] == 0x11);

	// A source page ECC can't correct isn't copied
	chip.ecc[page + 1] = SPI_NAND_ECC_FAILED;
	uint32_t before = chip.programs;
	CHECK(spi_nand_program(&n, page + 3, page + 1, NULL, 0) == SPI_NAND_ERROR && chip.programs == before);
	CHECK(chip.lock == 0x38);
	chip.ecc[page + 1] = SPI_NAND_ECC_OK;

	// Outside the write window, outside the chip, chunks past the spare area: refused before the bus
	CHECK(spi_nand_program(&n, 1 * PPB, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_ERROR);
	CHECK(spi_nand_program(&n, 6 * PPB, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_ERROR);
	CHECK(spi_nand_program(&n, PAGES, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_ERROR);
	CHECK(spi_nand_program(&n, page + 3, PAGES, NULL, 0) == SPI_NAND_ERROR);
	spi_nand_chunk_t past = { PAGE + SPARE - 1, 2, data };
	CHECK(spi_nand_program(&n, page + 3, SPI_NAND_NO_PAGE, &past, 1) == SPI_NAND_ERROR);
	CHECK(spi_nand_erase(&n, 1) == SPI_NAND_ERROR && spi_nand_erase(&n, 6) == SPI_NAND_ERROR);
	CHECK(spi_nand_mark_bad(&n, 7) == SPI_NAND_ERROR);
	CHECK(chip.programs == before && chip.erases == 1);
	// A read-only chip (no window): nothing at all
	spi_nand_t ro = fresh();
	CHECK(spi_nand_probe(&ro) && spi_nand_erase(&ro, 2) == SPI_NAND_ERROR);

	// The chip reports failures: FAILED, counted
	chip.worn[3] = true;
	CHECK(spi_nand_erase(&n, 3) == SPI_NAND_FAILED && n.program_fails == 1);
	CHECK(spi_nand_program(&n, 3 * PPB, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_FAILED && n.program_fails == 2);
	CHECK(chip.lock == 0x38);
	// Retiring it: page 0 all 0x00, main and spare, which the bad-block check reports
	chip.worn[3] = false;
	CHECK(spi_nand_mark_bad(&n, 3) == SPI_NAND_DONE);
	bool zero = true;
	for (uint32_t i = 0; i < PAGE + SPARE; i++) {
		zero = zero && chip.array[3 * PPB][i] == 0x00;
	}
	bool bad = false;
	CHECK(zero && spi_nand_block_bad(&n, 3, &bad) && bad);

	// The bus fails or the chip stays busy: ERROR, and the lock is put back when the bus allows
	chip.bus_fail = true;
	CHECK(spi_nand_program(&n, page + 3, SPI_NAND_NO_PAGE, c, 2) == SPI_NAND_ERROR);
	CHECK(spi_nand_erase(&n, 4) == SPI_NAND_ERROR);
	chip.bus_fail = false;
	CHECK(chip.lock == 0x38);
	// Still busy when the driver gives up: no command but status reads goes in (the model counts any as a
	// violation); the next operation locks again
	chip.stuck_busy = true;
	CHECK(spi_nand_erase(&n, 4) == SPI_NAND_ERROR);
	chip.stuck_busy = false;
	chip.status = 0;
	chip.busy_polls = 0;
	CHECK(spi_nand_erase(&n, 4) == SPI_NAND_DONE && chip.lock == 0x38);
	CHECK(chip.violations == 0);
}

// The unique ID page (Kioxia 4.14): 16 copies of the ID and its complement. A copy that doesn't check out is skipped;
// feature 0xB0 is put back and the cache no longer counts as holding a data page
static void test_uid(void)
{
	spi_nand_t nand = fresh();
	uint8_t id[SPI_NAND_UID_LEN], page0[8], buf[8];
	chip_reset();
	CHECK(spi_nand_probe(&nand));
	chip.config = 0x10;
	for (unsigned c = 0; c < PAGE / 32; c++) {	// as many copies as the model's page holds
		for (unsigned i = 0; i < 16; i++) {
			chip.id_page[c * 32 + i] = (uint8_t)(0x30 + i);
			chip.id_page[c * 32 + 16 + i] = (uint8_t)~(0x30 + i);
		}
	}
	chip.id_page[0] ^= 0x01;	// the first copy is damaged
	memset(chip.array[0], 0x5A, 16);
	CHECK(spi_nand_read_page(&nand, 0, 0, page0, sizeof(page0)) && page0[0] == 0x5A);
	CHECK(spi_nand_read_uid(&nand, id) && id[0] == 0x30 && id[15] == 0x3F);
	CHECK(chip.config == 0x10);
	CHECK(spi_nand_read_page(&nand, 0, 0, buf, sizeof(buf)) && buf[0] == 0x5A);	// read again, not the ID page

	// No copy checks out (a chip without an ID page): false, config back all the same
	memset(chip.id_page, 0xFF, sizeof(chip.id_page));
	CHECK(!spi_nand_read_uid(&nand, id));
	CHECK(chip.config == 0x10);
	CHECK(chip.violations == 0);
}

int main(void)
{
	test_probe();
	test_read();
	test_ecc();
	test_bad_blocks();
	test_program();
	test_uid();
	if (failures) {
		printf("spiNandTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("spiNandTest: all passed\n");
	return 0;
}
