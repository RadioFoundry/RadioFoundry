// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the MBE front ends' quantizers share (mbe.h): the prediction of a frame's log amplitudes from the last frame's.
#include "mbe.h"
#include "../dsp/fxp.h"

void mbe_predict(mbe_pred_t *s, int L, int32_t rho, int32_t pr[MBE_MAXL + 1])
{
	int32_t rcp, sum = 0, S;
	int rem = 0, ik = 0;

	if (L > s->L)
		for (int l = s->L + 1; l <= L; l++)
			s->lg[l] = s->lg[s->L];
	rcp = (int32_t)((1u << 30) / (uint32_t)L);
	for (int l = 1; l <= L; l++) {
		int32_t p0, p1, d;

		rem += s->L;
		while (rem >= L) {
			rem -= L;
			ik++;
		}
		p0 = s->lg[ik];
		p1 = s->lg[ik + 1];
		d = fxp_mulshr(rem, rcp, 15);		// rem / L, Q15 (rem < L: rem * rcp < 2^30)
		pr[l] = p0 * 32 + fxp_mulshr((p1 - p0) * 32, d, 15);
		sum += pr[l];
	}
	S = fxp_mulshr(fxp_mulshr(sum, rho, 15), rcp, 30);
	for (int l = 1; l <= L; l++)
		pr[l] = fxp_mulshr(pr[l], rho, 15) - S;
}
