// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The open-firmware CPS shim (core/cps) against a simulated flash holding an open-firmware codeplug, driven like qdmr's
// open-firmware UV380 driver does it: radio info, the session opening, 32-byte block reads of the regions it
// transfers, prepare / data / commit per sector, close, save and reboot. Plus request assembly from arbitrary chunks,
// the protected areas, the quick-key slots (written like the rest), the calibration window, the VFO overlay, overflows and the session time-out.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cps/cps_openfw.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x200000u	// 2 MB: the open-firmware layout fits (to 0x120000)
#define EE		0x000000u
#define FL		0x020000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = 0x1C0000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x1C0000u, .native_size = 0x40000u,
};

static uint8_t rom[4096];
static uint32_t radio_type(void) { return 8; }
static void sys_read(uint32_t addr, void *dst, size_t n) { memcpy(dst, &rom[addr - 0x08000000u], n); }
static bool sec_read(uint32_t addr, void *buf, uint32_t len)
{
	if (addr < 0x1000u || addr + len > 0x1100u) {
		return false;
	}
	memset(buf, 0xA0 + (addr >> 12), len);
	return true;
}
static const hal_system_t sys = { .read = sys_read };
static const hal_cps_t cps_info = { .radio_type = radio_type, .rom_base = 0x08000000u, .rom_size = sizeof(rom),
				    .security_read = sec_read };
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash,
			.system = &sys, .cps = &cps_info };

// --- the host side ---
static uint8_t out[8192];
static size_t out_len;
static int reboots, reboot_saved;
static uint32_t now = 1000;

static void io_write(const uint8_t *buf, size_t len)
{
	if (out_len + len <= sizeof(out)) {
		memcpy(out + out_len, buf, len);
	}
	out_len += len;
}
static void io_reboot(bool save) { reboots++; reboot_saved = save; }
static const cps_io_t io = { .write = io_write, .reboot = io_reboot };

// One request: fed in chunks of `chunk` bytes (0 = one go), then the host waits (ticks) for the reply
static size_t request(const uint8_t *buf, size_t len, size_t chunk)
{
	out_len = 0;
	for (size_t off = 0; off < len;) {
		size_t n = chunk == 0 || len - off < chunk ? len - off : chunk;
		cps_feed(buf + off, n, now);
		off += n;
	}
	for (int i = 0; i < CPS_STALL_MS / 5 + 3 && out_len == 0; i++) {
		now += 5;
		cps_feed(NULL, 0, now);
	}
	now += 1;
	return out_len;
}

static bool is_nak(void) { return out_len == 1 && out[0] == '-'; }

static bool control(uint8_t sub, uint8_t arg)
{
	uint8_t r[3] = { 'C', sub, arg };
	request(r, sub == 6 ? 3 : 2, 0);
	return is_nak();	// '-' = done
}

static bool read_block(uint8_t area, uint32_t addr, uint16_t len, uint8_t *dst)
{
	uint8_t r[8] = { 'R', area, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
			 (uint8_t)(len >> 8), (uint8_t)len };
	request(r, sizeof(r), 0);
	if (out_len != 3u + len || out[0] != 'R' || ((out[1] << 8) | out[2]) != len) {
		return false;
	}
	memcpy(dst, out + 3, len);
	return true;
}

static bool x_prepare(uint32_t sector)
{
	uint8_t r[5] = { 'X', 1, (uint8_t)(sector >> 16), (uint8_t)(sector >> 8), (uint8_t)sector };
	request(r, sizeof(r), 0);
	return out_len == 2 && out[0] == 'X' && out[1] == 1;
}

static bool x_data(uint32_t addr, const uint8_t *data, uint16_t len, size_t chunk)
{
	uint8_t r[8 + 2048];
	r[0] = 'X';
	r[1] = 2;
	r[2] = (uint8_t)(addr >> 24); r[3] = (uint8_t)(addr >> 16); r[4] = (uint8_t)(addr >> 8); r[5] = (uint8_t)addr;
	r[6] = (uint8_t)(len >> 8); r[7] = (uint8_t)len;
	memcpy(r + 8, data, len);
	request(r, 8u + len, chunk);
	return out_len == 2 && out[0] == 'X' && out[1] == 2;
}

static bool x_commit(void)
{
	uint8_t r[2] = { 'X', 3 };
	request(r, 2, 0);
	return out_len == 2 && out[0] == 'X' && out[1] == 3;
}

// qdmr's regions for the UV380 family (open-firmware-cps.md 7.1), wire addresses
typedef struct { uint32_t addr, len; } region_t;
static const region_t regions[] = {
	{ 0x00080, 0x5FE0 }, { 0x07500, 0x3B00 }, { 0x20000, 0x11A0 }, { 0x9B000, 0x13E60 },
};

// Read every region in 32-byte blocks into img (same layout as the flash)
static bool qdmr_read(uint8_t *img)
{
	bool ok = control(0, 0) && control(1, 0);
	uint8_t line[] = { 'C', 2, 0, 0, 3, 1, 0, 'R', 'e', 'a', 'd', 'i', 'n', 'g', 0 };
	request(line, sizeof(line), 0);
	ok = ok && is_nak() && control(3, 0) && control(6, 3) && control(6, 2);
	for (unsigned r = 0; ok && r < sizeof(regions) / sizeof(regions[0]); r++) {
		for (uint32_t off = 0; ok && off < regions[r].len; off += 32) {
			uint32_t n = regions[r].len - off < 32 ? regions[r].len - off : 32;
			ok = read_block(1, regions[r].addr + off, (uint16_t)n, img + regions[r].addr + off);
		}
	}
	return ok && control(5, 0);
}

// Write every region: per 4 KB sector prepare, 32-byte data blocks, commit
static bool qdmr_write(const uint8_t *img, bool reboot)
{
	bool ok = control(0, 0) && control(1, 0) && control(3, 0) && control(6, 4) && control(6, 2);
	for (unsigned r = 0; ok && r < sizeof(regions) / sizeof(regions[0]); r++) {
		uint32_t a = regions[r].addr, end = regions[r].addr + regions[r].len;
		while (ok && a < end) {
			uint32_t sector = a / 4096u, send = (sector + 1u) * 4096u;
			ok = x_prepare(sector);
			for (; ok && a < end && a < send; a += 32) {
				uint32_t n = end - a < 32 ? end - a : 32;
				ok = x_data(a, img + a, (uint16_t)n, 0);
			}
			ok = ok && x_commit();
		}
	}
	ok = ok && control(5, 0);
	if (reboot) {
		ok = ok && control(6, 0);
	}
	return ok;
}

// --- an open-firmware codeplug to start from ---

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	cps_init(&io);
}

static void populate(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	for (uint16_t n = 1; n <= 300; n += 7) {
		channel_t c;
		memset(&c, 0, sizeof(c));
		snprintf(c.name, sizeof(c.name), "Ch %u", n);
		c.rx_hz = c.tx_hz = 145000000u + n * 12500u;
		c.power = CODEPLUG_POWER_DEFAULT;
		c.timeslot = 1;
		CHECK(codeplug_set_channel(n, &c));
	}
	zone_t z;
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Zone");
	z.count = 3;
	z.channels[0] = 1; z.channels[1] = 8; z.channels[2] = 295;
	CHECK(codeplug_set_zone(0, &z));
	contact_t ct = { "TG 9", 9, CONTACT_GROUP, 0 };
	CHECK(codeplug_set_contact(1, &ct));
	// Quick keys and a calibration table in place, as on a radio that ran the aftermarket firmware
	for (unsigned i = 0; i < 20; i++) {
		sim_flash_data[EE + 0x7524 + i] = (uint8_t)(0x40 + i);
	}
	for (unsigned i = 0; i < 0x200; i++) {
		sim_flash_data[0x10000 + i] = (uint8_t)i;
	}
	codeplug_init();
}

static void test_info(void)
{
	uint8_t r[8] = { 'R', 9, 0, 0, 0, 0, 0, 0x20 };	// asks 32, gets the structure's 46
	request(r, sizeof(r), 0);
	CHECK(out_len == 3 + 46 && out[0] == 'R' && out[2] == 46);
	CHECK(out[3] == 3 && out[4] == 0 && out[7] == 8);	// struct version 3, radio type 8
	CHECK(out[3 + 40] == 0x15 && out[3 + 41] == 0x40);	// 2 MB: capacity code 0x15
	CHECK(out[3 + 24] == '2' && out[3 + 24 + 13] >= '0');	// build date YYYYMMDDhhmmss
	// Area 5 (MCU ROM) and 10 (security registers); 6, 7, 8 refused
	uint8_t b[64];
	rom[100] = 0x5A;
	CHECK(read_block(5, 100, 16, b) && b[0] == 0x5A);
	uint8_t big[8] = { 'R', 5, 0, 0, 0x0F, 0xF0, 0, 0x20 };
	request(big, 8, 0);
	CHECK(is_nak());					// past the ROM
	CHECK(read_block(10, 0x1000, 32, b) && b[0] == 0xA1);
	uint8_t a6[8] = { 'R', 6, 0, 0, 0, 0, 0, 0x20 };
	request(a6, 8, 0);
	CHECK(is_nak());
	a6[1] = 8;
	request(a6, 8, 0);
	CHECK(is_nak());
}

static void test_read_session(void)
{
	uint8_t *img = calloc(1, FLASH_SIZE);
	CHECK(qdmr_read(img));
	for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
		CHECK(memcmp(img + regions[r].addr, sim_flash_data + regions[r].addr, regions[r].len) == 0);
	}
	// Big reads (the open-firmware CPS uses up to the buffer size) and area 2 = the same bytes
	uint8_t b[2048], c[2048];
	CHECK(read_block(1, 0x3780, 2045, b) && memcmp(b, sim_flash_data + 0x3780, 2045) == 0);
	CHECK(read_block(2, 0x3780, 2045, c) && memcmp(b, c, 2045) == 0);
	uint8_t over[8] = { 'R', 1, 0, 0, 0, 0, 0x10, 0x00 };	// 4096: more than the original firmware's 2045, all of it
	request(over, 8, 0);
	CHECK(out_len == 3 + 4096 && out[1] == 0x10 && out[2] == 0x00 && memcmp(out + 3, sim_flash_data, 4096) == 0);
	uint8_t past[8] = { 'R', 2, 0, 0, 0xFF, 0xF0, 0, 0x20 };	// past the 64 KB EEPROM image
	request(past, 8, 0);
	CHECK(is_nak());
	free(img);
}

static void test_write_session(void)
{
	uint8_t *img = calloc(1, FLASH_SIZE);
	cps_stats_t st;
	channel_t ch;
	uint32_t serial = codeplug_serial();

	CHECK(qdmr_read(img));
	// Edit like a CPS: rename channel 8, add channel 2, change the zone's name, set the quick-key slots (the side
	// key assignment lives there: the CPS may write them)
	memcpy(img + EE + 0x3790 + 56u * 7u, "Renamed\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16);
	memcpy(img + EE + 0x3790 + 56u * 1u, img + EE + 0x3790, 56);
	memcpy(img + EE + 0x3790 + 56u * 1u, "Copy\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16);
	img[EE + 0x3780] |= 0x02;
	memcpy(img + EE + 0x8030, "Mine\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16);
	for (unsigned i = 0; i < 20; i++) {
		img[EE + 0x7524 + i] = (uint8_t)(0x10 + i);
	}
	CHECK(qdmr_write(img, true));
	CHECK(reboots == 1 && reboot_saved == 1);
	cps_get_stats(&st);
	CHECK(st.commits > 10 && !st.pending && !st.session);
	// Close re-read the codeplug: the radio core sees it through the serial
	CHECK(codeplug_serial() != serial);
	CHECK(codeplug_channel(8, &ch) && strcmp(ch.name, "Renamed") == 0);
	CHECK(codeplug_channel(2, &ch) && strcmp(ch.name, "Copy") == 0 && ch.rx_hz == 145012500u);
	zone_t z;
	CHECK(codeplug_zone(0, &z) && strcmp(z.name, "Mine") == 0);
	for (unsigned i = 0; i < 20; i++) {
		CHECK(sim_flash_data[EE + 0x7524 + i] == 0x10 + i);	// the quick-key slots written
	}
	// Everything is what the CPS sent
	for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
		CHECK(memcmp(img + regions[r].addr, sim_flash_data + regions[r].addr, regions[r].len) == 0);
	}
	// Read back = written
	uint8_t *back = calloc(1, FLASH_SIZE);
	CHECK(qdmr_read(back));
	for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
		CHECK(memcmp(img + regions[r].addr, back + regions[r].addr, regions[r].len) == 0);
	}
	// Writing back what was read changes nothing: every sector is skipped
	storage_stats_t s0, s1;
	storage_get_stats(&s0);
	CHECK(qdmr_write(back, false));
	storage_get_stats(&s1);
	CHECK(s1.sector_writes == s0.sector_writes && s1.sector_skips > s0.sector_skips);
	CHECK(sim_flash_violations() == 0);
	free(img);
	free(back);
}

static void test_protected(void)
{
	uint8_t d[32];
	memset(d, 0x11, sizeof(d));
	// The radio's own areas and anything outside the open-firmware images
	CHECK(!x_prepare(0x11));	// settings
	CHECK(!x_prepare(0x19));	// journal
	CHECK(!x_prepare(0x1C));	// gap after the journal: no CPS address maps there
	CHECK(!x_prepare(0x1C0));	// native codeplug reserve
	CHECK(!x_prepare(0x7FF));	// past the flash
	CHECK(!x_commit() && !x_data(0x3000, d, 32, 0));	// nothing pending
	// One pending sector at a time
	CHECK(x_prepare(0x3));
	CHECK(!x_prepare(0x4));		// fails, and drops the pending one
	CHECK(!x_commit());
	// Data outside the pending sector is ignored
	uint8_t before[64];
	memcpy(before, sim_flash_data + 0x4000, 64);
	CHECK(x_prepare(0x3) && x_data(0x3FF0, d, 32, 0) && x_commit());
	CHECK(memcmp(sim_flash_data + 0x4000, before, 16) == 0 && sim_flash_data[0x3FF0] == 0x11);
	// The calibration sector: only its 512-byte window takes the CPS's bytes
	uint8_t w[32];
	memset(w, 0x77, sizeof(w));
	CHECK(x_prepare(0x10) && x_data(0x101F0, w, 32, 0) && x_commit());
	CHECK(sim_flash_data[0x101F0] == 0x77 && sim_flash_data[0x101FF] == 0x77 && sim_flash_data[0x10200] == 0xFF);
	// W is the STM32 family's fake write: acknowledged, nothing written
	uint8_t wr[5] = { 'W', 1, 0, 0, 3 };
	request(wr, 5, 0);
	CHECK(out_len == 2 && out[0] == 'W');
	uint8_t x7[10] = { 'X', 7, 0, 0, 0, 0, 0, 2, 1, 2 };
	request(x7, sizeof(x7), 0);
	CHECK(is_nak());
}

static void test_assembly(void)
{
	uint8_t data[1024], b[64];
	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i * 3 + 0xA5);	// 0xA5 bytes inside: data, not a debug frame
	}
	// A 1 KB write in 64-byte USB packets, and in odd chunks
	CHECK(x_prepare(0x5) && x_data(0x5000, data, sizeof(data), 64) && x_data(0x5400, data, 1000, 7) && x_commit());
	CHECK(memcmp(sim_flash_data + 0x5000, data, 1024) == 0 && memcmp(sim_flash_data + 0x5400, data, 1000) == 0);
	// A read header split byte by byte
	uint8_t r[8] = { 'R', 1, 0, 0, 0x50, 0x00, 0, 16 };
	request(r, 8, 1);
	CHECK(out_len == 19 && memcmp(out + 3, data, 16) == 0);
	// A read header in two pieces with more than the gap between them (a UART's two interrupts, the task held up):
	// still one read, not a '-' for each piece
	out_len = 0;
	cps_feed(r, 4, now);
	now += CPS_GAP_MS + 2;
	cps_feed(NULL, 0, now);
	CHECK(out_len == 0 && cps_mid_request());	// the host link keeps the rest for the engine
	cps_feed(r + 4, 4, now);
	CHECK(out_len == 19 && out[0] == 'R' && memcmp(out + 3, data, 16) == 0 && !cps_mid_request());
	now += 10;
	// ... but a piece that never gets its rest is answered '-' after the stall
	out_len = 0;
	cps_feed(r, 4, now);
	now += CPS_STALL_MS;
	cps_feed(NULL, 0, now);
	CHECK(is_nak());
	now += 10;
	// Two requests in one chunk: two replies
	uint8_t two[16] = { 'R', 1, 0, 0, 0x50, 0x00, 0, 4, 'R', 1, 0, 0, 0x50, 0x04, 0, 4 };
	request(two, 16, 0);
	CHECK(out_len == 14 && out[7] == 'R' && memcmp(out + 10, data + 4, 4) == 0);
	// C 2 with a NUL, and without one (ends at the gap): one '-' each
	uint8_t l1[] = { 'C', 2, 0, 16, 1, 0, 0, 'H', 'i', 0 };
	request(l1, sizeof(l1), 0);
	CHECK(is_nak());
	uint8_t l2[] = { 'C', 2, 0, 16, 1, 0, 0, 'N', 'o', 'n', 'u', 'l' };
	request(l2, sizeof(l2), 0);
	CHECK(is_nak());
	// C 6 7 (clock, 4 more bytes) and C 254 ping
	uint8_t clk[7] = { 'C', 6, 7, 1, 2, 3, 4 };
	request(clk, 7, 3);
	CHECK(is_nak());
	CHECK(control(254, 0));
	// D: our data service is not on this radio yet: "failed"
	uint8_t d[3] = { 'D', 5, 0 };
	request(d, 2, 0);
	CHECK(out_len == 3 && out[0] == 'D' && out[1] == 5 && out[2] == 0);
	// Noise (the DM-1801 Pro's cable: bursts of 0xFC / 0xFE): dropped and counted, never answered, so a stray '-'
	// can't be taken for the next request's reply; noise right in front of a read doesn't spoil it
	uint8_t junk[5] = { 0x01, 0xFC, 0xFE, 0xFF, 0x05 };
	cps_stats_t js;
	cps_get_stats(&js);
	uint32_t junk0 = js.junk;
	request(junk, 5, 0);
	CHECK(out_len == 0);
	uint8_t jr[3 + 8] = { 0xFC, 0xFC, 0xFE, 'R', 1, 0, 0, 0x50, 0x00, 0, 4 };
	request(jr, sizeof(jr), 0);
	CHECK(out_len == 7 && out[0] == 'R' && memcmp(out + 3, data, 4) == 0);
	cps_get_stats(&js);
	CHECK(js.junk == junk0 + 8 && !cps_mid_request());
	// Overflow: a write header asking for more than the buffer holds; the bytes are dropped until the gap
	uint8_t *huge = malloc(3000);
	memset(huge, 0x33, 3000);
	huge[0] = 'X'; huge[1] = 2; huge[2] = 0; huge[3] = 0; huge[4] = 0x50; huge[5] = 0; huge[6] = 0x0B; huge[7] = 0xB0;
	request(huge, 3000, 64);
	CHECK(is_nak());
	cps_stats_t st;
	cps_get_stats(&st);
	CHECK(st.overflows == 1);
	free(huge);
	CHECK(read_block(1, 0x5000, 16, b) && memcmp(b, data, 16) == 0);	// back in step
}

static void test_vfo(void)
{
	uint8_t b[96], rec[56];
	settings_t st;
	channel_t ch;
	// The live VFO shows in reads that touch VFO A
	settings_t *w = settings_begin();
	w->vfo_rx_hz = 146520000;
	w->vfo_tx_hz = 146520000;
	w->vfo_bw = RADIO_BW_25K;
	w->vfo_power = SETTINGS_UNSET8;
	settings_end(now);
	CHECK(read_block(1, EE + 0x7580, 96, b));
	memcpy(rec, b + 0x10, 56);
	CHECK(codeplug_openfw_decode(rec, &ch) && ch.rx_hz == 146520000 && ch.bw == RADIO_BW_25K);
	CHECK(read_block(1, EE + 0x75A0, 8, b) && b[0] == 0x00 && b[3] == 0x14);	// bytes 0x10-0x13 of the record
	// Written back as read: the settings stay; changed by the CPS: the VFO takes it
	uint8_t sec[4096];
	CHECK(read_block(1, EE + 0x7000, 2045, sec) && read_block(1, EE + 0x77FD, 2045, sec + 2045) &&
	      read_block(1, EE + 0x7FFA, 6, sec + 4090));
	CHECK(x_prepare(0x7));
	for (unsigned off = 0; off < 4096; off += 1024) {
		CHECK(x_data(EE + 0x7000 + off, sec + off, 1024, 64));
	}
	CHECK(x_commit());
	settings_read(&st);
	CHECK(st.vfo_rx_hz == 146520000 && st.vfo_bw == RADIO_BW_25K);
	ch.rx_hz = ch.tx_hz = 433500000;
	ch.bw = RADIO_BW_12K5;
	codeplug_openfw_encode(&ch, &sec[0x590]);
	CHECK(x_prepare(0x7) && x_data(EE + 0x7590, &sec[0x590], 56, 0) && x_commit());
	settings_read(&st);
	CHECK(st.vfo_rx_hz == 433500000 && st.vfo_bw == RADIO_BW_12K5);
}

static void test_session_timeout(void)
{
	CHECK(x_prepare(0x6));
	CHECK(storage_buffer_acquire() == NULL);	// the CPS holds the sector buffer
	now += CPS_SESSION_MS + 10;
	cps_feed(NULL, 0, now);
	cps_stats_t st;
	cps_get_stats(&st);
	CHECK(!st.pending && !st.session);
	uint8_t *b = storage_buffer_acquire();
	CHECK(b != NULL);
	storage_buffer_release();
	CHECK(!x_commit());
}

// C 6 0 / 1 reboot only in programming mode (after C 0): "C 6 1" alone on a link shared with the debug link
// rebooted the radio (bench 2026-10-15)
static void test_stray_reboot(void)
{
	cps_stats_t st;
	int before = reboots;

	now += CPS_SESSION_MS + 10;
	CHECK(control(6, 1) && control(6, 0));
	cps_feed(NULL, 0, now);
	cps_get_stats(&st);
	CHECK(reboots == before && st.reboots_ignored == 2);
	// C 0, then a pause shorter than the session time: taken
	CHECK(control(0, 0));
	now += CPS_SESSION_MS - 10;
	CHECK(control(6, 1));
	cps_feed(NULL, 0, now);
	CHECK(reboots == before + 1 && reboot_saved == 0);
	// Once is enough: the next one needs C 0 again
	CHECK(control(6, 1));
	cps_feed(NULL, 0, now);
	CHECK(reboots == before + 1);
	// C 0, then a longer quiet: programming mode is over
	CHECK(control(0, 0));
	now += CPS_SESSION_MS + 10;
	CHECK(control(6, 0));
	cps_feed(NULL, 0, now);
	cps_get_stats(&st);
	CHECK(reboots == before + 1 && st.reboots_ignored == 4);
}

// The settings / journal region holds another firmware's data: reads work, every write is refused until a claim
static void test_foreign(void)
{
	uint8_t d[32], img[32];
	populate();
	for (uint32_t i = 0; i < 0xA000u; i++) {
		sim_flash_data[0x11000u + i] = (uint8_t)(0x30 + i * 11u);	// ring and journal: no record of ours
	}
	sim_flash_power_on();
	CHECK(!storage_init());
	settings_init();
	codeplug_init();
	cps_init(&io);
	CHECK(storage_region() == STORAGE_REGION_FOREIGN);
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	CHECK(read_block(1, 0x3790, 32, img));		// reads still answered
	memset(d, 0x11, sizeof(d));
	CHECK(!x_prepare(0x3));				// no CPS write through a foreign journal
	CHECK(!x_commit() && !x_data(0x3000, d, 32, 0));
	channel_t c;
	memset(&c, 0, sizeof(c));
	strcpy(c.name, "X");
	c.rx_hz = c.tx_hz = 145500000u;
	CHECK(!codeplug_set_channel(2, &c));		// nor a radio-side edit
	CHECK(control(6, 2));				// "save the settings": answered, nothing written
	CHECK(memcmp(snap, sim_flash_data, FLASH_SIZE) == 0);
	// Claimed (the debug link), erased by the housekeeping: writes work again
	CHECK(storage_claim_request());
	settings_poll(now);
	CHECK(storage_ready());
	CHECK(x_prepare(0x3) && x_data(0x3000, d, 32, 0) && x_commit() && control(5, 0));
	CHECK(sim_flash_data[0x3000] == 0x11);
	free(snap);
}

// Requests a host shouldn't send: reads of nothing, past the image or the flash, too long for a reply, a security
// register out of range, a data block cut short, a commit the flash fails; and a commit cut by a power loss comes
// back whole at the next boot
static bool r_nak(uint8_t area, uint32_t addr, uint16_t len)
{
	uint8_t r[8] = { 'R', area, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
			 (uint8_t)(len >> 8), (uint8_t)len };
	request(r, sizeof(r), 0);
	return is_nak();
}

static void test_bad_requests(void)
{
	uint8_t b[64], d[32];
	CHECK(r_nak(1, 0x5000, 0));			// nothing
	CHECK(r_nak(2, 0xFFF0, 0x20));			// past the 64 KB EEPROM image
	CHECK(r_nak(2, 0x10000, 1));
	CHECK(r_nak(1, FLASH_SIZE - 0x10, 0x20));	// past the flash
	CHECK(r_nak(1, 0xFFFFFFF0u, 0x20));		// wrapping round
	uint8_t rl[8] = { 'R', 1, 0, 0, 0x50, 0x00, (uint8_t)((CPS_REPLY_MAX + 1) >> 8), (uint8_t)(CPS_REPLY_MAX + 1) };
	request(rl, sizeof(rl), 0);			// longer than a reply: clipped to the longest
	CHECK(out_len == 3u + CPS_REPLY_MAX && ((out[1] << 8) | out[2]) == CPS_REPLY_MAX);
	CHECK(r_nak(10, 0x2000, 0x20));			// a security register the board doesn't have
	CHECK(r_nak(10, 0x10F0, 0x20));			// ... running past one
	CHECK(r_nak(200, 0, 0x20));			// no such area
	CHECK(read_block(2, 0x5000, 16, b) && memcmp(b, sim_flash_data + EE + 0x5000, 16) == 0);
	CHECK(read_block(1, FLASH_SIZE - 0x20, 0x20, b));	// the very end: fine

	// A data block shorter than its header says: taken at the gap, only the bytes that came
	memset(d, 0x5C, sizeof(d));
	CHECK(x_prepare(0x6));
	uint8_t r[8 + 10] = { 'X', 2, 0, 0, 0x60, 0x00, 0, 100 };
	memcpy(r + 8, d, 10);
	uint8_t before = sim_flash_data[0x600A];
	request(r, sizeof(r), 0);
	CHECK(out_len == 2 && out[0] == 'X' && out[1] == 2);
	CHECK(x_commit());
	CHECK(sim_flash_data[0x6000] == 0x5C && sim_flash_data[0x6009] == 0x5C && sim_flash_data[0x600A] == before);

	// The flash fails the commit: '-', nothing pending afterwards, the sector as it was
	uint8_t old[4096];
	memcpy(old, sim_flash_data + 0x7000, sizeof(old));
	CHECK(x_prepare(0x7) && x_data(0x7000, d, 32, 0));
	sim_flash_bad_range(0x7000, 0x1000);
	CHECK(!x_commit());
	sim_flash_bad_range(0, 0);
	CHECK(!x_commit());				// dropped
	cps_stats_t st;
	cps_get_stats(&st);
	CHECK(!st.pending);

	// A power cut in the middle of a commit: the next boot finishes it from the journal
	memcpy(old, sim_flash_data + 0x8000, sizeof(old));
	CHECK(x_prepare(0x8) && x_data(0x8100, d, 32, 0));
	sim_flash_cut_after(4096 + 50);		// J1 written (1 + 4096 units), J0's header (1 + 16), the target erased
						// (1), 31 bytes of it programmed
	CHECK(!x_commit());
	boot();
	CHECK(memcmp(sim_flash_data + 0x8100, d, 32) == 0 && memcmp(sim_flash_data + 0x8000, old, 0x100) == 0 &&
	      memcmp(sim_flash_data + 0x8120, old + 0x120, 4096 - 0x120) == 0);
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	populate();
	test_info();
	test_read_session();
	test_write_session();
	test_protected();
	test_assembly();
	test_bad_requests();
	test_vfo();
	test_session_timeout();
	test_stray_reboot();
	test_foreign();
	sim_flash_free();
	printf("# cpsTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
