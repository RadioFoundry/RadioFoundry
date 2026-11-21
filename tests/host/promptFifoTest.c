// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker path through the HR_C6000's prompt FIFO (#673; modem-hrc6000.md 8.1): the ring between the audio task
// and the feeder (platform/stm32f4/hrc/pcm_ring.h) gives back every block in order, in 32-sample chunks, across the
// 2^32 index wrap, reports an underrun as silence and refuses a block it has no room for; the chip's FIFO functions
// (hrc_prompt_*) set and clear only 4:0x06 bit 1, read the room from 4:0x88 bit 0 and write 32 samples big endian to
// page 3 address 0x00. Against a fake register file. The chip the speaker goes to (pcm_fifo_chip, #753): the FM copy's
// always, a chip in FM receive only while the PCM route is on; and a beep in FM receive, through the ring at the
// feeder's priming and the FIFO's drain, has played its tone out before the route's hold ends

#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000.h"
#include "../../platform/stm32f4/hrc/pcm_ring.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

#define BLOCK	160	// AUDIO_BLOCK_SAMPLES: 20 ms at 8 kHz

static uint8_t regs[8][256];
static int page3_writes;

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	if (hdr_len != 2) {
		return false;
	}
	uint8_t page = hdr[0] & 7u;
	bool read = (hdr[0] & 0x80u) != 0;
	page3_writes += !read && page == 3;
	for (uint16_t i = 0; i < len; i++) {
		uint8_t r = (uint8_t)(hdr[1] + i);
		if (read) {
			rx[i] = regs[page][r];
		} else {
			regs[page][r] = tx != NULL ? tx[i] : 0xFFu;
		}
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, NULL };
static hrc_t dev;
static pcm_ring_t ring;

static void block(int16_t *b, int16_t first)
{
	for (int i = 0; i < BLOCK; i++) {
		b[i] = (int16_t)(first + i);
	}
}

// Feeds n blocks, three in hand, popping a chunk at a time; true if every sample came out in order
static bool stream(uint32_t start, int n)
{
	ring.head = ring.tail = start;
	int16_t b[BLOCK], c[PCM_RING_CHUNK];
	int16_t want = 0, next = 0;
	int pushed = 0;
	bool ok = true;
	while (pushed < n || pcm_ring_level(&ring) > 0) {
		while (pushed < n && pcm_ring_fits(&ring, BLOCK)) {
			block(b, next);
			ok &= pcm_ring_push(&ring, b, BLOCK);
			next = (int16_t)(next + BLOCK);
			pushed++;
		}
		ok &= pcm_ring_pop(&ring, c);
		for (unsigned i = 0; i < PCM_RING_CHUNK; i++) {
			ok &= c[i] == want;
			want++;
		}
	}
	return ok && want == next;
}

// A beep in FM receive as i2s_pcm.c plays it, a block at a time: the route comes on in the first (lead-in) block's
// callback, and the FIFO follows the route a block late (fifo_follow runs before the callback). The feeder starts once
// FIFO_PRIME is in the ring and keeps the FIFO's 64 samples topped up while the chip plays 8 a millisecond; the route goes
// off hold blocks after the tone, and the FIFO with it (what it still holds is lost). Returns the tone samples played
#define FIFO_PRIME	(2 * BLOCK)	// i2s_pcm.c
#define FIFO_DEPTH	64		// HRC_PROMPT_DEPTH
static int beep_through_fifo(int tone_blocks, int hold_blocks)
{
	int16_t b[BLOCK], c[PCM_RING_CHUNK];
	int16_t fifo[FIFO_DEPTH];
	int fill = 0, played = 0;
	bool route = false, on = false, primed = false;
	pcm_ring_reset(&ring);
	for (int blk = 0; blk <= 1 + tone_blocks + hold_blocks; blk++) {
		if (on != route) {				// fifo_follow: the route as the last block left it
			on = route;
			if (!on) {
				break;
			}
			pcm_ring_reset(&ring);
			primed = false;
		}
		bool tone = blk >= 1 && blk <= tone_blocks;
		route = blk < 1 + tone_blocks + hold_blocks;	// the callback: lead-in, tone and hold
		if (on) {
			for (int i = 0; i < BLOCK; i++) {
				b[i] = tone ? 1 : 0;
			}
			CHECK(pcm_ring_push(&ring, b, BLOCK));
		}
		for (int ms = 0; ms < 20; ms++) {		// the feeder's polls through the block
			for (int i = 0; i < 8 && fill > 0; i++) {	// the chip plays 8 samples a millisecond
				played += fifo[0];
				memmove(fifo, fifo + 1, (size_t)--fill * sizeof fifo[0]);
			}
			primed = primed || pcm_ring_level(&ring) >= FIFO_PRIME;
			while (on && primed && fill + (int)PCM_RING_CHUNK <= FIFO_DEPTH) {
				(void)pcm_ring_pop(&ring, c);	// an underrun is silence
				memcpy(&fifo[fill], c, sizeof c);
				fill += PCM_RING_CHUNK;
			}
		}
	}
	return played;
}

int main(void)
{
	// --- the ring ---
	CHECK(stream(0, 50));
	CHECK(stream(0xFFFFFF00u, 50));		// head and tail wrap through 2^32 mid-stream
	CHECK(stream(0u - 2u * BLOCK, 7));		// the wrap two blocks in

	int16_t b[BLOCK], c[PCM_RING_CHUNK];
	ring.head = ring.tail = 0;
	block(b, 1);
	CHECK(pcm_ring_push(&ring, b, BLOCK));
	CHECK(pcm_ring_push(&ring, b, BLOCK));
	CHECK(pcm_ring_push(&ring, b, BLOCK));
	CHECK(!pcm_ring_push(&ring, b, BLOCK));		// 480 of 512 used: a fourth block doesn't fit
	CHECK(pcm_ring_level(&ring) == 3u * BLOCK);	// and nothing of it was written

	ring.head = ring.tail = 64;
	memset(c, 0x55, sizeof c);
	CHECK(!pcm_ring_pop(&ring, c));			// empty: an underrun, silence out
	for (unsigned i = 0; i < PCM_RING_CHUNK; i++) {
		CHECK(c[i] == 0);
	}
	CHECK(ring.tail == 64u);			// an underrun takes nothing

	ring.head = 1000;
	ring.tail = 0;
	pcm_ring_reset(&ring);
	CHECK(pcm_ring_level(&ring) == 0);

	// A ring from the heap starts with garbage indices: the reset puts them on a chunk boundary, so no chunk runs past
	// the end of s (it once played the memory after the ring)
	ring.head = ring.tail = 0x12345677u;
	pcm_ring_reset(&ring);
	CHECK(ring.head % PCM_RING_CHUNK == 0 && ring.tail % PCM_RING_CHUNK == 0);
	CHECK(stream(ring.head, 20));

	// --- which chip ---
	int copy_chip, fm_chip;
	CHECK(pcm_fifo_chip(&copy_chip, NULL, false) == &copy_chip);	// the copy: every block, route or not
	CHECK(pcm_fifo_chip(&copy_chip, &fm_chip, true) == &copy_chip);
	CHECK(pcm_fifo_chip(NULL, &fm_chip, true) == &fm_chip);		// FM receive: a beep
	CHECK(pcm_fifo_chip(NULL, &fm_chip, false) == NULL);		// FM audio alone: the FIFO stays off
	CHECK(pcm_fifo_chip(NULL, NULL, true) == NULL);			// DMR: I2S reaches the line out

	// --- a beep in FM receive ---
	CHECK(beep_through_fifo(2, 4) == 2 * BLOCK);	// a key beep (two blocks): whole within beep.c's 4-block hold
	CHECK(beep_through_fifo(20, 4) == 20 * BLOCK);	// a 400 ms beep (TX denied; the error beep is 150 ms)
	CHECK(beep_through_fifo(20, 0) < 20 * BLOCK);	// no hold: the tail is lost (the model can fail)

	// --- the chip's FIFO ---
	hrc6000_attach(&dev, &bus);
	regs[4][0x06] = 0x20;				// vocoder select as the init leaves it
	CHECK(hrc_prompt_on(&dev, true));
	CHECK(regs[4][0x06] == 0x22);
	CHECK(hrc_prompt_on(&dev, false));
	CHECK(regs[4][0x06] == 0x20);
	regs[4][0x06] = 0xFF;
	CHECK(hrc_prompt_on(&dev, false));
	CHECK(regs[4][0x06] == 0xFD);			// only bit 1 changes

	regs[4][0x88] = 0x00;
	CHECK(hrc_prompt_room(&dev));
	regs[4][0x88] = 0x01;
	CHECK(!hrc_prompt_room(&dev));			// bit 0: full
	regs[4][0x88] = 0xFE;
	CHECK(hrc_prompt_room(&dev));			// the other bits don't matter

	int16_t s[HRC_PROMPT_CHUNK];
	for (int i = 0; i < HRC_PROMPT_CHUNK; i++) {
		s[i] = (int16_t)(i == 0 ? -2 : 0x0102 * i);
	}
	page3_writes = 0;
	CHECK(hrc_prompt_write(&dev, s));
	CHECK(page3_writes == 1);			// one burst
	CHECK(regs[3][0x00] == 0xFF && regs[3][0x01] == 0xFE);	// -2, high byte first
	CHECK(regs[3][0x02] == 0x01 && regs[3][0x03] == 0x02);
	CHECK(regs[3][0x3E] == (uint8_t)((0x0102 * 31) >> 8) && regs[3][0x3F] == (uint8_t)(0x0102 * 31));
	CHECK(regs[3][0x40] == 0);			// nothing past 32 samples

	printf("promptFifoTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
