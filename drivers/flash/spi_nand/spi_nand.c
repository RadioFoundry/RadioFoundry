// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic SPI NAND flash (docs/spec/hardware-db25d.md 6.5: the Kioxia TC58CVG1S3HRAIJ data sheet and the sequence the
// DB25-D stock firmware uses on it)
#include <stddef.h>
#include "spi_nand.h"
#include "hal_time.h"

enum {
	CMD_RESET		= 0xFF,
	CMD_READ_ID		= 0x9F,
	CMD_GET_FEATURE		= 0x0F,
	CMD_SET_FEATURE		= 0x1F,
	CMD_PAGE_READ		= 0x13,
	CMD_READ_CACHE		= 0x03,
	CMD_WRITE_ENABLE	= 0x06,
	CMD_PROGRAM_LOAD	= 0x02,		// clears the chip's buffer to 0xFF, then loads
	CMD_PROGRAM_RANDOM	= 0x84,		// loads without clearing
	CMD_PROGRAM_EXECUTE	= 0x10,
	CMD_BLOCK_ERASE		= 0xD8,
};

// Busy limits in POLL_US polls, with room for slower parts (Kioxia maximums: page read 180 us, reset 550 us, program
// 500 us, block erase 7 ms)
#define POLL_US			10u
#define PAGE_READ_POLLS		200u
#define RESET_POLLS		500u
#define PROGRAM_POLLS		200u
#define ERASE_POLLS		2000u

// One command: a header clocked out, then len bytes read in the same chip select
static bool command(const spi_nand_t *nand, const uint8_t *hdr, uint32_t hdr_len, uint8_t *rx, uint32_t len)
{
	const spi_bus_t *bus = nand->bus;
	bus->select(true);
	bool ok = bus->xfer(hdr, NULL, hdr_len);
	if (ok && len > 0) {
		ok = bus->xfer(NULL, rx, len);
	}
	bus->select(false);
	return ok;
}

bool spi_nand_get_feature(spi_nand_t *nand, uint8_t addr, uint8_t *out)
{
	const uint8_t hdr[2] = { CMD_GET_FEATURE, addr };
	return command(nand, hdr, sizeof(hdr), out, 1);
}

bool spi_nand_set_feature(spi_nand_t *nand, uint8_t addr, uint8_t value)
{
	const uint8_t hdr[3] = { CMD_SET_FEATURE, addr, value };
	return command(nand, hdr, sizeof(hdr), NULL, 0);
}

// Poll until the operation in progress ends; the last status in *status
static bool wait_ready(spi_nand_t *nand, uint32_t polls, uint8_t *status)
{
	for (uint32_t i = 0; i <= polls; i++) {
		if (!spi_nand_get_feature(nand, SPI_NAND_FEATURE_STATUS, status)) {
			return false;
		}
		if ((*status & SPI_NAND_STATUS_OIP) == 0) {
			return true;
		}
		hal_delay_us(POLL_US);
	}
	return false;
}

bool spi_nand_probe(spi_nand_t *nand)
{
	static const uint8_t reset = CMD_RESET;
	static const uint8_t read_id[2] = { CMD_READ_ID, 0x00 };
	uint8_t id[2], status;
	nand->manufacturer = 0;
	nand->device = 0;
	nand->cached = SPI_NAND_NO_PAGE;
	nand->ecc = SPI_NAND_ECC_OK;
	nand->corrected = 0;
	if (!command(nand, &reset, 1, NULL, 0) || !wait_ready(nand, RESET_POLLS, &status) ||
	    !command(nand, read_id, sizeof(read_id), id, sizeof(id))) {
		return false;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF)) {
		return false;	// bus stuck low or nothing driving MISO
	}
	nand->manufacturer = id[0];
	nand->device = id[1];
	// The configuration (a feature keeps its value until power off, through resets), and every block locked until a
	// program or erase needs it open
	if ((nand->config != 0 && !spi_nand_set_feature(nand, SPI_NAND_FEATURE_CONFIG, nand->config)) ||
	    (nand->lock_all != 0 && !spi_nand_set_feature(nand, SPI_NAND_FEATURE_LOCK, nand->lock_all))) {
		return false;
	}
	return true;
}

uint32_t spi_nand_size(const spi_nand_t *nand)
{
	return (uint32_t)nand->page_size * nand->pages_per_block * nand->blocks;
}

// Bring page into the chip's cache unless it's there already. With any_ecc a page ECC couldn't correct still counts as
// loaded (its raw data is in the cache), but isn't remembered as cached
static bool load(spi_nand_t *nand, uint32_t page, bool any_ecc)
{
	if (page == nand->cached) {
		return true;
	}
	const uint8_t hdr[4] = { CMD_PAGE_READ, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
	uint8_t status;
	nand->cached = SPI_NAND_NO_PAGE;
	nand->ecc = SPI_NAND_ECC_OK;
	if (!command(nand, hdr, sizeof(hdr), NULL, 0) || !wait_ready(nand, PAGE_READ_POLLS, &status)) {
		return false;
	}
	nand->ecc = (uint8_t)((status >> SPI_NAND_STATUS_ECC_SHIFT) & 3u);
	if (nand->ecc == SPI_NAND_ECC_FAILED) {
		return any_ecc;	// left uncached, so a retry reads the array again
	}
	if (nand->ecc != SPI_NAND_ECC_OK) {
		nand->corrected++;
	}
	nand->cached = page;
	return true;
}

static bool read_cache(const spi_nand_t *nand, uint16_t column, void *buf, uint32_t len)
{
	const uint8_t hdr[4] = { CMD_READ_CACHE, (uint8_t)(column >> 8), (uint8_t)column, 0x00 };	// dummy byte
	return command(nand, hdr, sizeof(hdr), buf, len);
}

bool spi_nand_read_page(spi_nand_t *nand, uint32_t page, uint16_t column, void *buf, uint32_t len)
{
	uint32_t total = (uint32_t)nand->page_size + nand->spare_size;
	if (page >= (uint32_t)nand->pages_per_block * nand->blocks || column > total || len > total - column) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	return load(nand, page, false) && read_cache(nand, column, buf, len);
}

// The unique ID page: feature 0xB0 bit 6 (IDR_E) set, a page read of page 0, then 16 copies of the ID and its
// complement, 32 bytes each (Kioxia data sheet 4.14). The first copy whose complement matches wins
#define CONFIG_IDR_E		0x40u
#define UID_COPIES		16u

bool spi_nand_read_uid(spi_nand_t *nand, uint8_t id[SPI_NAND_UID_LEN])
{
	const uint8_t hdr[4] = { CMD_PAGE_READ, 0x00, 0x00, 0x00 };
	uint8_t config, status, copy[2 * SPI_NAND_UID_LEN];
	if (!spi_nand_get_feature(nand, SPI_NAND_FEATURE_CONFIG, &config)) {
		return false;
	}
	nand->cached = SPI_NAND_NO_PAGE;	// the cache is about to hold the ID page
	bool found = false;
	if (spi_nand_set_feature(nand, SPI_NAND_FEATURE_CONFIG, (uint8_t)(config | CONFIG_IDR_E)) &&
	    command(nand, hdr, sizeof(hdr), NULL, 0) && wait_ready(nand, PAGE_READ_POLLS, &status)) {
		for (uint32_t c = 0; c < UID_COPIES && (c + 1u) * sizeof(copy) <= nand->page_size && !found; c++) {
			if (!read_cache(nand, (uint16_t)(c * sizeof(copy)), copy, sizeof(copy))) {
				break;
			}
			found = true;
			for (uint32_t i = 0; i < SPI_NAND_UID_LEN; i++) {
				found = found && (uint8_t)(copy[i] ^ copy[SPI_NAND_UID_LEN + i]) == 0xFFu;
			}
		}
	}
	// Back to the normal array whatever happened: with IDR_E set, page reads return the ID and parameter pages
	bool restored = spi_nand_set_feature(nand, SPI_NAND_FEATURE_CONFIG, config);
	if (found) {
		for (uint32_t i = 0; i < SPI_NAND_UID_LEN; i++) {
			id[i] = copy[i];
		}
	}
	return found && restored;
}

bool spi_nand_read(spi_nand_t *nand, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *p = buf;
	if (nand->page_size == 0 || addr > spi_nand_size(nand) || len > spi_nand_size(nand) - addr) {
		return false;
	}
	while (len > 0) {
		uint32_t page = addr / nand->page_size;
		uint16_t column = (uint16_t)(addr % nand->page_size);
		uint32_t n = nand->page_size - column;
		if (n > len) {
			n = len;
		}
		if (!spi_nand_read_page(nand, page, column, p, n)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

bool spi_nand_block_bad(spi_nand_t *nand, uint32_t block, bool *bad)
{
	// The mark is judged from the data whatever the ECC status says (Kioxia's bad-block test flow): a marked page can
	// fail ECC
	uint8_t mark;
	if (block >= nand->blocks || !load(nand, block * nand->pages_per_block, true) ||
	    !read_cache(nand, nand->page_size, &mark, 1)) {
		return false;
	}
	*bad = mark != 0xFFu;
	return true;
}

static bool writable(const spi_nand_t *nand, uint32_t block)
{
	return nand->pages_per_block != 0 && block < nand->blocks && block >= nand->write_first &&
	       block - nand->write_first < nand->write_blocks;
}

// A program load: 02 clears the buffer first, 84 keeps what is there
static bool load_buffer(const spi_nand_t *nand, uint8_t cmd, uint16_t column, const void *data, uint32_t len)
{
	const spi_bus_t *bus = nand->bus;
	const uint8_t hdr[3] = { cmd, (uint8_t)(column >> 8), (uint8_t)column };
	bus->select(true);
	bool ok = bus->xfer(hdr, NULL, sizeof(hdr)) && (len == 0 || bus->xfer(data, NULL, len));
	bus->select(false);
	return ok;
}

static bool unlock(spi_nand_t *nand)
{
	return nand->lock_all == 0 || spi_nand_set_feature(nand, SPI_NAND_FEATURE_LOCK, 0x00);
}

static bool relock(spi_nand_t *nand)
{
	return nand->lock_all == 0 || spi_nand_set_feature(nand, SPI_NAND_FEATURE_LOCK, nand->lock_all);
}

// After a failure: lock again, but only if the chip is idle (while it's busy, only 0F and a reset may be sent). Left
// unlocked, the next program or erase locks it
static void relock_if_idle(spi_nand_t *nand)
{
	uint8_t status;
	if (spi_nand_get_feature(nand, SPI_NAND_FEATURE_STATUS, &status) && (status & SPI_NAND_STATUS_OIP) == 0) {
		relock(nand);
	}
}

// Write enable, the command with a row address, wait, lock again (the caller unlocked). fail_bit: the status bit that
// says the operation failed
static spi_nand_result_t execute(spi_nand_t *nand, uint8_t cmd, uint32_t row, uint32_t polls, uint8_t fail_bit)
{
	static const uint8_t wren = CMD_WRITE_ENABLE;
	const uint8_t hdr[4] = { cmd, (uint8_t)(row >> 16), (uint8_t)(row >> 8), (uint8_t)row };
	uint8_t status = 0;
	if (!command(nand, &wren, 1, NULL, 0) || !command(nand, hdr, sizeof(hdr), NULL, 0) ||
	    !wait_ready(nand, polls, &status)) {
		relock_if_idle(nand);
		return SPI_NAND_ERROR;
	}
	if (!relock(nand)) {
		return SPI_NAND_ERROR;
	}
	if (status & fail_bit) {
		nand->program_fails++;
		return SPI_NAND_FAILED;
	}
	return SPI_NAND_DONE;
}

spi_nand_result_t spi_nand_program(spi_nand_t *nand, uint32_t page, uint32_t src, const spi_nand_chunk_t *chunks,
				   unsigned n)
{
	static const uint8_t ff = 0xFF;
	static const uint8_t wren = CMD_WRITE_ENABLE;
	uint32_t pages = (uint32_t)nand->pages_per_block * nand->blocks;
	uint32_t total = (uint32_t)nand->page_size + nand->spare_size;
	if (page >= pages || !writable(nand, page / nand->pages_per_block) || (src != SPI_NAND_NO_PAGE && src >= pages)) {
		return SPI_NAND_ERROR;
	}
	for (unsigned i = 0; i < n; i++) {
		if (chunks[i].column > total || chunks[i].len > total - chunks[i].column) {
			return SPI_NAND_ERROR;
		}
	}
	// The buffer is about to change: whatever page it held isn't there any more
	nand->cached = SPI_NAND_NO_PAGE;
	if (!unlock(nand)) {
		return SPI_NAND_ERROR;
	}
	bool ok = true;
	unsigned i = 0;
	if (src == SPI_NAND_NO_PAGE) {
		// 02 clears the buffer: with the first chunk, or one 0xFF byte at column 0 when there is none
		ok = n > 0 ? load_buffer(nand, CMD_PROGRAM_LOAD, chunks[0].column, chunks[0].data, chunks[0].len) :
			     load_buffer(nand, CMD_PROGRAM_LOAD, 0, &ff, 1);
		i = 1;
	} else {
		// The data sheet's internal data move: 13, status (the source's ECC), write enable, 84 ..., 10
		const uint8_t hdr[4] = { CMD_PAGE_READ, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src };
		uint8_t status = 0;
		ok = command(nand, hdr, sizeof(hdr), NULL, 0) && wait_ready(nand, PAGE_READ_POLLS, &status) &&
		     ((status >> SPI_NAND_STATUS_ECC_SHIFT) & 3u) != SPI_NAND_ECC_FAILED;
		ok = ok && command(nand, &wren, 1, NULL, 0);
	}
	for (; ok && i < n; i++) {
		ok = load_buffer(nand, CMD_PROGRAM_RANDOM, chunks[i].column, chunks[i].data, chunks[i].len);
	}
	if (!ok) {
		relock_if_idle(nand);
		return SPI_NAND_ERROR;
	}
	return execute(nand, CMD_PROGRAM_EXECUTE, page, PROGRAM_POLLS, SPI_NAND_STATUS_P_FAIL);
}

spi_nand_result_t spi_nand_erase(spi_nand_t *nand, uint32_t block)
{
	if (!writable(nand, block)) {
		return SPI_NAND_ERROR;
	}
	if (nand->cached != SPI_NAND_NO_PAGE && nand->cached / nand->pages_per_block == block) {
		nand->cached = SPI_NAND_NO_PAGE;
	}
	if (!unlock(nand)) {
		return SPI_NAND_ERROR;
	}
	return execute(nand, CMD_BLOCK_ERASE, block * nand->pages_per_block, ERASE_POLLS, SPI_NAND_STATUS_E_FAIL);
}

spi_nand_result_t spi_nand_mark_bad(spi_nand_t *nand, uint32_t block)
{
	static const uint8_t zeros[64] = { 0 };
	if (!writable(nand, block)) {
		return SPI_NAND_ERROR;
	}
	(void)spi_nand_erase(nand, block);	// a worn block may not erase; the zeros go over whatever is left
	// The first chunk through 02, then the rest of the page and its spare area, 64 bytes at a time
	uint32_t total = (uint32_t)nand->page_size + nand->spare_size;
	uint32_t page = block * nand->pages_per_block;
	nand->cached = SPI_NAND_NO_PAGE;
	if (!unlock(nand)) {
		return SPI_NAND_ERROR;
	}
	bool ok = true;
	for (uint32_t col = 0; ok && col < total; col += sizeof(zeros)) {
		uint32_t len = total - col < sizeof(zeros) ? total - col : sizeof(zeros);
		ok = load_buffer(nand, col == 0 ? CMD_PROGRAM_LOAD : CMD_PROGRAM_RANDOM, (uint16_t)col, zeros, len);
	}
	if (!ok) {
		relock_if_idle(nand);
		return SPI_NAND_ERROR;
	}
	return execute(nand, CMD_PROGRAM_EXECUTE, page, PROGRAM_POLLS, SPI_NAND_STATUS_P_FAIL);
}
