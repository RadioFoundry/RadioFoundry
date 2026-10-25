// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TFTP server (tftp.h, docs/spec/tftp.md), written from RFC 1350, 2347, 2348 and 2349. Lock step: one DATA (read)
// or ACK (write) outstanding, resent after the timeout, a duplicate ACK never answered with a DATA (the "Sorcerer's
// Apprentice" rule of RFC 1123 4.2.3.1). Block numbers roll over from 65535 to 0, as most clients expect.
#include <string.h>
#include "net/tftp.h"

#define OP_RRQ		1
#define OP_WRQ		2
#define OP_DATA		3
#define OP_ACK		4
#define OP_ERROR	5
#define OP_OACK		6

#define BLKSIZE_MIN	8
#define PORT_FIRST	49152u		// the transfers' own ports (TIDs) come from here on
#define PORT_COUNT	16000u

enum { LAST_NONE, LAST_OACK, LAST_DATA, LAST_ACK };

static const tftp_io_t *io;
static const tftp_file_t *const *files;
static int nfiles;
static tftp_stats_t stats;
static uint16_t next_port;

static struct {
	uint8_t state;			// tftp_state_t
	uint8_t last;			// LAST_*: the packet to resend
	bool unsent;			// it hasn't gone out yet (busy)
	bool final;			// reading: the last DATA (shorter than blksize) is the one outstanding
	bool opt_blksize, opt_tsize, opt_timeout;	// options to acknowledge
	uint8_t tries;
	const tftp_file_t *f;
	uint32_t peer_ip;
	uint16_t peer_port;
	uint16_t port;			// ours
	uint16_t blksize;
	uint32_t timeout_ms;
	uint32_t block;			// reading: the DATA block outstanding; writing: the last block taken
	uint32_t size;			// reading: the file's size; writing: bytes taken
	uint32_t tsize;			// the tsize option's answer
	uint32_t sent_ms;
} t;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void set16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static bool same_text(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (x != y) {
			return false;
		}
	}
	return *a == *b;
}

// Decimal text to a number; false if not a plain number
static bool parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	if (*s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9' || v > 429496728u) {
			return false;
		}
		v = v * 10u + (uint32_t)(*s - '0');
	}
	*out = v;
	return true;
}

static int put_text(uint8_t *p, const char *s)
{
	int n = (int)strlen(s);
	memcpy(p, s, (size_t)n + 1u);
	return n + 1;
}

static int put_number(uint8_t *p, uint32_t v)
{
	char b[11];
	int n = 0;
	do {
		b[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0);
	for (int i = 0; i < n; i++) {
		p[i] = (uint8_t)b[n - 1 - i];
	}
	p[n] = 0;
	return n + 1;
}

static void send_error(uint32_t ip, uint16_t from, uint16_t to, int code, const char *msg)
{
	int max;
	uint8_t *b = io->buffer(&max);
	int n = (int)strlen(msg);
	if (b == NULL || max < 5) {
		return;
	}
	if (n > max - 5) {
		n = max - 5;
	}
	set16(b, OP_ERROR);
	set16(b + 2, (uint16_t)code);
	memcpy(b + 4, msg, (size_t)n);
	b[4 + n] = 0;
	stats.errors++;
	io->send(ip, from, to, b, 5 + n);	// best effort: an error is never resent
}

static void finish(void)
{
	if (t.state == TFTP_WRITING && t.f != NULL && t.f->abort != NULL) {
		t.f->abort();
	}
	if (t.port != 0) {
		io->bind(t.port, false);
	}
	memset(&t, 0, sizeof(t));
	t.state = TFTP_IDLE;
}

static void fail(int code, const char *msg)
{
	send_error(t.peer_ip, t.port, t.peer_port, code, msg);
	finish();
}

// Build and send the packet t.last says; false while busy (t.unsent stays set)
static bool transmit(uint32_t now)
{
	int max, n = 0;
	uint8_t *b = io->buffer(&max);
	if (b == NULL) {
		return false;
	}
	switch (t.last) {
	case LAST_OACK:
		set16(b, OP_OACK);
		n = 2;
		if (t.opt_blksize) {
			n += put_text(b + n, "blksize");
			n += put_number(b + n, t.blksize);
		}
		if (t.opt_tsize) {
			n += put_text(b + n, "tsize");
			n += put_number(b + n, t.tsize);
		}
		if (t.opt_timeout) {
			n += put_text(b + n, "timeout");
			n += put_number(b + n, t.timeout_ms / 1000u);
		}
		break;
	case LAST_DATA: {
		uint32_t off = (t.block - 1u) * t.blksize;
		uint32_t len = off < t.size ? t.size - off : 0;
		if (len > t.blksize) {
			len = t.blksize;
		}
		set16(b, OP_DATA);
		set16(b + 2, (uint16_t)t.block);
		if (len > 0 && !t.f->read(off, b + 4, (uint16_t)len)) {
			fail(TFTP_ERR_OTHER, "read failed");
			return true;
		}
		t.final = len < t.blksize;
		n = 4 + (int)len;
		break;
	}
	case LAST_ACK:
		set16(b, OP_ACK);
		set16(b + 2, (uint16_t)t.block);
		n = 4;
		break;
	default:
		return true;
	}
	if (!io->send(t.peer_ip, t.port, t.peer_port, b, n)) {
		t.unsent = true;
		return false;
	}
	t.unsent = false;
	t.sent_ms = now;
	return true;
}

static void send_next(uint8_t what, uint32_t now)
{
	t.last = what;
	t.tries = 0;
	transmit(now);
}

// RRQ / WRQ to port 69
static void request(uint32_t ip, uint16_t port, bool write, const uint8_t *p, int length, uint32_t now)
{
	const char *field[16];
	int nf = 0;

	// NUL-terminated fields after the opcode: file name, mode, then option name / value pairs
	for (int i = 2, start = 2; i < length && nf < 16; i++) {
		if (p[i] == 0) {
			field[nf++] = (const char *)&p[start];
			start = i + 1;
		}
	}
	if (nf < 2) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_ILLEGAL, "malformed request");
		return;
	}
	if (t.state != TFTP_IDLE && t.state != TFTP_DALLYING) {
		if (ip == t.peer_ip && port == t.peer_port) {
			return;		// the same request again: our answer is resent by the timer
		}
		send_error(ip, TFTP_PORT, port, TFTP_ERR_OTHER, "busy: one transfer at a time");
		return;
	}
	finish();
	if (!same_text(field[1], "octet")) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_ILLEGAL, "octet mode only");
		return;
	}
	const char *name = field[0];
	while (*name == '/') {
		name++;
	}
	const tftp_file_t *f = NULL;
	for (int i = 0; i < nfiles; i++) {
		if (same_text(files[i]->name, name)) {
			f = files[i];
		}
	}
	if (f == NULL) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_NOT_FOUND, "no such file");
		return;
	}
	if ((write && f->open_write == NULL) || (!write && f->read == NULL)) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_ACCESS, write ? "read only" : "write only");
		return;
	}

	int max;
	io->buffer(&max);
	uint32_t blksize = TFTP_BLKSIZE, timeout_s = 0, tsize = 0;
	bool opt_blksize = false, opt_tsize = false, opt_timeout = false;
	for (int i = 2; i + 1 < nf; i += 2) {
		uint32_t v;
		if (!parse_u32(field[i + 1], &v)) {
			continue;		// a malformed value: the option is ignored (RFC 2347)
		}
		if (same_text(field[i], "blksize") && v >= BLKSIZE_MIN && v <= (uint32_t)(max - 4)) {
			// A bigger one than a frame holds is ignored (512 then), not answered with less: RFC 2348 allows
			// that, but macOS tftp(1) keeps its own size and takes the first block for the last
			blksize = v;
			opt_blksize = true;
		} else if (same_text(field[i], "tsize")) {
			tsize = v;
			opt_tsize = true;
		} else if (same_text(field[i], "timeout") && v >= 1 && v <= 255) {
			timeout_s = v;
			opt_timeout = true;
		}
	}
	if (blksize > (uint32_t)(max - 4) || max < 4 + BLKSIZE_MIN) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_OTHER, "no buffer");
		return;
	}

	uint32_t size = 0;
	if (write) {
		const char *msg = NULL;
		int r = f->open_write(opt_tsize ? tsize : 0, &msg);
		if (r != TFTP_OK) {
			send_error(ip, TFTP_PORT, port, r & 0xFF, msg != NULL ? msg : "refused");
			return;
		}
	} else if (f->open_read == NULL || !f->open_read(&size)) {
		send_error(ip, TFTP_PORT, port, TFTP_ERR_NOT_FOUND, "not available");
		return;
	}

	t.f = f;
	t.peer_ip = ip;
	t.peer_port = port;
	t.port = (uint16_t)(PORT_FIRST + next_port);
	next_port = (uint16_t)((next_port + 1u) % PORT_COUNT);
	t.blksize = (uint16_t)blksize;
	t.timeout_ms = opt_timeout ? timeout_s * 1000u : TFTP_TIMEOUT_MS;
	t.opt_blksize = opt_blksize;
	t.opt_tsize = opt_tsize;
	t.opt_timeout = opt_timeout;
	t.state = write ? TFTP_WRITING : TFTP_READING;
	t.size = size;		// writing: 0, bytes taken so far
	t.tsize = write ? tsize : size;
	t.block = 0;
	stats.blksize = t.blksize;
	io->bind(t.port, true);
	if (opt_blksize || opt_tsize || opt_timeout) {
		send_next(LAST_OACK, now);
	} else if (write) {
		send_next(LAST_ACK, now);
	} else {
		t.block = 1;
		send_next(LAST_DATA, now);
	}
}

void tftpReceive(uint32_t srcIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *p, int length, uint32_t now)
{
	if (io == NULL || length < 2) {
		return;
	}
	uint16_t op = get16(p);
	if (dstPort == TFTP_PORT) {
		if (op == OP_RRQ || op == OP_WRQ) {
			request(srcIp, srcPort, op == OP_WRQ, p, length, now);
		} else if (op != OP_ERROR) {
			send_error(srcIp, TFTP_PORT, srcPort, TFTP_ERR_ILLEGAL, "expected a request");
		}
		return;
	}
	if (t.state == TFTP_IDLE || dstPort != t.port) {
		return;
	}
	if (srcIp != t.peer_ip || srcPort != t.peer_port) {
		send_error(srcIp, t.port, srcPort, TFTP_ERR_TID, "unknown transfer ID");	// the transfer goes on
		return;
	}
	if (op == OP_ERROR) {
		if (t.state != TFTP_COMMITTING) {
			finish();	// the client gave up (a commit under way finishes: the data is all there)
		}
		return;
	}
	uint16_t n = length >= 4 ? get16(p + 2) : 0;
	switch (t.state) {
	case TFTP_READING:
		if (op != OP_ACK || length < 4) {
			fail(TFTP_ERR_ILLEGAL, "expected an ACK");
		} else if (t.last == LAST_OACK && n == 0) {
			t.block = 1;
			send_next(LAST_DATA, now);
		} else if (t.last == LAST_DATA && n == (uint16_t)t.block) {
			if (t.final) {
				stats.reads++;
				stats.last_size = t.size;
				finish();
			} else {
				t.block++;
				send_next(LAST_DATA, now);
			}
		}
		// an older block's ACK again: ignored, never answered with a DATA
		break;
	case TFTP_WRITING:
	case TFTP_DALLYING:
		if (op != OP_DATA || length < 4) {
			fail(TFTP_ERR_ILLEGAL, "expected DATA");
		} else if (n == (uint16_t)t.block && (t.block != 0 || t.state == TFTP_DALLYING)) {
			transmit(now);		// our ACK was lost: once more
		} else if (t.state == TFTP_WRITING && n == (uint16_t)(t.block + 1u)) {
			int len = length - 4;
			const char *msg = NULL;
			if (len > t.blksize) {
				fail(TFTP_ERR_ILLEGAL, "block longer than blksize");
				return;
			}
			int r = len > 0 ? t.f->write(t.size, p + 4, (uint16_t)len, &msg) : TFTP_OK;
			if (r != TFTP_OK) {
				fail(r & 0xFF, msg != NULL ? msg : "write failed");
				return;
			}
			t.block++;
			t.size += (uint32_t)len;
			t.last = LAST_ACK;
			t.tries = 0;
			if (len < t.blksize) {
				t.state = TFTP_COMMITTING;	// the last ACK waits for the commit, from the next tftpTick (not
				t.sent_ms = now;		// here, deep in the receive path's stack)
			} else {
				transmit(now);
			}
		}
		break;
	default:		// committing: resent DATA waits for the commit's ACK
		break;
	}
}

void tftpTick(uint32_t now)
{
	if (io == NULL || t.state == TFTP_IDLE) {
		stats.state = TFTP_IDLE;
		return;
	}
	if (t.state == TFTP_COMMITTING) {
		const char *msg = NULL;
		int r = t.f->commit(t.size, &msg);
		if (r == TFTP_PENDING) {
			stats.state = t.state;
			return;
		}
		if (r != TFTP_OK) {
			t.state = TFTP_IDLE;	// the file callbacks are done with it: no abort
			fail(r & 0xFF, msg != NULL ? msg : "not stored");
			return;
		}
		stats.writes++;
		stats.last_size = t.size;
		t.state = TFTP_DALLYING;
		t.sent_ms = now;
		transmit(now);
		stats.state = t.state;
		return;
	}
	if (t.unsent) {
		transmit(now);
	} else if (t.state == TFTP_DALLYING) {
		if (now - t.sent_ms >= TFTP_DALLY_MS) {
			finish();
		}
	} else if (now - t.sent_ms >= t.timeout_ms) {
		if (++t.tries > TFTP_RETRIES) {
			stats.timeouts++;
			fail(TFTP_ERR_OTHER, "timed out");
			return;
		}
		stats.retransmits++;
		transmit(now);
	}
	stats.state = t.state;
}

void tftpInit(const tftp_io_t *i, const tftp_file_t *const *f, int count)
{
	io = i;
	files = f;
	nfiles = count;
	memset(&stats, 0, sizeof(stats));
	memset(&t, 0, sizeof(t));
	next_port = 0;
}

void tftpStats(tftp_stats_t *s)
{
	*s = stats;
	s->state = t.state;
}
