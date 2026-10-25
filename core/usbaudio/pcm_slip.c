// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PCM jitter buffer with sample slipping between two 8 kHz clocks (pcm_slip.h)
#include "pcm_slip.h"

#include <string.h>

#define MASK		(PCM_SLIP_CAP - 1u)
#define AVG_SHIFT	4	// fill smoothing: 1/16 per pull (about 320 ms at 20 ms pulls)

void pcm_slip_init(pcm_slip_t *p, uint16_t target, uint16_t block)
{
	memset(p, 0, sizeof *p);
	if (block == 0) {
		block = 1;
	}
	if ((uint32_t)target + 2u * block > PCM_SLIP_CAP) {
		target = (uint16_t)(PCM_SLIP_CAP - 2u * block);
	}
	p->target = target;
	p->block = block;
}

void pcm_slip_reset(pcm_slip_t *p)
{
	p->rd = p->wr;
	p->primed = false;
}

uint32_t pcm_slip_fill(const pcm_slip_t *p)
{
	return p->wr - p->rd;
}

void pcm_slip_push(pcm_slip_t *p, const int16_t *pcm, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p->wr - p->rd >= PCM_SLIP_CAP) {
			p->rd++;	// full: the oldest goes
			p->stats.overflow++;
		}
		p->buf[p->wr & MASK] = pcm[i];
		// The sample is in place before the consumer can see it (an interrupt pulling while this pushes)
		__atomic_signal_fence(__ATOMIC_RELEASE);
		p->wr++;
	}
	p->stats.pushed += (uint32_t)n;
}

bool pcm_slip_pull(pcm_slip_t *p, int16_t *out, size_t n)
{
	uint32_t fill = pcm_slip_fill(p);
	p->stats.pulled += (uint32_t)n;
	if (!p->primed) {
		if (fill < p->target) {
			memset(out, 0, n * sizeof *out);
			return false;
		}
		p->primed = true;
		p->stats.primes++;
		p->avg_x16 = (int32_t)fill << AVG_SHIFT;
	}
	if (fill < n) {
		// Ran dry: what is left, then silence, and prime again
		size_t i = 0;
		for (; i < fill; i++) {
			out[i] = p->buf[p->rd++ & MASK];
		}
		memset(out + i, 0, (n - i) * sizeof *out);
		p->primed = false;
		p->stats.underruns++;
		return false;
	}

	p->avg_x16 += (int32_t)fill - (p->avg_x16 >> AVG_SHIFT);
	int32_t avg = p->avg_x16 >> AVG_SHIFT;
	// Between pushes and pulls the fill swings by about a block above the target; outside that, slip one sample
	int slip = 0;
	if (avg > (int32_t)p->target + (int32_t)p->block + (int32_t)p->block / 2 && fill > n) {
		slip = 1;	// drop one
	} else if (avg < (int32_t)p->target - (int32_t)p->block / 2) {
		slip = -1;	// repeat one
	}

	size_t mid = n / 2;
	for (size_t i = 0; i < n; i++) {
		if (slip == 1 && i == mid) {
			// Drop: blend the two samples into one so the waveform has no step
			int32_t a = p->buf[p->rd++ & MASK];
			int32_t b = p->buf[p->rd++ & MASK];
			out[i] = (int16_t)((a + b) / 2);
			continue;
		}
		if (slip == -1 && i == mid) {
			// Repeat: the next sample twice (not consumed this time)
			out[i] = p->buf[p->rd & MASK];
			continue;
		}
		out[i] = p->buf[p->rd++ & MASK];
	}
	if (slip == 1) {
		p->stats.drops++;
		p->avg_x16 -= 1 << AVG_SHIFT;
	} else if (slip == -1) {
		p->stats.repeats++;
		p->avg_x16 += 1 << AVG_SHIFT;
	}
	return true;
}
