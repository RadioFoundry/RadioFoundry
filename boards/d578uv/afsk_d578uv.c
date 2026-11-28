// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV APRS (hal_afsk.h). Transmit: the tones on PB9 into the RF chip's audio input (tone_d578uv.c). Receive
// (CONFIG_APRS_RX): the demodulated receive audio on PC0, sampled at 9600 Hz (eight samples a bit at 1200 baud) by the
// receive tap (rxtap_d578uv.c) for the core's AFSK demodulator.
//
// The tap runs only between the core's rx_start and rx_stop, which it calls while the radio receives FM on the APRS
// frequency. Readings reach the core only while side A is receiving FM: while it transmits or side B has the AM
// receiver, the tap's ring is emptied instead. That PC0 still carries side A's audio while side B also receives is not
// known.
#include "d578uv.h"
#include "config.h"

#if CONFIG_APRS
#if CONFIG_APRS_RX
#define SAMPLE_HZ		9600u

static bool rx_start(void)
{
	return d578uv_rxtap_start(SAMPLE_HZ);
}

static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	return d578uv_rxtap_read(buf, max, RADIO_MODE_FM);
}

#endif	// CONFIG_APRS_RX

const hal_afsk_t d578uv_afsk = {
	.tx_start = d578uv_afsk_tx_start,
	.tx_busy = d578uv_afsk_tx_busy,
	.tx_stop = d578uv_afsk_tx_stop,
#if CONFIG_APRS_RX
	.rx_rate_hz = SAMPLE_HZ,
	.rx_start = rx_start,
	.rx_read = rx_read,
	.rx_stop = d578uv_rxtap_stop,
#endif
};

#endif	// CONFIG_APRS
