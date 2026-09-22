// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A TFTP server (RFC 1350, octet mode only) with the option extension (RFC 2347): blksize (RFC 2348), tsize and
// timeout (RFC 2349). One transfer at a time, from a fixed table of files (docs/spec/tftp.md). CONFIG_TFTP.
//
// No RTOS here: the user feeds the datagrams for port 69 and for the transfer's own port in, calls tftpTick every few
// ms, and sends through tftp_io_t (core/net/net_services.c is the firmware glue, tests/host/tftpTest.c the fake).
#ifndef ODR_TFTP_H
#define ODR_TFTP_H

#include <stdint.h>
#include <stdbool.h>

#define TFTP_PORT		69
#define TFTP_BLKSIZE		512		// without the blksize option
#define TFTP_TIMEOUT_MS		1000		// retransmit after this without an answer (the timeout option overrides)
#define TFTP_RETRIES		5		// then the transfer is abandoned
#define TFTP_DALLY_MS		3000		// after the last ACK of a write: a resent last DATA is ACKed again

// Error codes (RFC 1350 5, RFC 2347)
enum {
	TFTP_ERR_OTHER = 0, TFTP_ERR_NOT_FOUND = 1, TFTP_ERR_ACCESS = 2, TFTP_ERR_FULL = 3, TFTP_ERR_ILLEGAL = 4,
	TFTP_ERR_TID = 5, TFTP_ERR_EXISTS = 6, TFTP_ERR_USER = 7, TFTP_ERR_OPTION = 8,
};

// What a file's write callbacks return
#define TFTP_OK			0
#define TFTP_PENDING		(-1)		// commit: not done yet, ask again on the next tick
#define TFTP_FAIL(code)		(0x100 | (code))	// refused: the client gets error code (TFTP_ERR_*)

// A file. read == NULL: write only; open_write == NULL: read only. *msg: the error text for the client (optional)
typedef struct {
	const char *name;
	// Read: the size now, then the bytes at off (off + len <= size)
	bool (*open_read)(uint32_t *size);
	bool (*read)(uint32_t off, uint8_t *buf, uint16_t len);
	// Write: open with the size the client announced (tsize, 0 = not given): TFTP_OK or TFTP_FAIL(code)
	int (*open_write)(uint32_t tsize, const char **msg);
	// The bytes of one DATA block, in order: TFTP_OK or TFTP_FAIL(code)
	int (*write)(uint32_t off, const uint8_t *buf, uint16_t len, const char **msg);
	// The last block is in (size bytes in all): check and store it. TFTP_OK, TFTP_PENDING or TFTP_FAIL(code); the
	// last ACK goes out only after TFTP_OK, so the client learns whether the file was taken
	int (*commit)(uint32_t size, const char **msg);
	void (*abort)(void);		// a write that didn't finish: drop what came
} tftp_file_t;

typedef struct {
	// Room for one packet of at most max bytes, kept until send returns; send false = busy, tried again
	uint8_t *(*buffer)(int *max);
	bool (*send)(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *pkt, int length);
	// The transfer's own port (its TID) is bound to tftpReceive while it lasts, then unbound (port 0)
	void (*bind)(uint16_t port, bool on);
} tftp_io_t;

typedef enum { TFTP_IDLE, TFTP_READING, TFTP_WRITING, TFTP_COMMITTING, TFTP_DALLYING } tftp_state_t;

typedef struct {
	uint32_t reads, writes;		// transfers completed
	uint32_t errors;		// error packets sent (refusals, failed transfers)
	uint32_t timeouts;		// transfers abandoned after TFTP_RETRIES
	uint32_t retransmits;
	uint32_t last_size;		// bytes in the last transfer
	uint8_t state;			// tftp_state_t
	uint16_t blksize;		// of the current or last transfer
} tftp_stats_t;

void tftpInit(const tftp_io_t *io, const tftp_file_t *const *files, int count);
// A datagram to TFTP_PORT or to the transfer's port
void tftpReceive(uint32_t srcIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *pkt, int length, uint32_t now);
void tftpTick(uint32_t now);
void tftpStats(tftp_stats_t *s);

#endif
