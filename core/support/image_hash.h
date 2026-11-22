// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware image's SHA-256, as the release manifest and SHA256SUMS give it for the same file
// (docs/spec/support-info.md 3): a host compares it with ours to tell one of our images from any other. Only on request
// (the About page opens, the debug link's SUPPORT HASH), never at start-up, and a bounded piece per step, so the
// caller's task stays responsive. One hash for both callers, so they share its RAM and its progress; no RTOS calls.
#ifndef IMAGE_HASH_H
#define IMAGE_HASH_H

#include <stdbool.h>
#include <stdint.h>
#include "support/sha256.h"

#define IMAGE_HASH_STEP_UI	0x10000u	// at most this many bytes per About redraw
#define IMAGE_HASH_STEP_LINK	0x8000u		// at most this many bytes per debug link request (a short handler)
// and a step ends after this long, whatever it read: the image's read() may be a slow SPI flash and the SHA-256 a
// slow core, and the UI task does nothing else while its step runs
#define IMAGE_HASH_STEP_MS	5u

// The two callers, each in its own task: a step from one while the other's runs does nothing (image_hash.c)
typedef enum {
	IMAGE_HASH_UI,
	IMAGE_HASH_LINK,
} image_hash_caller_t;

// The image's span, false where the platform doesn't say (no hash then)
bool image_hash_span(uint32_t *addr, uint32_t *len);
// Hash up to max more bytes (starting the hash on the first call). True when the hash is known: out then holds it
// (the result is kept, so later calls return it at once)
bool image_hash_step(image_hash_caller_t who, uint32_t max, uint8_t out[SHA256_LEN]);
// The kept result, false until it is done
bool image_hash_known(uint8_t out[SHA256_LEN]);
// Bytes hashed so far, the image's length once a step started it
uint32_t image_hash_done(void);

#endif
