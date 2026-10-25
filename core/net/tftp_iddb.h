// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR ID database over TFTP (docs/spec/tftp.md 5): iddb.bin, the database alone (its 12-byte header, then the
// records back to back, as a CPS would upload them), and the database part of a combined codeplug.bin. Written
// straight into its areas, no staging: the header is erased first and programmed last, so a transfer that stops
// leaves no database, never a broken one. CONFIG_TFTP.
#ifndef RFOUNDRY_TFTP_IDDB_H
#define RFOUNDRY_TFTP_IDDB_H

#include <stdint.h>
#include <stdbool.h>
#include "net/tftp.h"

// Image addresses (file offsets of codeplug.bin, open-firmware-cps.md 7.5): area 1 holds the header and as many
// whole records as fit, the rest continue at area 2, up to the end of the open-firmware space
#define TFTP_IDDB_HEADER	12u
#define TFTP_IDDB_AREA1		0x50000u
#define TFTP_IDDB_AREA1_SIZE	0x40000u
#define TFTP_IDDB_AREA2		0xD8000u

extern const tftp_file_t tftp_iddb_file;

// codeplug.bin's database part. The header (as it is in the file at TFTP_IDDB_AREA1): true if it is a database the
// radio reads and it fits; *end is then the image address after its last record
bool tftp_iddb_header_ok(const uint8_t *h, uint32_t *end, const char **msg);
// Start writing that database: the stored header is erased (the radio has no database until tftp_iddb_finish)
int tftp_iddb_begin(const uint8_t *h, const char **msg);
// Bytes of the image, in order: those inside the database's records are written, the rest ignored
int tftp_iddb_image(uint32_t off, const uint8_t *buf, uint32_t len, const char **msg);
// Every record is in: the header is programmed and the database read again. TFTP_OK or TFTP_FAIL
int tftp_iddb_finish(const char **msg);
// Stop without a header (no database)
void tftp_iddb_cancel(void);
// The image address after the last byte the open-firmware space holds (the end of area 2)
uint32_t tftp_iddb_image_end(void);

#endif
