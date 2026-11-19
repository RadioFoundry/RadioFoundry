// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware image's SHA-256, a piece at a time (image_hash.h)
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "support/image_hash.h"

#define READ_BYTES	256u	// one read() into the caller's stack

static struct {
	sha256_t sha;
	uint32_t addr, len;	// the image (hal_system_t.image)
	uint32_t done;		// bytes hashed
	bool started;
} h;
static uint8_t known[SHA256_LEN];
static volatile bool have;	// set after known is written: another task may read it

// Who is stepping h. The two callers run in different tasks and a step may be preempted, so a step enters only when
// the other's isn't running: Peterson's entry test, but a caller that loses goes away instead of waiting (it tries
// again on its next redraw or request). Plain loads and stores with barriers, since not every core here has an atomic
// swap, and no RTOS call
static volatile bool want[2];
static volatile uint8_t turn;

static bool enter(image_hash_caller_t me)
{
	image_hash_caller_t other = me == IMAGE_HASH_UI ? IMAGE_HASH_LINK : IMAGE_HASH_UI;
	want[me] = true;
	turn = (uint8_t)other;
	__sync_synchronize();
	if (want[other] && turn == (uint8_t)other) {
		want[me] = false;
		return false;
	}
	return true;
}

static void leave(image_hash_caller_t me)
{
	__sync_synchronize();	// h is written before the other may enter
	want[me] = false;
}

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

uint32_t image_hash_done(void)
{
	return h.done;
}

bool image_hash_step(image_hash_caller_t who, uint32_t max, uint8_t out[SHA256_LEN])
{
	if (image_hash_known(out)) {
		return true;
	}
	if (!enter(who)) {
		return false;
	}
	if (image_hash_known(out)) {	// the other caller finished it since the test above
		leave(who);
		return true;
	}
	if (!h.started) {
		if (!image_hash_span(&h.addr, &h.len)) {
			leave(who);
			return false;
		}
		sha256_init(&h.sha);
		h.done = 0;
		h.started = true;
	}
	uint8_t buf[READ_BYTES];
	uint32_t t0 = hal_millis();
	for (uint32_t n = 0; n < max && h.done < h.len;) {
		uint32_t k = h.len - h.done < READ_BYTES ? h.len - h.done : READ_BYTES;
		board.system->read(h.addr + h.done, buf, k);
		sha256_update(&h.sha, buf, k);
		h.done += k;
		n += k;
		if (hal_millis() - t0 >= IMAGE_HASH_STEP_MS) {	// after a read, so a preempted step still moves on
			break;
		}
	}
	bool done = h.done >= h.len;
	if (done) {
		sha256_final(&h.sha, out);
		memcpy(known, out, SHA256_LEN);
		__sync_synchronize();	// known is written before have says so
		have = true;
	}
	leave(who);
	return done;
}
