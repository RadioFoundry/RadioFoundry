// SPDX-License-Identifier: MIT Copyright (c) 2026 The OpenDigitalRadio Authors codeplug.bin for the TFTP server
// (docs/spec/tftp.md 3, 4): the open-firmware codeplug image in the open-firmware CPS's address space, the same flat
// file tools/cps_client.py reads and writes (file offset = STM32 wire address). Reads give the regions qdmr transfers
// and 0xFF elsewhere; a write goes to the flash map's staging area first, is checked there, and only then replaces the
// stored codeplug, sector by sector through the journal, under a commit record that a power cut can't break
// (tftp_codeplug_resume finishes it at the next boot). CONFIG_TFTP.
#ifndef ODR_TFTP_CODEPLUG_H
#define ODR_TFTP_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "net/tftp.h"
#include "storage/storage.h"

#define TFTP_CODEPLUG_SIZE	0xAEE60u	// up to the end of the flash codeplug region
#define TFTP_CODEPLUG_FULL	0x120000u	// a whole cps_client.py image is taken too (the rest is ignored)
#define TFTP_STAGING_MAGIC	STORAGE_STAGE_MAGIC_TFTP	// the staging header's first word

extern const tftp_file_t tftp_codeplug_file;

// Writes allowed (the setting SETTINGS_NET_TFTP_NO_WRITE, read at start-up and when the user changes it)
void tftp_codeplug_allow_write(bool allow);
bool tftp_codeplug_write_allowed(void);

// At boot, after storage_init and before codeplug_init: a commit that a power cut interrupted is finished. True if
// one was. A record of the fast codeplug protocol (STORAGE_STAGE_MAGIC_CPSF) is left alone
bool tftp_codeplug_resume(void);
// A write is open: the staging area is this server's until it ends (the fast codeplug protocol waits)
bool tftp_codeplug_writing(void);

#endif
