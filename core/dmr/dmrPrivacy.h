// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR voice privacy (static keystream "basic privacy" schemes)
 */

#ifndef RFOUNDRY_DMRPRIVACY_H
#define RFOUNDRY_DMRPRIVACY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The keystream is XORed onto the 49 AMBE+2 parameter bits of each vocoder frame (ambe_d[] order, as in mbelib and
 * DSD-FME), between the FEC decode and the vocoder. A voice burst holds 3 frames, a superframe (bursts A-F) 18, and the
 * keystream position restarts at each burst A. The burst is handled in its on-air form (27 bytes, 3 x 72 FEC coded bits),
 * so it is applied where the burst sequence is known: when the burst is handed to the HR-C6000 for TX, and before the
 * vocoder on RX.
 */

typedef enum
{
	DMR_PRIVACY_OFF = 0,
	DMR_PRIVACY_MOTOROLA_BP,  // Motorola Basic Privacy, key 1-255
	DMR_PRIVACY_ANYTONE_BP,   // Anytone 16-bit Basic Privacy, key 0x0000-0xFFFF
	DMR_PRIVACY_NUM_TYPES
} dmrPrivacyType_t;

#define DMR_PRIVACY_FID                 0x10 // FID sent in the voice LC of a private call (Motorola)
#define DMR_PRIVACY_SERVICE_OPTION      0x40 // Privacy bit of the Service Options (ETSI TS 102 361-2 7.2.1)

#define DMR_PRIVACY_AMBE_FRAME_BITS     49
#define DMR_PRIVACY_AMBE_FRAME_BYTES    9    // 72 FEC coded bits
#define DMR_PRIVACY_AMBE_BURST_FRAMES   3

bool dmrPrivacyKeyIsValid(dmrPrivacyType_t type, uint16_t key);
uint16_t dmrPrivacyKeyMax(dmrPrivacyType_t type);
const char *dmrPrivacyTypeName(dmrPrivacyType_t type);

// The Service Options byte of a voice LC with the privacy bit set when a scheme is in use
uint8_t dmrPrivacyServiceOptions(dmrPrivacyType_t type, uint8_t serviceOptions);
uint8_t dmrPrivacyFID(dmrPrivacyType_t type, uint8_t fid);
// Whether a received voice LC marks the call as private
bool dmrPrivacyLCIsPrivate(const uint8_t *lc);

// Encrypt (TX) or decrypt (RX) the 3 frames of a voice burst in place. voiceSequence is the burst, 0 (A) to 5 (F).
// correctErrors runs the frames through the FEC decoder first (RX); TX frames come straight from the encoder.
void dmrPrivacyApplyBurst(dmrPrivacyType_t type, uint16_t key, uint8_t *ambe, uint8_t voiceSequence, bool correctErrors);

// The AMBE+2 3600x2450 FEC of one vocoder frame: 49 parameter bits (one per byte) <-> 9 interleaved bytes on air
void dmrPrivacyAmbeEncode(const uint8_t *bits, uint8_t *frame);
void dmrPrivacyAmbeExtract(const uint8_t *frame, uint8_t *bits); // no error correction

// A key as the call stack's privacy hook takes it (dmr_call_config_t.privacy_ctx)
typedef struct
{
	dmrPrivacyType_t type;
	uint16_t key;
} dmrPrivacyKey_t;

// The call stack's voice privacy hook (dmr_privacy_fn, core/dmr/call/dmr_call.h): ctx is a dmrPrivacyKey_t. RX bursts
// are FEC corrected (core/dmr/ambe/ambeFec) before the keystream is removed, TX bursts are taken as encoded
void dmrPrivacyCallHook(uint8_t *ambe27, uint8_t voiceIndex, bool tx, void *ctx);

#endif
