// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR forward error correction used by the data, hotspot and SFR code, from ETSI TS 102 361-1 annex B:
 * BPTC(196,96) (B.1.1) with its Hamming(15,11,3) row and Hamming(13,9,3) column codes (B.3.4, B.3.5), the
 * slot type's Golay(20,8) (B.3.1), the voice EMB's QR(16,7,6) (B.3.2), and the reverse channel (9.1.5, B.2.2.2,
 * B.3.13).
 *
 * A burst is 33 bytes (264 bits, most significant bit of byte 0 first): 98 info bits, 10 slot type bits, 48 sync bits,
 * 10 slot type bits, 98 info bits. The 196 BPTC info bits are burst bits 0-97 and 166-263; the slot type is burst bits
 * 98-107 and 156-165.
 */

#ifndef RFOUNDRY_DMR_FEC_H
#define RFOUNDRY_DMR_FEC_H

#include <stdbool.h>
#include <stdint.h>

#define DMR_FEC_BURST_BYTES			33
#define DMR_FEC_BPTC_PAYLOAD_BYTES	12		// 96 bits, I(95) first

// BPTC(196,96): 12 payload bytes into the 196 info bits of the burst. Only the info bits are written; the slot type
// and sync bits are left as they are.
void dmrFecBptcEncode(const uint8_t *payload, uint8_t *burst);

// BPTC(196,96) back to the 12 payload bytes, single errors in rows and columns corrected. Returns the number of bits
// corrected, or -1 when errors remain that the code can't fix (the payload is then the best guess).
int dmrFecBptcDecode(const uint8_t *burst, uint8_t *payload);

// The same with the reserved bits R(3)-R(0) (361-1 B.1.1), as r bits 3-0. The standard sends them 0; some systems set
// them, and a codeword re-encoded to compare with what was received needs them as sent. rbits holds R(3) in bit 3
// down to R(0) in bit 0; the decode's may be NULL
void dmrFecBptcEncodeR(const uint8_t *payload, uint8_t rbits, uint8_t *burst);
int dmrFecBptcDecodeR(const uint8_t *burst, uint8_t *payload, uint8_t *rbits);

// The same from the code matrix: rows[r] is row r (0-12), column 0 in bit 14; the reserved bit R(3) is not in it. The
// rows are corrected in place
int dmrFecBptcDecodeRows(uint16_t *rows, uint8_t *payload);

// Hamming codewords as unsigned integers, codeword bit 0 (the first transmitted, data bit 0) in the most significant
// position: (15,11,3) is 15 bits wide, 11 data then 4 parity; (13,9,3) is 13 bits, 9 data then 4 parity.
uint16_t dmrFecHamming15113Encode(uint16_t data);
uint16_t dmrFecHamming1393Encode(uint16_t data);
// Correct one bit error in place. Returns false if the syndrome matches no single bit (more than one error).
bool dmrFecHamming15113Correct(uint16_t *codeword);
bool dmrFecHamming1393Correct(uint16_t *codeword);

// Golay(20,8): the 20 bit codeword for 8 data bits, data in bits 19-12, then 12 parity bits
uint32_t dmrFecGolay2087Encode(uint8_t data);
// Nearest codeword's data (up to 3 bit errors corrected). Returns the number of bit errors, or -1 if there were too many.
int dmrFecGolay2087Decode(uint32_t codeword, uint8_t *data);

// Slot type: color code (4 bits) and data type (4 bits), Golay(20,8) coded, into / out of the burst
void dmrFecSlotTypeEncode(uint8_t colorCode, uint8_t dataType, uint8_t *burst);
bool dmrFecSlotTypeDecode(const uint8_t *burst, uint8_t *colorCode, uint8_t *dataType);

// QR(16,7,6): the 16 bit codeword for 7 data bits, data in bits 15-9, then 9 parity bits
uint16_t dmrFecQr1676Encode(uint8_t data);
// Nearest codeword's data (up to 2 bit errors corrected). Returns the number of bit errors, or -1 if there were too many.
int dmrFecQr1676Decode(uint16_t codeword, uint8_t *data);

// The EMB of voice bursts B-F (ETSI TS 102 361-1 9.1.2): color code (4 bits), PI (1), LCSS (2), QR(16,7,6) coded.
// In a burst, codeword bits 15-8 are burst bits 108-115 and bits 7-0 are burst bits 148-155 (the 32 embedded
// signaling bits sit between them)
uint16_t dmrFecEmbEncode(uint8_t colorCode, bool pi, uint8_t lcss);
// Returns the number of bit errors corrected, or -1 if the EMB can't be trusted
int dmrFecEmbDecode(uint16_t emb, uint8_t *colorCode, bool *pi, uint8_t *lcss);

// Reverse channel (RC, ETSI TS 102 361-1 9.1.5): a 4-bit command (TS 102 361-4 table 6.32) and its CRC-7 (B.3.13)
// masked with 0x7A (table B.21) make the 11 RC info bits, RC(10)-RC(0); the single burst BPTC (B.2.2.2) makes them
// the 32-bit field sent in place of an embedded LC fragment.
#define DMR_RC_POWER_UP		0x0u
#define DMR_RC_POWER_DOWN	0x1u
#define DMR_RC_POWER_HIGHEST	0x2u
#define DMR_RC_POWER_LOWEST	0x3u
#define DMR_RC_CEASE		0x4u	// cease transmission command
#define DMR_RC_CEASE_REQUEST	0x5u	// cease transmission request: the MS may go on

// The 11 RC info bits for a command: the command in bits 10-7, the masked CRC-7 in bits 6-0
uint16_t dmrFecRcInfo(uint8_t command);
// The command in 11 RC info bits (as a modem that decodes the BPTC itself gives them), or -1 when the CRC fails
int dmrFecRcInfoDecode(uint16_t info);
// The 32-bit field for a command, interleaved (table B.5B): TX(31), the first bit sent, in bit 31
uint32_t dmrFecRcEncode(uint8_t command);
// The nearest command to a received field (up to 3 bit errors corrected). Returns the number of bit errors, or -1 if
// there were too many
int dmrFecRcDecode(uint32_t field, uint8_t *command);
// Voice burst F's late-entry privacy field (DMRA, measured on air on AES-256 calls): the 11 info bits are the
// key ID in bits 10-3 and the algorithm's low 3 bits in 2-0 (0x25 -> 5), single burst BPTC as the RC's but with each
// column's second bit a copy of the first (even parity). ALG 0x25, key ID 1 -> 0x44428881. TX(31) first in bit 31
uint32_t dmrFecPrivacyField(uint8_t alg, uint8_t keyId);
// An outbound RC burst's 48-bit embedded field (6.4.2, 9.3.2, 9.3.3), in place of the sync or an embedded LC
// fragment: the EMB with PI 1 and LCSS 00 (a single fragment) around the 32 RC bits. 6 bytes, MSB first: EMB 8 bits,
// RC 32, EMB 8 (on air, burst bits 108-155)
void dmrFecRcField(uint8_t colorCode, uint8_t command, uint8_t field[6]);
// The RC in a received embedded field, when its EMB has colorCode, PI 1 and LCSS 00. Returns the bit errors corrected
// (EMB and RC), or -1 when the field carries no RC
int dmrFecRcFromField(const uint8_t *field, uint8_t colorCode, uint8_t *command);

#endif
