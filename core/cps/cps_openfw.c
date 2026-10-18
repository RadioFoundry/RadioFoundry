// SPDX-License-Identifier: MIT Copyright (c) 2026 The OpenDigitalRadio Authors The open-firmware CPS protocol engine
// (cps_openfw.h). Requests are assembled from the byte stream: the host link sees no USB packet boundaries, so a
// request ends when its header says it is complete (R, W/X, most C commands), or after CPS_GAP_MS of quiet for the rest
// (C 2 text without a NUL, D, unknown bytes). A request still short of its header's length waits CPS_STALL_MS, so a
// UART that delivers it in pieces doesn't cut it short; the PC waits for each reply before it sends again.
#include <string.h>
#include "cps/cps_openfw.h"
#include "board.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "storage/storage.h"

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

#define NAK		0x2D	// '-': "error", and also the normal reply to every C command
#define CHUNK		64u
#define INFO_LEN	46u

// EEPROM image ranges with special handling (open-firmware-cps.md 2.2, 7.2)
#define EE_VFO_A	0x7590u
#define EE_IMAGE_SIZE	0x10000u

static const cps_io_t *io;
static uint8_t req[CPS_REQUEST_MAX];
static size_t req_len;
static bool overflow;
static uint32_t last_rx_ms;
static cps_stats_t stats;

// The pending sector (X 1 .. X 3)
static uint8_t *sector;			// the shared storage buffer while a sector is pending
static uint32_t sector_addr;
static bool sector_cal;			// the calibration sector: only the window may change
static bool image_written;		// a commit since the session began: re-read the codeplug at its end
static int8_t reboot_req = -1;		// C 6 0 / 1 seen: reboot (1 = save first) once the bytes are handled
static bool programming;		// C 0 seen, and no quiet longer than CPS_SESSION_MS since: C 6 0 / 1 may reboot

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void send(const uint8_t *buf, size_t len)
{
	if (io != NULL && io->write != NULL) {
		io->write(buf, len);
	}
}

static void nak(void)
{
	static const uint8_t b = NAK;
	send(&b, 1);
}

static void refuse(void)
{
	stats.refused++;
	nak();
}

static const hal_flash_map_t *map(void)
{
	return storage_map();
}

static uint32_t flash_size(void)
{
	return board.flash != NULL ? board.flash->size : 0;
}

static void drop_sector(void)
{
	if (sector != NULL) {
		storage_buffer_release();
		sector = NULL;
	}
	stats.pending = false;
}

static void end_session(void)
{
	drop_sector();
	if (image_written) {
		image_written = false;
		codeplug_reload();	// the radio core follows codeplug_serial()
	}
	stats.session = false;
}

// --- The live VFO in the image (open-firmware-cps.md 2.2 "VFO overlay") ---

static bool vfo_from_settings(channel_t *ch)
{
	settings_t st;
	settings_read(&st);
	if (st.vfo_rx_hz == 0) {
		return false;
	}
	memset(ch, 0, sizeof(*ch));
	strcpy(ch->name, "VFO A");
	ch->rx_hz = st.vfo_rx_hz;
	ch->tx_hz = st.vfo_tx_hz != 0 ? st.vfo_tx_hz : st.vfo_rx_hz;
	ch->mode = settings_vfo_mode(&st);
	ch->bw = settings_vfo_bw(&st);
	ch->rx_tone = (subtone_t){ st.vfo_rx_tone_type, st.vfo_rx_tone };
	ch->tx_tone = (subtone_t){ st.vfo_tx_tone_type, st.vfo_tx_tone };
	ch->power = st.vfo_power == SETTINGS_UNSET8 ? CODEPLUG_POWER_DEFAULT : st.vfo_power;
	ch->color_code = settings_vfo_cc(&st);
	ch->timeslot = settings_vfo_ts(&st);
	return true;
}

// The VFO A record as the radio would show it, built on the bytes stored (so the vendor bytes stay)
static bool vfo_record(uint8_t *rec)
{
	channel_t ch;
	uint32_t a = map()->openfw_eeprom + EE_VFO_A;
	if (!vfo_from_settings(&ch) || !board.flash->read(a, rec, CODEPLUG_OPENFW_CHANNEL_SIZE)) {
		return false;
	}
	codeplug_openfw_encode(&ch, rec);
	return true;
}

// Replace the bytes of [addr, addr + len) that fall in the VFO A record with the live VFO
static void overlay_vfo(uint32_t addr, uint8_t *buf, uint32_t len, const uint8_t *rec)
{
	uint32_t v = map()->openfw_eeprom + EE_VFO_A;
	for (uint32_t i = 0; i < CODEPLUG_OPENFW_CHANNEL_SIZE; i++) {
		if (v + i >= addr && v + i < addr + len) {
			buf[v + i - addr] = rec[i];
		}
	}
}

void cps_openfw_vfo_overlay(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t rec[CODEPLUG_OPENFW_CHANNEL_SIZE];
	const hal_flash_map_t *m = map();
	uint32_t v = m != NULL ? m->openfw_eeprom + EE_VFO_A : 0;
	if (m != NULL && addr < v + CODEPLUG_OPENFW_CHANNEL_SIZE && addr + len > v && vfo_record(rec)) {
		overlay_vfo(addr, buf, len, rec);
	}
}

// A commit that changed the VFO A record (the CPS edited it): the VFO takes the new settings at the next boot
void cps_openfw_vfo_written(uint32_t addr, const uint8_t *data, uint32_t now_ms)
{
	uint32_t v = map()->openfw_eeprom + EE_VFO_A;
	uint8_t live[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t ch;
	if (v < addr || v + CODEPLUG_OPENFW_CHANNEL_SIZE > addr + STORAGE_SECTOR) {
		return;
	}
	const uint8_t *rec = &data[v - addr];
	if ((vfo_record(live) && memcmp(live, rec, sizeof(live)) == 0) || !codeplug_openfw_decode(rec, &ch)) {
		return;		// unchanged (written back as read), or not a valid record
	}
	settings_t *st = settings_begin();
	st->vfo_rx_hz = ch.rx_hz;
	st->vfo_tx_hz = ch.tx_hz;
	settings_set_vfo_mode_bw(st, ch.mode == RADIO_MODE_DMR && CONFIG_VFO_DMR ? RADIO_MODE_DMR :
				 CONFIG_VFO_FM ? RADIO_MODE_FM : RADIO_MODE_DMR, ch.bw);
	st->vfo_rx_tone_type = ch.rx_tone.type;
	st->vfo_rx_tone = ch.rx_tone.value;
	st->vfo_tx_tone_type = ch.tx_tone.type;
	st->vfo_tx_tone = ch.tx_tone.value;
	st->vfo_power = ch.power == CODEPLUG_POWER_DEFAULT ? SETTINGS_UNSET8 : ch.power;
	settings_set_vfo_cc_ts(st, ch.color_code, ch.timeslot);
	settings_end(now_ms);
}

// --- R: read ---

static void radio_info(void)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	uint8_t r[3 + INFO_LEN];
	uint8_t *p = &r[3];
	const char *d = __DATE__;	// "Mmm dd yyyy"
	const char *t = __TIME__;	// "hh:mm:ss"
	int m = 1;

	memset(r, 0, sizeof(r));
	r[0] = 'R';
	r[1] = 0;
	r[2] = INFO_LEN;
	put32le(&p[0], 3);
	put32le(&p[4], board.cps != NULL && board.cps->radio_type != NULL ? board.cps->radio_type() : 6);
	strncpy((char *)&p[8], ODR_GIT, 15);
	for (int i = 0; i < 12; i++) {
		if (strncmp(&months[3 * i], d, 3) == 0) {
			m = i + 1;
		}
	}
	char *b = (char *)&p[24];
	memcpy(b, &d[7], 4);
	b[4] = (char)('0' + m / 10);
	b[5] = (char)('0' + m % 10);
	b[6] = d[4] == ' ' ? '0' : d[4];
	b[7] = d[5];
	memcpy(&b[8], &t[0], 2);
	memcpy(&b[10], &t[3], 2);
	memcpy(&b[12], &t[6], 2);
	// Flash part id: 0x40 << 8 | log2(size), as the W25Q JEDEC capacity code
	uint32_t size = flash_size(), code = 0;
	while (code < 31 && (1u << code) < size) {
		code++;
	}
	put32le(&p[40], size != 0 ? 0x4000u | code : 0);
	p[44] = 0;		// features: no inverted display, no voice prompts
	p[45] = 0;
	send(r, sizeof(r));
	stats.reads++;
}

// A read reply's header goes out in the same write as the first data: qdmr takes a reply as soon as part of it is in,
// and a 32-byte block written as header, then data reached it short (the late data was then read as the next reply)
static size_t reply_header(uint8_t *c, uint16_t len)
{
	c[0] = 'R';
	c[1] = (uint8_t)(len >> 8);
	c[2] = (uint8_t)len;
	return 3;
}

// Stream len bytes from the data flash at addr; the VFO overlay when the range touches VFO A
static void read_flash(uint32_t addr, uint16_t len)
{
	uint8_t c[3 + CHUNK];
	uint8_t vfo[CODEPLUG_OPENFW_CHANNEL_SIZE];
	const hal_flash_map_t *m = map();
	uint32_t v = m != NULL ? m->openfw_eeprom + EE_VFO_A : 0;
	bool overlay = m != NULL && addr < v + CODEPLUG_OPENFW_CHANNEL_SIZE && addr + len > v && vfo_record(vfo);

	// Check the whole range can be read before the header goes out (a failure later can't be a '-' any more)
	if (!board.flash->read(addr, c, 1)) {
		refuse();
		return;
	}
	size_t h = reply_header(c, len);
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!board.flash->read(addr + off, c + h, n)) {
			memset(c + h, 0xFF, n);
		}
		if (overlay) {
			overlay_vfo(addr + off, c + h, n, vfo);
		}
		send(c, h + n);
		h = 0;
	}
	stats.reads++;
}

// A read through a byte reader (MCU ROM, security registers), chunk by chunk
static void read_other(bool rom, uint32_t addr, uint16_t len)
{
	uint8_t c[3 + CHUNK];
	const hal_cps_t *cps = board.cps;

	if (rom) {
		if (cps == NULL || cps->rom_size == 0 || board.system == NULL || board.system->read == NULL ||
		    addr >= cps->rom_size || len > cps->rom_size - addr) {
			refuse();
			return;
		}
	} else if (cps == NULL || cps->security_read == NULL || !cps->security_read(addr, c, len < CHUNK ? len : CHUNK)) {
		refuse();
		return;
	}
	size_t h = reply_header(c, len);
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (rom) {
			board.system->read(cps->rom_base + addr + off, c + h, n);
		} else if (!cps->security_read(addr + off, c + h, n)) {
			memset(c + h, 0xFF, n);
		}
		send(c, h + n);
		h = 0;
	}
	stats.reads++;
}

static void cmd_read(void)
{
	uint8_t area = req[1];
	uint32_t addr = be32(&req[2]);
	uint16_t len = be16(&req[6]);
	const hal_flash_map_t *m = map();

	if (len > CPS_REPLY_MAX) {
		len = CPS_REPLY_MAX;
	}
	switch (area) {
	case 1:		// flash: physical addresses (the STM32 family's tools add the 128 KB offset themselves)
	case 2:		// EEPROM image: the first 64 KB of the open-firmware layout
		if (m == NULL || board.flash == NULL || len == 0) {
			refuse();
			return;
		}
		if (area == 2) {
			if (addr >= EE_IMAGE_SIZE || len > EE_IMAGE_SIZE - addr) {
				refuse();
				return;
			}
			addr += m->openfw_eeprom;
		}
		if (addr >= flash_size() || len > flash_size() - addr) {
			refuse();
			return;
		}
		read_flash(addr, len);
		return;
	case 5:
		read_other(true, addr, len);
		return;
	case 9:
		radio_info();
		return;
	case 10:
		read_other(false, addr, len);
		return;
	default:	// 6 display, 7 WAV, 8 AMBE compress: not offered (open-firmware-cps.md 3)
		refuse();
		return;
	}
}

// --- W / X: write ---

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// A sector the open-firmware CPS may write: inside the EEPROM image or the open-firmware flash image, never one of the
// radio's own areas; the calibration sector with only its window (sector_cal)
static bool writable(uint32_t addr, bool *cal)
{
	const hal_flash_map_t *m = map();
	*cal = false;
	if (m == NULL || !storage_ready() || addr >= flash_size()) {
		return false;
	}
	if (overlaps(addr, STORAGE_SECTOR, m->settings_addr, m->settings_size) ||
	    overlaps(addr, STORAGE_SECTOR, m->journal_addr, 2u * STORAGE_SECTOR) ||
	    overlaps(addr, STORAGE_SECTOR, m->native_addr, m->native_size)) {
		return false;
	}
	if (m->calibration_size != 0 && overlaps(addr, STORAGE_SECTOR, m->calibration_addr, m->calibration_size)) {
		*cal = true;
		return true;
	}
	return (addr >= m->openfw_eeprom && addr < m->openfw_eeprom + EE_IMAGE_SIZE) ||
	       (addr >= m->openfw_flash && addr < m->openfw_end);
}

// Whether a byte of the pending sector may take the CPS's value: all of a codeplug sector (the quick-key slots
// included: the side key assignment, core/input/keyassign.h), only the table in the calibration window
static bool byte_ok(uint32_t a)
{
	const hal_flash_map_t *m = map();
	if (sector_cal) {
		return a >= m->calibration_addr && a < m->calibration_addr + m->calibration_size;
	}
	return true;
}

static void ack(void)
{
	uint8_t r[2] = { req[0], req[1] };
	send(r, 2);
}

static void cmd_write(void)
{
	uint8_t sub = req[1];

	if (req[0] == 'W') {
		// The STM32 family's "fake write": acknowledged, nothing done (open-firmware-cps.md 2)
		if (sub >= 1 && sub <= 4) {
			ack();
		} else {
			refuse();
		}
		return;
	}
	switch (sub) {
	case 1: {
		uint32_t addr = (((uint32_t)req[2] << 16) | ((uint32_t)req[3] << 8) | req[4]) * STORAGE_SECTOR;
		bool cal;
		if (sector != NULL || !writable(addr, &cal)) {
			drop_sector();
			refuse();
			return;
		}
		sector = storage_buffer_acquire();
		if (sector == NULL || !board.flash->read(addr, sector, STORAGE_SECTOR)) {
			drop_sector();
			refuse();
			return;
		}
		sector_addr = addr;
		sector_cal = cal;
		stats.pending = true;
		ack();
		return;
	}
	case 2: {
		uint32_t addr = be32(&req[2]);
		uint32_t len = be16(&req[6]);
		if (sector == NULL) {
			refuse();
			return;
		}
		if (len > req_len - 8u) {
			len = (uint32_t)req_len - 8u;	// clipped to what came
		}
		for (uint32_t i = 0; i < len; i++) {
			uint32_t a = addr + i;
			if (a >= sector_addr && a < sector_addr + STORAGE_SECTOR && byte_ok(a)) {
				sector[a - sector_addr] = req[8 + i];
			}
		}
		ack();
		return;
	}
	case 3: {
		if (sector == NULL) {
			refuse();
			return;
		}
		cps_openfw_vfo_written(sector_addr, sector, last_rx_ms);
		bool ok = storage_sector_write(sector_addr, sector);
		drop_sector();
		if (!ok) {
			refuse();
			return;
		}
		stats.commits++;
		image_written = true;
		ack();
		return;
	}
	case 4:		// EEPROM write: emulated in flash on our radios; the tools write it with 1-3 (2.2)
		ack();
		return;
	default:	// 7 WAV buffer: not offered
		drop_sector();
		refuse();
		return;
	}
}

// --- C: control. Every reply is '-' ("done") ---

static void cmd_control(void)
{
	uint8_t sub = req[1];

	switch (sub) {
	case 5:		// close: the codeplug is re-read if it was written
		end_session();
		break;
	case 6:
		switch (req[2]) {
		case 0:		// save settings and reboot
		case 1:		// reboot
			nak();
			// Only after C 0: otherwise any three bytes "C 6 1" on a link shared with the debug link reboot
			// the radio (bench 2026-10-15)
			if (!programming) {
				stats.reboots_ignored++;
				return;
			}
			programming = false;
			end_session();
			if (req[2] == 0) {
				settings_flush();
			}
			reboot_req = req[2] == 0 ? 1 : 0;	// after this feed: the engine may be re-initialized
			return;
		case 2:		// save the settings and the VFOs (the image shows the live VFO anyway)
			settings_flush();
			break;
		default:	// LEDs, AMBE mode, sound buffers, clock, wait: nothing to do here
			break;
		}
		break;
	case 0:		// programming screen
		programming = true;
		break;
	default:	// 1-4 screen text and backlight, 7 GPS, 254 ping
		break;
	}
	nak();
}

// --- Request assembly ---

// Bytes the request at req[0..req_len) needs: > 0 the total, 0 more header bytes first, -1 ends at the gap
static int needed(void)
{
	if (req_len == 0) {
		return 0;
	}
	switch (req[0]) {
	case 'R':
		return 8;
	case 'W':
	case 'X':
		if (req_len < 2) {
			return 0;
		}
		switch (req[1]) {
		case 1:
			return 5;
		case 3:
			return 2;
		case 2:
		case 4:
		case 7:
			return req_len < 8 ? 0 : 8 + be16(&req[6]);
		default:
			return 2;
		}
	case 'C':
		if (req_len < 2) {
			return 0;
		}
		if (req[1] == 2) {
			// x, y, font, alignment, inverse, then up to 16 characters ending at a NUL
			for (size_t i = 7; i < req_len; i++) {
				if (req[i] == 0) {
					return (int)i + 1;
				}
			}
			return req_len >= 23 ? 23 : -1;
		}
		if (req[1] == 6) {
			if (req_len < 3) {
				return 0;
			}
			return req[2] == 7 ? 7 : 3;
		}
		if (req[1] == 'P') {
			return -1;
		}
		return 2;
	default:	// D (our data service host commands: not on this radio yet), anything else
		return -1;
	}
}

static void handle(size_t n)
{
	stats.requests++;
	stats.last = req[0];
	stats.session = true;
	switch (req[0]) {
	case 'R':
		cmd_read();
		break;
	case 'W':
	case 'X':
		cmd_write();
		break;
	case 'C':
		cmd_control();
		break;
	case 'D': {
		uint8_t r[3] = { 'D', n > 1 ? req[1] : 0, 0 };	// "failed": no data service on this radio yet
		send(r, sizeof(r));
		break;
	}
	default:
		refuse();
		break;
	}
}

// A byte no request starts with. The DM-1801 Pro's cable (on the microphone / speaker jack) now and then delivers a
// burst of noise bytes (0xFC, 0xFE) between requests; answered '-', it was read as the reply to the next request
static bool junk(uint8_t b)
{
	return b != 'R' && b != 'W' && b != 'X' && b != 'C' && b != 'D';
}

// Take complete requests off the front of the buffer, dropping noise in front of one (counted, never answered). After
// the gap, a request whose length the header doesn't give is whatever came; one whose header gives its length (or
// isn't whole yet) waits for the stall, so a UART that delivers it in pieces doesn't cut it short
static void process(bool gap, bool stall)
{
	while (req_len > 0) {
		size_t skip = 0;
		while (skip < req_len && junk(req[skip])) {
			skip++;
		}
		if (skip > 0) {
			stats.junk += skip;
			memmove(req, req + skip, req_len - skip);
			req_len -= skip;
			memset(req + req_len, 0, skip);
			continue;
		}
		int n = needed();
		if (n > CPS_REQUEST_MAX) {
			overflow = true;	// the header asks for more than fits: wait for the gap, then '-'
			return;
		}
		if (n > 0 && req_len >= (size_t)n) {
			handle((size_t)n);
			memmove(req, req + n, req_len - (size_t)n);
			req_len -= (size_t)n;
			memset(req + req_len, 0, (size_t)n);	// clear behind, as a radio on the aftermarket firmware does
		} else if (n < 0 ? gap : stall) {
			handle(req_len);	// gap-terminated, or incomplete for too long: whatever came is the request
			memset(req, 0, req_len);
			req_len = 0;
		} else {
			return;
		}
	}
}

void cps_init(const cps_io_t *p)
{
	io = p;
	req_len = 0;
	overflow = false;
	drop_sector();
	image_written = false;
	reboot_req = -1;
	programming = false;
	memset(&stats, 0, sizeof(stats));
	memset(req, 0, sizeof(req));
}

// A reboot asked for by C 6 0 / 1, once the bytes at hand are handled (the reboot may re-initialize the engine)
static void reboot_now(void)
{
	if (reboot_req >= 0) {
		bool save = reboot_req == 1;
		reboot_req = -1;
		req_len = 0;
		if (io != NULL && io->reboot != NULL) {
			io->reboot(save);
		}
	}
}

void cps_feed(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	bool gap = (now_ms - last_rx_ms) >= CPS_GAP_MS;
	bool stall = (now_ms - last_rx_ms) >= CPS_STALL_MS;

	// After a quiet gap, what waited is a whole request (or an overlong one, answered '-')
	if (gap && overflow) {
		overflow = false;
		req_len = 0;
		memset(req, 0, sizeof(req));
		stats.overflows++;
		refuse();
	} else if (gap && req_len > 0) {
		process(true, stall);
	}
	if (len == 0) {
		// Tick: a long quiet ends the session
		if (stats.session && (now_ms - last_rx_ms) > CPS_SESSION_MS) {
			end_session();
		}
		reboot_now();
		return;
	}
	if ((now_ms - last_rx_ms) > CPS_SESSION_MS) {
		programming = false;
	}
	last_rx_ms = now_ms;
	if (overflow) {
		return;			// discard until the gap
	}
	if (req_len + len > CPS_REQUEST_MAX) {
		overflow = true;
		return;
	}
	memcpy(req + req_len, buf, len);
	req_len += len;
	process(false, false);
	reboot_now();
}

void cps_get_stats(cps_stats_t *s)
{
	*s = stats;
}

bool cps_mid_request(void)
{
	return req_len > 0 || overflow;
}
