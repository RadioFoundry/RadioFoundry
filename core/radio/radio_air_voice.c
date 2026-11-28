// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What NXDN and YSF share on the radio core's side (radio_air.h): the DMR side's voice path, the engine's memory and
// its counters
#include "config.h"
#if CONFIG_NXDN || CONFIG_YSF	// the whole file
#include <stdint.h>
#include "radio/radio_air.h"
#include "dmr/ambe/ambeVoice.h"

void radio_air_voice_rx_start(void)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	s->begin(NULL, s->ctx);
}

void radio_air_voice_rx_frame(const uint8_t *v)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	s->frame(v, s->ctx);
}

void radio_air_voice_rx_end(void)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	s->end(s->ctx);
}

bool radio_air_voice_tx_next(uint8_t *v)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	return s->next(v, s->ctx);
}

void radio_air_voice_tx(bool on)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	if (on)
		s->begin(s->ctx);
	else
		s->end(s->ctx);
}

void *radio_air_engine_get(size_t need)
{
	size_t len = 0;
	void *m = radio_air_engine_mem(&len);

	if (m == NULL || len < need || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
}

void radio_air_count(radio_air_counts_t *total, radio_air_counts_t *base, uint32_t rx_calls, uint32_t tx_calls,
		     uint32_t tx_filler, const air_session_stats_t *a)
{
	total->rx_calls += rx_calls - base->rx_calls;
	total->tx_calls += tx_calls - base->tx_calls;
	total->tx_filler += tx_filler - base->tx_filler;
	total->rx_chunks += a->rx_chunks - base->rx_chunks;
	total->tx_underruns += a->tx_underruns - base->tx_underruns;
	total->tx_syms += a->tx_syms - base->tx_syms;
	base->rx_calls = rx_calls;
	base->tx_calls = tx_calls;
	base->tx_filler = tx_filler;
	base->rx_chunks = a->rx_chunks;
	base->tx_underruns = a->tx_underruns;
	base->tx_syms = a->tx_syms;
}
#endif	// CONFIG_NXDN || CONFIG_YSF
