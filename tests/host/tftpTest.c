// SPDX-License-Identifier: MIT Copyright (c) 2026 The OpenDigitalRadio Authors The TFTP server (core/net/tftp.c) behind
// the real IP gateway, driven by a small client in this file: reads with and without options, blksize negotiation and
// clamping, tsize, the empty last block, block number roll-over, timeouts and retransmits, duplicate ACKs, foreign
// TIDs, busy, refusals, writes with a slow commit, the dally, a client's error. Then codeplug.bin
// (core/net/tftp_codeplug.c) on a simulated flash with an open-firmware codeplug: the image read, written back edited
// (the quick-key slots written too, the codeplug re-read), refused writes leaving the flash untouched, and power cuts all through a
// commit, finished by tftp_codeplug_resume. The short transfers go to tftpTest.pcap for tshark.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/ipGateway.h"
#include "net/tftp.h"
#include "net/tftp_codeplug.h"
#include "codeplug/codeplug.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#include "fuzz.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define RADIO_ID	10005u
#define HOST_IP		(IPGW_INDIVIDUAL_NET | RADIO_ID)
#define RADIO_IP	(HOST_IP ^ 1u)
#define CPORT		50000		// the client's port

static const uint8_t HOST_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x04 };
static const uint8_t RADIO_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x05 };

static FILE *pcap;
static bool capture = true;
static uint32_t now = 1000;

// ---- the gateway's user ----
#define RING	8
static uint8_t ring[RING][700];
static int ring_len[RING], ring_in, ring_out;
static int sent_frames;

bool ipGatewaySendFrame(const uint8_t *frame, int length)
{
	if (capture) {
		uint32_t rec[4] = { now / 1000u, (now % 1000u) * 1000u, (uint32_t)length, (uint32_t)length };
		fwrite(rec, sizeof(rec), 1, pcap);
		fwrite(frame, (size_t)length, 1, pcap);
	}
	memcpy(ring[ring_in], frame, (size_t)length);
	ring_len[ring_in] = length;
	ring_in = (ring_in + 1) % RING;
	sent_frames++;
	return true;
}
uint32_t ipGatewayRadioId(void) { return RADIO_ID; }
void ipGatewaySerialIn(const uint8_t *data, int length) { (void)data; (void)length; }
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length) { return true; }
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length) { return true; }

// ---- the server's io ----
static void udp_in(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	tftpReceive(src, sport, dport, data, length, now);
}
static uint8_t *io_buffer(int *max) { *max = IPGW_UDP_PAYLOAD_MAX; return ipGatewayUdpPayload(); }
static bool io_send(uint32_t ip, uint16_t sport, uint16_t dport, const uint8_t *p, int n) { return ipGatewayUdpSend(ip, sport, dport, p, n); }
static void io_bind(uint16_t port, bool on) { ipGatewayUdpBind(port, on ? udp_in : NULL); }
static const tftp_io_t io = { io_buffer, io_send, io_bind };

// ---- files ----
static uint32_t ram_size = 1300;
static uint8_t pattern(uint32_t off) { return (uint8_t)(off * 7u + (off >> 8)); }
static bool ram_open(uint32_t *size) { *size = ram_size; return true; }
static bool ram_read(uint32_t off, uint8_t *buf, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		buf[i] = pattern(off + i);
	}
	return true;
}

static uint8_t wbuf[8192];
static uint32_t wlen;
static int commits_pending, commit_result, aborts, open_result = TFTP_OK;
static int w_open(uint32_t tsize, const char **msg)
{
	if (open_result != TFTP_OK) {
		*msg = "too big";
		return open_result;
	}
	wlen = 0;
	return TFTP_OK;
}
static int w_write(uint32_t off, const uint8_t *buf, uint16_t len, const char **msg)
{
	if (off + len > sizeof(wbuf)) {
		return TFTP_FAIL(TFTP_ERR_FULL);
	}
	memcpy(wbuf + off, buf, len);
	wlen = off + len;
	return TFTP_OK;
}
static int w_commit(uint32_t size, const char **msg)
{
	if (commits_pending > 0) {
		commits_pending--;
		return TFTP_PENDING;
	}
	if (commit_result != TFTP_OK) {
		*msg = "bad image";
	}
	return commit_result;
}
static void w_abort(void) { aborts++; }

static const tftp_file_t ram_file = { .name = "ram.bin", .open_read = ram_open, .read = ram_read };
static const tftp_file_t wr_file = { .name = "wr.bin", .open_write = w_open, .write = w_write, .commit = w_commit,
				     .abort = w_abort };
static const tftp_file_t *const files[] = { &ram_file, &wr_file, &tftp_codeplug_file };

// ---- the client ----
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint16_t csum(const uint8_t *p, int len, uint32_t sum)
{
	for (; len > 1; p += 2, len -= 2) {
		sum += get16(p);
	}
	if (len) {
		sum += (uint32_t)p[0] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static void send_udp(uint16_t sport, uint16_t dport, const uint8_t *data, int len)
{
	uint8_t f[700];
	memset(f, 0, 42);
	memcpy(f, RADIO_MAC, 6);
	memcpy(f + 6, HOST_MAC, 6);
	put16(f + 12, 0x0800);
	uint8_t *ip = f + 14, *udp = f + 34;
	ip[0] = 0x45;
	put16(ip + 2, (uint16_t)(28 + len));
	ip[8] = 64;
	ip[9] = 17;
	put32(ip + 12, HOST_IP);
	put32(ip + 16, RADIO_IP);
	put16(ip + 10, csum(ip, 20, 0));
	put16(udp, sport);
	put16(udp + 2, dport);
	put16(udp + 4, (uint16_t)(8 + len));
	memcpy(udp + 8, data, (size_t)len);
	if (capture) {
		uint32_t rec[4] = { now / 1000u, (now % 1000u) * 1000u, (uint32_t)(42 + len), (uint32_t)(42 + len) };
		fwrite(rec, sizeof(rec), 1, pcap);
		fwrite(f, (size_t)(42 + len), 1, pcap);
	}
	ipGatewayEthernetIn(f, 42 + len);
}

// A packet from the server: its payload, source port; false if none waits
typedef struct {
	uint8_t p[600];
	int len;
	uint16_t sport, dport;
	uint8_t ttl;
} pkt_t;

static bool next_pkt(pkt_t *k)
{
	if (ring_out == ring_in) {
		return false;
	}
	const uint8_t *f = ring[ring_out];
	k->sport = get16(f + 34);
	k->dport = get16(f + 36);
	k->ttl = f[22];
	k->len = ring_len[ring_out] - 42;
	memcpy(k->p, f + 42, (size_t)k->len);
	ring_out = (ring_out + 1) % RING;
	return true;
}

static void drain(void)
{
	ring_out = ring_in;
}

static void tick(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		tftpTick(now);
	}
}

static int request(uint16_t op, uint16_t cport, const char *name, const char *mode, const char *const *opts)
{
	uint8_t b[256];
	int n = 2;
	put16(b, op);
	n += sprintf((char *)b + n, "%s", name) + 1;
	n += sprintf((char *)b + n, "%s", mode) + 1;
	for (int i = 0; opts != NULL && opts[i] != NULL; i++) {
		n += sprintf((char *)b + n, "%s", opts[i]) + 1;
	}
	send_udp(cport, TFTP_PORT, b, n);
	return n;
}

static void send_ack(uint16_t tid, uint16_t block)
{
	uint8_t b[4];
	put16(b, 4);
	put16(b + 2, block);
	send_udp(CPORT, tid, b, 4);
}

static void send_data(uint16_t tid, uint16_t block, const uint8_t *data, int len)
{
	uint8_t b[600];
	put16(b, 3);
	put16(b + 2, block);
	memcpy(b + 4, data, (size_t)len);
	send_udp(CPORT, tid, b, 4 + len);
}

// The value of an option in an OACK, -1 if absent
static long oack_value(const pkt_t *k, const char *name)
{
	for (int i = 2; i < k->len;) {
		const char *o = (const char *)k->p + i;
		const char *v = o + strlen(o) + 1;
		if (strcmp(o, name) == 0) {
			return atol(v);
		}
		i = (int)(v + strlen(v) + 1 - (const char *)k->p);
	}
	return -1;
}

// A whole read: the bytes into dst; the result: bytes, or -(error code + 1)
static long get(const char *name, const char *const *opts, uint8_t *dst, long max, int blksize)
{
	pkt_t k;
	long got = 0;
	uint16_t expect = 1;
	drain();
	request(1, CPORT, name, "octet", opts);
	for (int idle = 0; idle < 2000;) {
		if (!next_pkt(&k)) {
			tick(5);
			idle++;
			continue;
		}
		idle = 0;
		uint16_t op = get16(k.p);
		if (op == 5) {
			return -(long)get16(k.p + 2) - 1;
		}
		if (op == 6) {
			if (oack_value(&k, "blksize") > 0) {
				blksize = (int)oack_value(&k, "blksize");
			}
			send_ack(k.sport, 0);
		} else if (op == 3) {
			int n = k.len - 4;
			if (get16(k.p + 2) == expect) {
				if (got + n <= max) {
					memcpy(dst + got, k.p + 4, (size_t)n);
				}
				got += n;
				expect++;
			}
			send_ack(k.sport, get16(k.p + 2));
			if (n < blksize) {
				return got;
			}
		}
	}
	return -100;
}

// A whole write of src; the result 0 or -(error code + 1)
static long put(const char *name, const char *const *opts, const uint8_t *src, long len, int blksize)
{
	pkt_t k;
	uint16_t block = 0;
	long off = 0;
	bool last_sent = false;
	drain();
	request(2, CPORT, name, "octet", opts);
	for (int idle = 0; idle < 2000;) {
		if (!next_pkt(&k)) {
			tick(5);
			idle++;
			continue;
		}
		idle = 0;
		uint16_t op = get16(k.p);
		if (op == 5) {
			return -(long)get16(k.p + 2) - 1;
		}
		if (op == 6 && oack_value(&k, "blksize") > 0) {
			blksize = (int)oack_value(&k, "blksize");
		}
		if ((op == 4 && get16(k.p + 2) == block) || (op == 6 && block == 0)) {
			if (last_sent) {
				return 0;
			}
			int n = len - off < blksize ? (int)(len - off) : blksize;
			block++;
			send_data(k.sport, block, src + off, n);
			off += n;
			last_sent = n < blksize;
		}
	}
	return -100;
}

static void test_reads(void)
{
	static uint8_t buf[70000];
	pkt_t k;
	tftp_stats_t st;

	// No options: 512-byte blocks from a port of its own, TTL 255
	drain();
	request(1, CPORT, "ram.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 3 && get16(k.p + 2) == 1 && k.len == 516);
	CHECK(k.sport >= 49152 && k.dport == CPORT && k.ttl == 255);
	uint16_t tid = k.sport;
	send_ack(tid, 1);
	CHECK(next_pkt(&k) && get16(k.p + 2) == 2 && k.len == 516);
	send_ack(tid, 1);			// a duplicate ACK: never answered with a DATA
	CHECK(!next_pkt(&k));
	send_ack(tid, 2);
	CHECK(next_pkt(&k) && get16(k.p + 2) == 3 && k.len == 4 + 276);
	CHECK(k.p[4] == pattern(1024) && k.p[4 + 275] == pattern(1299));
	send_ack(tid, 3);
	tftpStats(&st);
	CHECK(st.reads == 1 && st.state == TFTP_IDLE && st.last_size == 1300);

	// A size that is a multiple of the block size ends with an empty block
	ram_size = 1024;
	CHECK(get("ram.bin", NULL, buf, sizeof(buf), 512) == 1024);
	tftpStats(&st);
	CHECK(st.reads == 2);

	// Options: a blksize bigger than a frame holds is ignored (512), tsize answered, timeout echoed
	ram_size = 1300;
	const char *opts[] = { "blksize", "1428", "tsize", "0", "timeout", "2", "foo", "bar", NULL };
	drain();
	request(1, CPORT, "RAM.BIN", "OCTET", opts);	// names and modes are case insensitive
	CHECK(next_pkt(&k) && get16(k.p) == 6);
	CHECK(oack_value(&k, "blksize") == -1 && oack_value(&k, "tsize") == 1300);
	CHECK(oack_value(&k, "timeout") == 2 && oack_value(&k, "foo") == -1);
	send_ack(k.sport, 0);
	CHECK(next_pkt(&k) && get16(k.p) == 3 && get16(k.p + 2) == 1 && k.len == 4 + 512);
	// the client's timeout: resent after 2 s, not 1 s
	tick(1500);
	CHECK(!next_pkt(&k));
	tick(600);
	CHECK(next_pkt(&k) && get16(k.p + 2) == 1);
	tick(10000);				// no client any more: abandoned after the retries, with an error
	tftpStats(&st);
	CHECK(st.timeouts == 1 && st.state == TFTP_IDLE && st.retransmits >= TFTP_RETRIES);
	drain();

	// The biggest blksize a frame holds is taken
	const char *most[] = { "blksize", "554", NULL };
	CHECK(get("ram.bin", most, buf, sizeof(buf), 554) == 1300 && buf[1299] == pattern(1299));
	tftpStats(&st);
	CHECK(st.blksize == IPGW_UDP_PAYLOAD_MAX - 4);

	// A small blksize, and block numbers rolling over past 65535
	const char *small[] = { "blksize", "8", NULL };
	ram_size = 100;
	long n = get("ram.bin", small, buf, sizeof(buf), 8);
	CHECK(n == 100 && buf[99] == pattern(99));
	capture = false;
	ram_size = 65537u * 8u + 3u;
	static uint8_t *big;
	big = malloc(ram_size);
	n = get("ram.bin", small, big, ram_size, 8);
	CHECK(n == (long)ram_size && big[ram_size - 1] == pattern(ram_size - 1) && big[65536 * 8] == pattern(65536 * 8));
	free(big);
	capture = true;
	ram_size = 1300;
}

static void test_refusals(void)
{
	pkt_t k;
	drain();
	request(1, CPORT, "nothere.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_NOT_FOUND && k.sport == TFTP_PORT);
	request(1, CPORT, "ram.bin", "netascii", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_ILLEGAL);
	request(2, CPORT, "ram.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_ACCESS);
	request(1, CPORT, "wr.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_ACCESS);
	uint8_t junk[3] = { 0, 9, 0 };
	capture = false;			// malformed on purpose: kept out of the capture tshark checks
	send_udp(CPORT, TFTP_PORT, junk, 3);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_ILLEGAL);
	uint8_t noname[2] = { 0, 1 };
	send_udp(CPORT, TFTP_PORT, noname, 2);
	capture = true;
	CHECK(next_pkt(&k) && get16(k.p) == 5);

	// Busy: a second client while a transfer runs; a foreign TID inside the transfer
	request(1, CPORT, "ram.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 3);
	uint16_t tid = k.sport;
	request(1, CPORT + 1, "ram.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_OTHER && k.dport == CPORT + 1);
	request(1, CPORT, "ram.bin", "octet", NULL);	// the same request again: nothing new
	CHECK(!next_pkt(&k));
	uint8_t ack[4] = { 0, 4, 0, 1 };
	send_udp(CPORT + 2, tid, ack, 4);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_TID && k.dport == CPORT + 2);
	send_ack(tid, 1);			// the transfer goes on
	CHECK(next_pkt(&k) && get16(k.p) == 3 && get16(k.p + 2) == 2);
	uint8_t err[5] = { 0, 5, 0, 0, 0 };	// the client gives up
	send_udp(CPORT, tid, err, 5);
	tftp_stats_t st;
	tftpStats(&st);
	CHECK(st.state == TFTP_IDLE);
	send_ack(tid, 2);			// the port is closed now: nothing
	CHECK(!next_pkt(&k));
}

static void test_writes(void)
{
	pkt_t k;
	tftp_stats_t st;
	uint8_t src[1500];
	for (int i = 0; i < (int)sizeof(src); i++) {
		src[i] = (uint8_t)(i ^ 0x5A);
	}
	// Without options: ACK 0, then a commit that takes three ticks before the last ACK
	commits_pending = 3;
	commit_result = TFTP_OK;
	drain();
	request(2, CPORT, "wr.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 0);
	uint16_t tid = k.sport;
	send_data(tid, 1, src, 512);
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 1);
	send_data(tid, 1, src, 512);		// our ACK was lost: once more, nothing written twice
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 1);
	send_data(tid, 2, src + 512, 100);
	CHECK(!next_pkt(&k));			// committing
	send_data(tid, 2, src + 512, 100);	// resent while committing: waits
	tick(10);
	CHECK(!next_pkt(&k));
	tick(10);
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 2);
	CHECK(wlen == 612 && memcmp(wbuf, src, 612) == 0);
	tftpStats(&st);
	CHECK(st.writes == 1 && st.state == TFTP_DALLYING);
	send_data(tid, 2, src + 512, 100);	// the last ACK lost: ACKed again while dallying
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 2);
	tick(TFTP_DALLY_MS + 10);
	tftpStats(&st);
	CHECK(st.state == TFTP_IDLE && aborts == 0);

	// Options, and a commit that refuses: an error instead of the last ACK
	const char *opts[] = { "blksize", "1024", "tsize", "1500", NULL };
	commit_result = TFTP_FAIL(TFTP_ERR_OTHER);
	CHECK(put("wr.bin", opts, src, 1500, 512) == -(TFTP_ERR_OTHER + 1));
	CHECK(wlen == 1500 && memcmp(wbuf, src, 1500) == 0 && aborts == 0);
	commit_result = TFTP_OK;
	CHECK(put("wr.bin", opts, src, 1500, 512) == 0);

	// open_write refuses (the announced size)
	open_result = TFTP_FAIL(TFTP_ERR_FULL);
	CHECK(put("wr.bin", opts, src, 1500, 512) == -(TFTP_ERR_FULL + 1));
	open_result = TFTP_OK;

	// A block longer than blksize, and the client giving up: aborted
	drain();
	request(2, CPORT, "wr.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 4);
	tid = k.sport;
	send_data(tid, 1, src, 512);
	CHECK(next_pkt(&k));
	uint8_t err[5] = { 0, 5, 0, 0, 0 };
	send_udp(CPORT, tid, err, 5);
	CHECK(aborts == 1);
	request(2, CPORT, "wr.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 4);
	tid = k.sport;
	uint8_t longer[520];
	memset(longer, 0, sizeof(longer));
	send_data(tid, 1, longer, 516);
	CHECK(next_pkt(&k) && get16(k.p) == 5 && get16(k.p + 2) == TFTP_ERR_ILLEGAL && aborts == 2);
	// No DATA at all: the ACK 0 is resent, then the transfer is abandoned and aborted
	request(2, CPORT, "wr.bin", "octet", NULL);
	CHECK(next_pkt(&k) && get16(k.p) == 4);
	tick(1100);
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 0);
	tick(10000);
	CHECK(aborts == 3);
	drain();
}

// ---- codeplug.bin on a simulated flash ----

#define FLASH_SIZE	0x400000u
#define EE		0x000000u
#define FL		0x020000u
#define STAGING		0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = 0x1C0000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x1C0000u, .native_size = 0x40000u,
	.staging_addr = STAGING, .staging_size = 0x40000u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static const struct { uint32_t start, end; } regions[] = {
	{ 0x00080u, 0x06060u }, { 0x07500u, 0x0B000u }, { 0x20000u, 0x211A0u }, { 0x9B000u, 0xAEE60u },
};

static void populate(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	for (uint16_t n = 1; n <= 300; n += 7) {
		channel_t c;
		memset(&c, 0, sizeof(c));
		snprintf(c.name, sizeof(c.name), "Ch %u", n);
		c.rx_hz = c.tx_hz = 145000000u + n * 12500u;
		c.power = CODEPLUG_POWER_DEFAULT;
		c.timeslot = 1;
		CHECK(codeplug_set_channel(n, &c));
	}
	CHECK(codeplug_set_dmr_id(3100001));
	for (unsigned i = 0; i < 20; i++) {
		sim_flash_data[EE + 0x7524 + i] = (uint8_t)(0x40 + i);	// the quick-key slots as the radio had them
	}
	memset(sim_flash_data + 0x40000, 0x77, 0x1000);		// not in any region (the DMR ID database)
	codeplug_init();
}

// The file as the stored flash says it should be: the regions, 0xFF elsewhere
static void expected_image(uint8_t *img)
{
	memset(img, 0xFF, TFTP_CODEPLUG_SIZE);
	for (unsigned i = 0; i < 4; i++) {
		for (uint32_t a = regions[i].start; a < regions[i].end; a++) {
			img[a] = sim_flash_data[a < 0x10000u ? EE + a : FL + (a - 0x20000u)];
		}
	}
}

static bool regions_equal(const uint8_t *img)
{
	for (unsigned i = 0; i < 4; i++) {
		for (uint32_t a = regions[i].start; a < regions[i].end; a++) {
			if (img[a] != sim_flash_data[a < 0x10000u ? EE + a : FL + (a - 0x20000u)]) {
				return false;
			}
		}
	}
	return true;
}

// Rename channel n in an image (record bank 0 at 0x3790, banks 1-7 at 0x9B1C0)
static void rename_channel(uint8_t *img, unsigned n, const char *name)
{
	unsigned i = n - 1u;
	uint32_t rec = (i < 128 ? 0x3790u : 0x9B1C0u + (i / 128u - 1u) * (16u + 128u * 56u)) + (i % 128u) * 56u;
	memset(img + rec, 0xFF, 16);
	memcpy(img + rec, name, strlen(name));
}

static void test_codeplug(void)
{
	static uint8_t img[TFTP_CODEPLUG_FULL], want[TFTP_CODEPLUG_FULL], before[FLASH_SIZE];
	const char *big[] = { "blksize", "1428", "tsize", "0", NULL };
	channel_t ch;
	capture = false;
	populate();

	// Read: the regions as stored, 0xFF elsewhere
	expected_image(want);
	CHECK(get("codeplug.bin", big, img, sizeof(img), 512) == (long)TFTP_CODEPLUG_SIZE);
	CHECK(memcmp(img, want, TFTP_CODEPLUG_SIZE) == 0);
	CHECK(img[0x40000] == 0xFF && img[0x10] == 0xFF);

	// Write it back with channel 8 renamed and other quick-key slots: both land (the side key assignment lives
	// there)
	uint32_t serial = codeplug_serial();
	rename_channel(img, 8, "Renamed");
	rename_channel(img, 260, "Far one");
	memset(img + 0x7524, 0x11, 20);
	char tsize[16];
	snprintf(tsize, sizeof(tsize), "%u", TFTP_CODEPLUG_SIZE);
	const char *wopts[] = { "blksize", "1428", "tsize", tsize, NULL };
	CHECK(put("codeplug.bin", wopts, img, TFTP_CODEPLUG_SIZE, 512) == 0);
	CHECK(regions_equal(img));
	CHECK(sim_flash_data[EE + 0x7524] == 0x11 && sim_flash_data[EE + 0x7537] == 0x11);
	CHECK(codeplug_serial() > serial);
	CHECK(codeplug_channel(8, &ch) && strcmp(ch.name, "Renamed") == 0);
	CHECK(codeplug_channel(260, &ch) && strcmp(ch.name, "Far one") == 0);
	CHECK(sim_flash_data[0x40000] == 0x77);			// outside the regions: untouched
	CHECK(sim_flash_data[STAGING] == 0xFF);			// the commit record is gone
	CHECK(sim_flash_violations() == 0);

	// A whole cps_client image (1179648 bytes) is taken too, without tsize, at the default block size
	memset(img + TFTP_CODEPLUG_SIZE, 0xFF, TFTP_CODEPLUG_FULL - TFTP_CODEPLUG_SIZE);
	rename_channel(img, 1, "First");
	CHECK(put("codeplug.bin", NULL, img, TFTP_CODEPLUG_FULL, 512) == 0);
	CHECK(codeplug_channel(1, &ch) && strcmp(ch.name, "First") == 0);

	// Refusals leave the flash as it was
	memcpy(before, sim_flash_data, FLASH_SIZE);
	const char *badsize[] = { "tsize", "1000", NULL };
	CHECK(put("codeplug.bin", badsize, img, 1000, 512) == -(TFTP_ERR_FULL + 1));	// refused up front
	CHECK(put("codeplug.bin", NULL, img, 100000, 512) == -(TFTP_ERR_OTHER + 1));	// short: refused at the end
	static uint8_t junk[TFTP_CODEPLUG_SIZE];
	memset(junk, 0x00, sizeof(junk));					// BCD zeros, no channel
	CHECK(put("codeplug.bin", NULL, junk, sizeof(junk), 512) == -(TFTP_ERR_OTHER + 1));
	memset(junk, 0xA5, sizeof(junk));					// not BCD
	CHECK(put("codeplug.bin", NULL, junk, sizeof(junk), 512) == -(TFTP_ERR_OTHER + 1));
	tftp_codeplug_allow_write(false);
	CHECK(put("codeplug.bin", NULL, img, TFTP_CODEPLUG_SIZE, 512) == -(TFTP_ERR_ACCESS + 1));
	tftp_codeplug_allow_write(true);
	// a transfer that stops half way: abandoned, nothing stored
	drain();
	request(2, CPORT, "codeplug.bin", "octet", NULL);
	pkt_t k;
	CHECK(next_pkt(&k) && get16(k.p) == 4);
	for (uint16_t b = 1; b <= 200; b++) {
		send_data(k.sport, b, img + (b - 1u) * 512u, 512);
	}
	tick(10000);
	drain();
	CHECK(memcmp(before, sim_flash_data, STAGING) == 0);	// staging aside, every byte as it was
	CHECK(!tftp_codeplug_resume());				// and nothing to finish at boot
	CHECK(memcmp(before, sim_flash_data, STAGING) == 0);
	capture = true;
}

// Power cuts at points all through a commit: after the restart and the resume, the stored codeplug is either the old
// one (cut before the commit record) or the new one, never a mix
static void test_power_cuts(void)
{
	static uint8_t img[TFTP_CODEPLUG_SIZE], old[TFTP_CODEPLUG_SIZE];
	capture = false;
	int olds = 0, news = 0, cuts = 0;
	static const uint32_t early[] = { 1, 10, 19, 20, 21, 40, 2000, 4100, 8300 };
	for (unsigned it = 0; it < 30; it++) {
		uint32_t cut = it < 9 ? early[it] : 2000u * (it - 8u);
		populate();
		expected_image(old);
		memcpy(img, old, sizeof(img));
		static const unsigned touched[] = { 1, 8, 50, 99, 134, 190, 253, 295 };	// eight sectors
		for (unsigned i = 0; i < 8; i++) {
			rename_channel(img, touched[i], "Cut test");
		}
		// everything but the last block, then the cut armed, then the last block (which starts the commit)
		drain();
		request(2, CPORT, "codeplug.bin", "octet", NULL);
		pkt_t k;
		CHECK(next_pkt(&k) && get16(k.p) == 4);
		uint16_t tid = k.sport;
		uint32_t blocks = TFTP_CODEPLUG_SIZE / 512u + 1u;
		for (uint32_t b = 1; b < blocks; b++) {
			send_data(tid, (uint16_t)b, img + (b - 1u) * 512u, 512);
			drain();
		}
		sim_flash_cut_after(cut);
		send_data(tid, (uint16_t)blocks, img + (blocks - 1u) * 512u, (int)(TFTP_CODEPLUG_SIZE % 512u));
		tick(3000);
		drain();
		bool was_cut = sim_flash_is_cut();
		sim_flash_cut_after(0);
		// power on again
		sim_flash_power_on();
		tftpInit(&io, files, 3);
		CHECK(storage_init());
		bool resumed = tftp_codeplug_resume();
		codeplug_init();
		bool is_new = regions_equal(img), is_old = regions_equal(old);
		CHECK(is_new || is_old);
		CHECK(!was_cut || resumed || is_old);
		CHECK(sim_flash_data[STAGING] == 0xFF || !was_cut);
		olds += is_old && !is_new;
		news += is_new;
		cuts += was_cut;
	}
	CHECK(olds > 0 && news > 0 && cuts >= 25);
	printf("# %d power cuts through a commit: %d left the old codeplug, %d the new one\n", cuts, olds, news);
	capture = true;
}

// codeplug.bin writes at the edges: a size between the image and the full cps_client image, more than the full image,
// duplicate and out-of-order DATA in the middle of a write, the smallest blksize (block numbers roll over from 65535
// to 0 inside the image). Refused ones leave every stored byte as it was.
static void test_codeplug_edges(void)
{
	static uint8_t img[TFTP_CODEPLUG_FULL + 1024], before[FLASH_SIZE];
	channel_t ch;
	capture = false;
	populate();
	expected_image(img);
	memset(img + TFTP_CODEPLUG_SIZE, 0xFF, sizeof(img) - TFTP_CODEPLUG_SIZE);
	memcpy(before, sim_flash_data, FLASH_SIZE);

	// sizes: one block past the image (no tsize: refused at the end), past the full image (refused on the way)
	CHECK(put("codeplug.bin", NULL, img, TFTP_CODEPLUG_SIZE + 512, 512) == -(TFTP_ERR_OTHER + 1));
	CHECK(memcmp(before, sim_flash_data, STAGING) == 0);
	CHECK(put("codeplug.bin", NULL, img, TFTP_CODEPLUG_FULL + 1024, 512) == -(TFTP_ERR_FULL + 1));
	CHECK(memcmp(before, sim_flash_data, STAGING) == 0);
	const char *one_less[] = { "tsize", "716383", NULL };
	CHECK(put("codeplug.bin", one_less, img, TFTP_CODEPLUG_SIZE - 1, 512) == -(TFTP_ERR_FULL + 1));
	CHECK(memcmp(before, sim_flash_data, STAGING) == 0 && !tftp_codeplug_resume());

	// Duplicates and a block from the future in the middle: each block taken once, the image stored as sent
	rename_channel(img, 3, "Dup test");
	drain();
	request(2, CPORT, "codeplug.bin", "octet", NULL);
	pkt_t k;
	CHECK(next_pkt(&k) && get16(k.p) == 4 && get16(k.p + 2) == 0);
	uint16_t tid = k.sport;
	uint32_t blocks = TFTP_CODEPLUG_SIZE / 512u + 1u;
	int acks = 0;
	for (uint32_t b = 1; b <= blocks; b++) {
		int n = (b < blocks) ? 512 : (int)(TFTP_CODEPLUG_SIZE % 512u);
		if ((b % 97u) == 0) {
			send_data(tid, (uint16_t)(b + 1u), img + b * 512u, 512);	// ahead of time: ignored
			CHECK(!next_pkt(&k));
		}
		send_data(tid, (uint16_t)b, img + (b - 1u) * 512u, n);
		if ((b % 89u) == 0 && b < blocks) {
			send_data(tid, (uint16_t)b, img + (b - 1u) * 512u, n);	// again: ACKed again, not written again
			send_data(tid, (uint16_t)(b - 1u), img + (b - 2u) * 512u, 512);	// an older one: ignored
		}
		while (next_pkt(&k)) {
			acks += get16(k.p) == 4;
		}
	}
	tick(3000);
	drain();
	CHECK(acks > (int)blocks - 2 && regions_equal(img) && codeplug_channel(3, &ch) && strcmp(ch.name, "Dup test") == 0);
	CHECK(sim_flash_data[STAGING] == 0xFF && sim_flash_violations() == 0);

	// blksize 8: 89548 blocks, the block number rolls over (65535 -> 0) in the middle of the image
	rename_channel(img, 5, "Rollover");
	rename_channel(img, 260, "Rollover far");	// in the flash part, after the roll-over
	const char *tiny[] = { "blksize", "8", NULL };
	CHECK(put("codeplug.bin", tiny, img, TFTP_CODEPLUG_SIZE, 8) == 0);
	CHECK(regions_equal(img) && codeplug_channel(5, &ch) && strcmp(ch.name, "Rollover") == 0);
	CHECK(codeplug_channel(260, &ch) && strcmp(ch.name, "Rollover far") == 0);
	tftp_stats_t st;
	tftpStats(&st);
	CHECK(st.state == TFTP_DALLYING || st.state == TFTP_IDLE);
	tick(10000);
	capture = true;
}

// The packet entry point straight from the gateway with mutated requests, ACKs, DATA and errors, in exact-size buffers:
// the server keeps one transfer at a time, every packet it sends is well formed, and nothing of the codeplug changes
// unless a whole image arrives (none can here: the RAM file and wr.bin take the writes)
static void test_fuzz(void)
{
	static uint8_t before[FLASH_SIZE];
	uint8_t seeds[8][80], buf[600];
	int seedLen[8];
	capture = false;
	populate();
	memcpy(before, sim_flash_data, FLASH_SIZE);
	tftpInit(&io, files, 2);	// ram.bin and wr.bin only
	int n = 0;
	seedLen[n++] = sprintf((char *)seeds[0] + 2, "ram.bin%coctet%cblksize%c1024%ctsize%c0%ctimeout%c2", 0, 0, 0, 0, 0, 0, 0) + 3;
	seedLen[n++] = sprintf((char *)seeds[1] + 2, "wr.bin%cOCTET%ctsize%c600%cblksize%c16", 0, 0, 0, 0, 0) + 3;
	seedLen[n++] = sprintf((char *)seeds[2] + 2, "/ram.bin%coctet", 0) + 3;
	put16(seeds[0], 1);
	put16(seeds[1], 2);
	put16(seeds[2], 1);
	put16(seeds[3], 4);	// ACK 1
	put16(seeds[3] + 2, 1);
	seedLen[3] = 4;
	put16(seeds[4], 3);	// DATA 1, 16 bytes
	put16(seeds[4] + 2, 1);
	memset(seeds[4] + 4, 0x5A, 16);
	seedLen[4] = 20;
	put16(seeds[5], 4);	// ACK 0 (after an OACK)
	put16(seeds[5] + 2, 0);
	seedLen[5] = 4;
	put16(seeds[6], 5);	// ERROR
	put16(seeds[6] + 2, 0);
	seeds[6][4] = 'x';
	seeds[6][5] = 0;
	seedLen[6] = 6;
	put16(seeds[7], 3);	// DATA 2, short: the last
	put16(seeds[7] + 2, 2);
	seedLen[7] = 7;

	fuzz_t f;
	fuzz_init(&f, 0x7F7Fu);
	long iters = fuzz_iters(20000), bad = 0, answers = 0;
	uint16_t last_op = 0, last_block = 0, last_port = 0;
	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 8);
		memcpy(buf, seeds[s], (size_t)seedLen[s]);
		int len = seedLen[s];
		// requests to port 69; the rest to the transfer's port (49152 on), sometimes from a foreign port
		uint16_t dport = (s < 3) ? TFTP_PORT : (uint16_t)(49152u + fuzz_below(&f, 4));
		if ((last_op != 0) && (fuzz_below(&f, 2) == 0)) {
			// the right answer to the server's last packet (ACK for DATA / OACK, the next DATA for an ACK), to get
			// deep into transfers; sometimes mutated too
			dport = last_port;
			put16(buf, (last_op == 4) ? 3 : 4);
			put16(buf + 2, (last_op == 3) ? last_block : (last_op == 4) ? (uint16_t)(last_block + 1u) : 0);
			len = 4;
			if (last_op == 4) {
				len += (int)fuzz_below(&f, 2) ? 16 : (int)fuzz_below(&f, 16);
				memset(buf + 4, 0x33, 16);
			}
			s = 3;
		}
		if ((s != 3) || (fuzz_below(&f, 4) == 0)) {
			len = fuzz_mutate(&f, buf, len, (int)sizeof(buf));
		}
		uint8_t *p = fuzz_exact(buf, len);
		uint16_t sport = (fuzz_below(&f, 8) == 0) ? (uint16_t)(CPORT + 1) : (uint16_t)CPORT;
		ring_out = ring_in;
		tftpReceive(HOST_IP, sport, dport, p, len, now);
		free(p);
		now += (fuzz_below(&f, 8) == 0) ? fuzz_below(&f, 6000) : fuzz_below(&f, 20);
		tftpTick(now);
		pkt_t k;
		while (next_pkt(&k)) {
			uint16_t op = get16(k.p);
			bool ok = k.len >= 4 && op >= 3 && op <= 6 && k.len <= IPGW_UDP_PAYLOAD_MAX;
			answers++;
			last_op = (op == 5) ? 0 : op;
			last_block = get16(k.p + 2);
			last_port = k.sport;
			if (op == 5) {
				ok = ok && k.p[k.len - 1] == 0;	// the message is NUL terminated
			}
			if (!ok && bad++ < 3) {
				printf("  fuzz #%ld: bad packet op %u len %d\n", i, op, k.len);
			}
		}
	}
	tick(20000);	// whatever was going on times out
	tftp_stats_t st;
	tftpStats(&st);
	CHECK(bad == 0 && st.state == TFTP_IDLE && answers > iters / 4);
	CHECK(memcmp(before, sim_flash_data, FLASH_SIZE) == 0);
	tftpStats(&st);
	printf("# tftp fuzz: %ld packets, %ld answers (%u reads, %u writes done)\n", iters, answers, (unsigned)st.reads,
	       (unsigned)st.writes);
	tftpInit(&io, files, 3);
	capture = true;
}

int main(void)
{
	uint32_t header[6] = { 0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1 };
	pcap = fopen("tftpTest.pcap", "wb");
	fwrite(header, sizeof(header), 1, pcap);
	sim_flash_create(FLASH_SIZE, &map);
	ipGatewayInit(RADIO_MAC, HOST_MAC);
	tftpInit(&io, files, 3);
	ipGatewayUdpBind(TFTP_PORT, udp_in);

	test_reads();
	test_refusals();
	test_writes();
	test_codeplug();
	test_power_cuts();
	test_codeplug_edges();
	test_fuzz();

	fclose(pcap);
	sim_flash_free();
	printf("# tftpTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
