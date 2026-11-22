// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fusion's Voice FR codec in the radio core (radio_ysf_vw.h)
#include <string.h>
#include "radio/radio_ysf_vw.h"

#if CONFIG_YSF_VW

#if YSF_VW_IMBE_BYTES != IMBE_BYTES
#error "a VW frame is the full-rate codec's 88 bits"
#endif

// The memory is the radio core's mode area (radio_ysf_vw_bind, while Fusion runs): the views of it the codec's calls
// take are built at the bind, so a call never looks the area up
static imbe_mem_t dec;
static imbe_enc_mem_t enc;
static bool bound;

void radio_ysf_vw_bind(radio_ysf_vw_mem_t *m)
{
	bound = m != NULL;
	if (!bound) {
		return;
	}
	dec = (imbe_mem_t){ &m->st, &m->pr, { &m->ms, &m->prev, &m->cur, m->buf } };
	enc = (imbe_enc_mem_t){ &m->st.q, &m->est, &m->pr, m->buf };
}

const uint8_t *const radio_ysf_vw_silence = imbe_silence;

void radio_ysf_vw_rx_start(void)
{
	if (bound) {
		imbe_dec_init(&dec);
	}
}

int radio_ysf_vw_decode(const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs, int16_t pcm[160])
{
	if (!bound) {
		memset(pcm, 0, 160 * sizeof pcm[0]);
		return 1;
	}
	return imbe_dec_frame(&dec, imbe, errs, pcm);
}

void radio_ysf_vw_lost(int16_t pcm[160])
{
	if (!bound) {
		memset(pcm, 0, 160 * sizeof pcm[0]);
		return;
	}
	imbe_dec_frame(&dec, imbe_silence, IMBE_ERRS_LOST, pcm);
}

void radio_ysf_vw_tx_start(void)
{
	if (bound) {
		imbe_enc_init(&enc);
	}
}

void radio_ysf_vw_encode(const int16_t pcm[160], uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	if (!bound) {
		memcpy(imbe, imbe_silence, YSF_VW_IMBE_BYTES);
		return;
	}
	imbe_enc_frame(&enc, pcm, imbe);
}

#endif
