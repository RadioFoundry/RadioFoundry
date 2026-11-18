// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware image's SHA-256, a piece at a time (image_hash.h)
#include <string.h>
#include "board.h"
#include "support/image_hash.h"

#define READ_BYTES	256u	// one read() into the caller's stack

static uint8_t known[SHA256_LEN];
static volatile bool have;	// set after known is written: another task may read it

bool image_hash_span(uint32_t *addr, uint32_t *len)
{
	if (board.system == NULL || board.system->image == NULL || board.system->read == NULL) {
		return false;
	}
	board.system->image(addr, len);
	return *len != 0u;
}

bool image_hash_known(uint8_t out[SHA256_LEN])
{
	if (!have) {
		return false;
	}
	memcpy(out, known, SHA256_LEN);
	return true;
}

bool image_hash_step(image_hash_t *h, uint32_t max, uint8_t out[SHA256_LEN])
{
	if (image_hash_known(out)) {
		return true;
	}
	if (!h->started) {
		if (!image_hash_span(&h->addr, &h->len)) {
			return false;
		}
		sha256_init(&h->sha);
		h->done = 0;
		h->started = true;
	}
	uint8_t buf[READ_BYTES];
	for (uint32_t n = 0; n < max && h->done < h->len;) {
		uint32_t k = h->len - h->done < READ_BYTES ? h->len - h->done : READ_BYTES;
		board.system->read(h->addr + h->done, buf, k);
		sha256_update(&h->sha, buf, k);
		h->done += k;
		n += k;
	}
	if (h->done < h->len) {
		return false;
	}
	sha256_final(&h->sha, out);
	memcpy(known, out, SHA256_LEN);
	have = true;
	h->started = false;
	return true;
}
