// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// One split duplex radio as a process (CONFIG_HOTSPOT_SPLIT): the MMDVM core (mmdvm.c, hotspot.c, hotspotSplit.c) on
// the simulated split modem (split_sim_modem.c), driven over stdin / stdout by tests/host/splitPairTest.py, which runs
// two of them in lockstep virtual time as the TX and RX halves of one repeater and plays the host between them
// (tools/split_duplex_host.py's time sync and relays). The core is a singleton, hence one process per radio.
//
// Requests, one per line; every request is answered by its output lines and then a line ".":
//   s <modem_us>                         set the modem clock (first, before anything else)
//   t <modem_us>                         advance the modem clock (u32, wraps) to modem_us, polling every 5 ms
//   h <hex>                              one MMDVM frame from the host, taken now
//   r <kind> <index> <t_start> <seed> <lc 18 hex>
//                                        an inbound burst whose slot started at t_start (modem clock), reported now
//                                        (voice: 27 payload bytes of seed; header / terminator: the LC)
//   q                                    quit
// Output lines:
//   o <modem_us> <hex>                   a frame the radio wrote to the host
//   a <counter> <t_us> <ts> <kind> <index> <at> <tc> <lcss> <bits> <underrun> <payload hex>
//                                        an outbound slot on air (TX half)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"
#include "hotspot/hotspotSplit.h"
#include "split_sim_modem.h"

static uint32_t poll_us;	// the modem clock at the last poll

static void io_write(const uint8_t *buf, size_t len)
{
	printf("o %u ", (unsigned)ssim.now_us);
	for (size_t i = 0; i < len; i++) {
		printf("%02X", buf[i]);
	}
	printf("\n");
}

static const mmdvm_io_t io = { .write = io_write, .description = "OpenDigitalRadio SplitSim 00000000" };

static bool r_enter(uint32_t rx) { (void)rx; return true; }
static bool r_set_freq(uint32_t rx, uint32_t tx) { (void)rx; (void)tx; return true; }
static void r_set_power(uint8_t level) { (void)level; }
static void r_leave(void) {}
static const hotspot_radio_ops_t radio_ops = { .enter = r_enter, .set_freq = r_set_freq, .set_power = r_set_power,
	.leave = r_leave };

static uint32_t ms_clock = 1000;	// the core's millisecond clock (its own counter: the modem clock wraps)

static uint32_t now_ms(void)
{
	return ms_clock;
}

static void flush_air(void)
{
	for (unsigned i = 0; i < ssim.air_n; i++) {
		const ssim_air_t *a = &ssim.air[i];

		printf("a %u %u %d %u %u %u %u %u %06X %u ", (unsigned)a->counter, (unsigned)a->t_us, a->ts, a->b.kind,
		       a->b.voice_index, a->cach.at, a->cach.tc, a->cach.lcss, (unsigned)(a->cach.bits & 0xFFFFFFu),
		       a->underrun ? 1u : 0u);
		for (unsigned k = 0; k < a->b.len && k < DMR_PAYLOAD_MAX; k++) {
			printf("%02X", a->b.payload[k]);
		}
		printf("-\n");
	}
	ssim.air_n = 0;
}

static void advance(uint32_t t)
{
	while ((int32_t)(t - poll_us) >= 5000) {
		poll_us += 5000u;
		ms_clock += 5u;
		ssim_advance_to(poll_us);
		mmdvm_poll(now_ms());
		flush_air();
	}
	ssim_advance_to(t);
	flush_air();
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while ((n < cap) && (hexval(s[0]) >= 0) && (hexval(s[1]) >= 0)) {
		out[n++] = (uint8_t)((hexval(s[0]) << 4) | hexval(s[1]));
		s += 2;
	}
	return n;
}

int main(void)
{
	char line[1024];

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	ssim_reset();
	poll_us = ssim.now_us;
	hotspot_init(&ssim_modem, mmdvm_send);
	hotspot_set_radio(&radio_ops);
	mmdvm_init(&io);
	hotspot_split_init();
	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (line[0] == 'q') {
			break;
		}
		if (line[0] == 's') {
			ssim.now_us = (uint32_t)strtoul(line + 2, NULL, 10);
			poll_us = ssim.now_us;
		} else if (line[0] == 't') {
			advance((uint32_t)strtoul(line + 2, NULL, 10));
		} else if (line[0] == 'h') {
			uint8_t f[MMDVM_FRAME_MAX];
			size_t n = unhex(line + 2, f, sizeof(f));

			if (n >= 3) {
				mmdvm_frame(f, (uint16_t)n, now_ms());
			}
			flush_air();
		} else if (line[0] == 'r') {
			unsigned kind, index, seed;
			unsigned long t;
			char lch[32] = { 0 };
			dmr_burst_t b;

			memset(&b, 0, sizeof(b));
			if (sscanf(line + 2, "%u %u %lu %u %31s", &kind, &index, &t, &seed, lch) == 5) {
				b.kind = (uint8_t)kind;
				b.voice_index = (uint8_t)index;
				b.crc_ok = true;
				b.in_step = true;
				b.rssi_dbm = -80;
				if (kind == DMR_BURST_VOICE) {
					b.len = DMR_VOICE_BYTES;
					memset(b.payload, (int)seed, DMR_VOICE_BYTES);
					if (index == 5) {
						b.lc_valid = unhex(lch, b.lc, 9) == 9;
					}
				} else {
					b.len = 12;
					b.lc_valid = unhex(lch, b.lc, 9) == 9;
				}
				ssim_rx_burst(&b, (uint32_t)t);
			}
		}
		printf(".\n");
		fflush(stdout);
	}
	return 0;
}
