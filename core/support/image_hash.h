// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware image's SHA-256, as the release manifest and SHA256SUMS give it for the same file
// (docs/spec/support-info.md 3): a host compares it with ours to tell one of our images from any other. Only on request
// (the About page opens, the debug link's SUPPORT HASH), never at start-up, and a bounded piece per step, so the
// caller's task stays responsive. No RTOS calls: each caller steps its own state; the first to finish keeps the result.
#ifndef IMAGE_HASH_H
#define IMAGE_HASH_H

#include <stdbool.h>
#include <stdint.h>
#include "support/sha256.h"

#define IMAGE_HASH_STEP_UI	0x10000u	// bytes per About redraw (a few ms on the UI task)
#define IMAGE_HASH_STEP_LINK	0x8000u		// bytes per debug link request (a short handler)

typedef struct {
	sha256_t sha;
	uint32_t addr, len;	// the image (hal_system_t.image)
	uint32_t done;		// bytes hashed
	bool started;
} image_hash_t;

// The image's span, false where the platform doesn't say (no hash then)
bool image_hash_span(uint32_t *addr, uint32_t *len);
// Hash up to max more bytes (starting the hash on the first call). True when the hash is known: out then holds it
// (the result is kept, so later calls return it at once)
bool image_hash_step(image_hash_t *h, uint32_t max, uint8_t out[SHA256_LEN]);
// The kept result, false until some caller finished
bool image_hash_known(uint8_t out[SHA256_LEN]);

#endif
