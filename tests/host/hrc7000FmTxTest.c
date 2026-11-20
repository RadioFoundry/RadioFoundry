// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 flat FM paths (drivers/modem/hrc7000/hrc7000.c, docs/spec/modem-hrc7000.md 8.6) on plain arrays standing in
// for the modem registers and buffers: the TX RAM half writer (byte order, the half FM_ADDR_SW names or the other, the
// other half untouched, the pulls in chunks), the FM TX interrupt's mask bit and its dispatch from the system
// interrupt, and the flat route's FM_BANDWIDTH and AUDIO_CONTROL bits set and restored. Built with CONFIG_DSTAR=1, as
// the dispatch is only in builds with a flat transmit path
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_dmr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t regs[0x600 / 4];
static uint8_t buf[0xA00];
static hrc7000_t dev;
static hrc7000_dmr_t eng;

#define REG(off)	regs[(off) / 4]
#define FILL		0xA5u	// what the buffers hold before a write

static void op_rf(void *ctx, bool tx) {}
static void op_pa_arm(void *ctx) {}
static uint32_t op_now(void *ctx) { return 0; }
static void op_lock(void *ctx) {}
static void op_unlock(void *ctx) {}
static void op_notify(void *ctx) {}

static const hrc7000_dmr_ops_t ops = {
	.rf = op_rf, .pa_arm = op_pa_arm, .now_us = op_now, .lock = op_lock, .unlock = op_unlock, .notify = op_notify,
};

static void setup(void)
{
	memset(regs, 0, sizeof regs);
	memset(buf, FILL, sizeof buf);
	hrc7000_attach(&dev, regs, buf);
	hrc7000_dmr_init(&eng, &dev, &ops);
}

// A pull that counts up from next, one sample at a time, and records the chunk sizes asked for
static int16_t next;
static int pulls;
static uint16_t pull_n_max, pull_n_min;

static void pull_count(int16_t *pcm, uint16_t n)
{
	pulls++;
	pull_n_max = n > pull_n_max ? n : pull_n_max;
	pull_n_min = n < pull_n_min ? n : pull_n_min;
	for (uint16_t i = 0; i < n; i++) {
		pcm[i] = next++;
	}
}

static void pull_reset(int16_t from)
{
	next = from;
	pulls = 0;
	pull_n_max = 0;
	pull_n_min = 0xFFFF;
}

static int16_t half_sample(uint32_t half, uint32_t i)
{
	const uint8_t *p = &buf[C7_FM_TX_AUDIO + half * 512u + 2u * i];
	return (int16_t)(uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static bool half_untouched(uint32_t half)
{
	for (uint32_t i = 0; i < 512u; i++) {
		if (buf[C7_FM_TX_AUDIO + half * 512u + i] != FILL) {
			return false;
		}
	}
	return true;
}

// The datasheet's 0x16000030..0x1600042F; nothing written outside it
static void test_layout(void)
{
	CHECK(C7_FM_TX_AUDIO == 0x30u && C7_FM_TX_AUDIO + 2u * 2u * HRC7000_FM_TX_SAMPLES == 0x430u);
	CHECK(HRC7000_FM_TX_SAMPLES % HRC7000_FM_TX_CHUNK == 0);
	setup();
	pull_reset(0);
	hrc7000_fm_tx_audio_fill(&dev, 0, pull_count);
	hrc7000_fm_tx_audio_fill(&dev, 1, pull_count);
	for (uint32_t i = 0; i < C7_FM_TX_AUDIO; i++) {
		CHECK(buf[i] == FILL);
	}
	for (uint32_t i = 0x430u; i < sizeof buf; i++) {
		if (buf[i] != FILL) {
			CHECK(buf[i] == FILL);
			break;
		}
	}
}

// High byte at the lower address, signed values whole, the first half then the second
static void test_fill_byte_order(void)
{
	setup();
	pull_reset(-128);
	hrc7000_fm_tx_audio_fill(&dev, 0, pull_count);
	CHECK(buf[C7_FM_TX_AUDIO] == 0xFFu && buf[C7_FM_TX_AUDIO + 1u] == 0x80u);	// -128 = 0xFF80
	CHECK(half_sample(0, 0) == -128 && half_sample(0, 128) == 0 && half_sample(0, 255) == 127);
	CHECK(half_untouched(1));

	setup();
	pull_reset(0x1234);
	hrc7000_fm_tx_audio_fill(&dev, 1, pull_count);
	CHECK(buf[C7_FM_TX_AUDIO + 512u] == 0x12u && buf[C7_FM_TX_AUDIO + 513u] == 0x34u);
	CHECK(half_sample(1, 255) == 0x1234 + 255);
	CHECK(half_untouched(0));

	setup();	// the extremes survive the split into bytes
	int16_t v[2] = { -32768, 32767 };
	for (int k = 0; k < 2; k++) {
		pull_reset(v[k]);
		next = v[k];
		hrc7000_fm_tx_audio_fill(&dev, 0, pull_count);
		CHECK(half_sample(0, 0) == v[k]);
	}
}

// A half is pulled in chunks of HRC7000_FM_TX_CHUNK, in order, with nothing lost between chunks
static void test_fill_chunks(void)
{
	setup();
	pull_reset(0);
	hrc7000_fm_tx_audio_fill(&dev, 0, pull_count);
	CHECK(pulls == (int)(HRC7000_FM_TX_SAMPLES / HRC7000_FM_TX_CHUNK));
	CHECK(pull_n_max == HRC7000_FM_TX_CHUNK && pull_n_min == HRC7000_FM_TX_CHUNK);
	bool in_order = true;
	for (uint32_t i = 0; i < HRC7000_FM_TX_SAMPLES; i++) {
		in_order = in_order && half_sample(0, i) == (int16_t)i;
	}
	CHECK(in_order);
}

// The half FM_ADDR_SW names (1 the first, 2 the second), or the other one; neither for 0 or 3
static void test_due(void)
{
	setup();
	REG(C7_FM_ADDR_SW) = 0;
	CHECK(hrc7000_fm_tx_audio_due(&dev, false) == -1 && hrc7000_fm_tx_audio_due(&dev, true) == -1);
	REG(C7_FM_ADDR_SW) = 1;
	CHECK(hrc7000_fm_tx_audio_due(&dev, false) == 0 && hrc7000_fm_tx_audio_due(&dev, true) == 1);
	REG(C7_FM_ADDR_SW) = 2;
	CHECK(hrc7000_fm_tx_audio_due(&dev, false) == 1 && hrc7000_fm_tx_audio_due(&dev, true) == 0);
	REG(C7_FM_ADDR_SW) = 3;
	CHECK(hrc7000_fm_tx_audio_due(&dev, false) == -1);
	REG(C7_FM_ADDR_SW) = 0xFFFFFF01u;	// only bits 1:0 count
	CHECK(hrc7000_fm_tx_audio_due(&dev, false) == 0);
}

// The mask bit alone, the power-on enables and the FM receive bit kept
static void test_irq_mask(void)
{
	setup();
	REG(C7_SYS_INTERP_MASK) = 0x1187Fu | C7_SYS_FM_RX;
	hrc7000_fm_tx_audio_irq(&dev, true);
	CHECK(REG(C7_SYS_INTERP_MASK) == (0x1187Fu | C7_SYS_FM_RX | C7_SYS_FM_TX));
	hrc7000_fm_tx_audio_irq(&dev, false);
	CHECK(REG(C7_SYS_INTERP_MASK) == (0x1187Fu | C7_SYS_FM_RX));
}

static int tx_hook_calls, rx_hook_calls;
// The chip clears the list on the write back; an array can't, so the hooks do
static void tx_hook(void) { tx_hook_calls++; REG(C7_SYS_INTERP_LIST) = 0; }
static void rx_hook(void) { rx_hook_calls++; REG(C7_SYS_INTERP_LIST) = 0; }

// Bit 15 reaches the FM TX hook with the engine stopped (FM), bit 14 the FM RX hook, each only its own
static void test_dispatch(void)
{
	setup();
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_TX;
	hrc7000_dmr_isr_sys(&eng);	// no hook: cleared, nothing else
	CHECK(REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_TX);

	tx_hook_calls = rx_hook_calls = 0;
	eng.fm_tx = tx_hook;
	eng.fm_rx = rx_hook;
	REG(C7_SYS_INTERP_CLEAR) = 0;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_TX;
	hrc7000_dmr_isr_sys(&eng);
	CHECK(tx_hook_calls == 1 && rx_hook_calls == 0 && REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_TX);
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_dmr_isr_sys(&eng);
	CHECK(tx_hook_calls == 1 && rx_hook_calls == 1);
	REG(C7_SYS_INTERP_LIST) = 1u << 5;
	hrc7000_dmr_isr_sys(&eng);	// another bit: neither
	CHECK(tx_hook_calls == 1 && rx_hook_calls == 1);
}

// FM transmit at both bandwidths: the band-pass and pre-emphasis bits clear and the TX RAM source on, then everything
// as hrc7000_fm_tx left it. The AUDIO_CONTROL shadow follows, and the other audio bits stay
static void test_flat_tx_route(void)
{
	for (int wide = 0; wide < 2; wide++) {
		setup();
		hrc7000_fm_tx(&dev, wide != 0);
		uint32_t bw = REG(C7_FM_BANDWIDTH);
		CHECK(bw == (wide ? 0x5Fu : 0x50u));
		hrc7000_audio_bits(&dev, C7_AUDIO_PCM_MIC, 0);	// the mic PCM stream runs meanwhile (the voice codec's input)
		uint32_t ac = REG(C7_AUDIO_CONTROL);
		CHECK((ac & C7_AUDIO_TX_RAM) == 0);

		hrc7000_fm_flat_tx(&dev, HRC7000_FM_FLAT_TX_CLR, true);
		CHECK(REG(C7_FM_BANDWIDTH) == (bw & ~(C7_FM_BW_BANDPASS | C7_FM_BW_PREEMPH)));
		CHECK(REG(C7_AUDIO_CONTROL) == (ac | C7_AUDIO_TX_RAM) && dev.audio_ctl == REG(C7_AUDIO_CONTROL));
		CHECK((REG(C7_AUDIO_CONTROL) & (C7_AUDIO_RING_PLAY | C7_AUDIO_FM_PLAY)) == 0);	// nothing else on air

		hrc7000_fm_flat_tx(&dev, HRC7000_FM_FLAT_TX_CLR, false);
		CHECK(REG(C7_FM_BANDWIDTH) == bw);
		CHECK(REG(C7_AUDIO_CONTROL) == ac && dev.audio_ctl == ac);
	}

	setup();	// a subset (the bench keeps the pre-emphasis bit): only it changes
	hrc7000_fm_tx(&dev, false);
	hrc7000_fm_flat_tx(&dev, C7_FM_BW_BANDPASS, true);
	CHECK(REG(C7_FM_BANDWIDTH) == 0x10u);
	hrc7000_fm_flat_tx(&dev, C7_FM_BW_BANDPASS, false);
	CHECK(REG(C7_FM_BANDWIDTH) == 0x50u);
}

// FM receive: the band-pass bit only; the FM audio bit and the source untouched
static void test_flat_rx_route(void)
{
	for (int wide = 0; wide < 2; wide++) {
		setup();
		hrc7000_fm_rx(&dev, wide != 0);
		uint32_t bw = REG(C7_FM_BANDWIDTH), ac = REG(C7_AUDIO_CONTROL);
		CHECK(bw == (wide ? 0x4Fu : 0x40u));
		hrc7000_fm_flat_rx(&dev, HRC7000_FM_FLAT_RX_CLR, true);
		CHECK(REG(C7_FM_BANDWIDTH) == (wide ? 0x0Fu : 0x00u) && REG(C7_AUDIO_CONTROL) == ac);
		hrc7000_fm_flat_rx(&dev, HRC7000_FM_FLAT_RX_CLR, false);
		CHECK(REG(C7_FM_BANDWIDTH) == bw && REG(C7_AUDIO_CONTROL) == ac);
	}
}

// A transmission's start as the platform runs it: both halves primed in order, then each interrupt fills the half
// it names, the other left as it was
static void test_halves_in_turn(void)
{
	setup();
	pull_reset(0);
	hrc7000_fm_tx_audio_fill(&dev, 0, pull_count);
	hrc7000_fm_tx_audio_fill(&dev, 1, pull_count);
	CHECK(half_sample(0, 0) == 0 && half_sample(1, 0) == 256 && half_sample(1, 255) == 511);
	REG(C7_FM_ADDR_SW) = 1;
	int h = hrc7000_fm_tx_audio_due(&dev, false);
	CHECK(h == 0);
	hrc7000_fm_tx_audio_fill(&dev, (uint32_t)h, pull_count);
	CHECK(half_sample(0, 0) == 512 && half_sample(0, 255) == 767);
	CHECK(half_sample(1, 0) == 256 && half_sample(1, 255) == 511);
	REG(C7_FM_ADDR_SW) = 2;
	h = hrc7000_fm_tx_audio_due(&dev, false);
	hrc7000_fm_tx_audio_fill(&dev, (uint32_t)h, pull_count);
	CHECK(half_sample(1, 0) == 768 && half_sample(0, 0) == 512);
}

int main(void)
{
	test_layout();
	test_fill_byte_order();
	test_fill_chunks();
	test_due();
	test_irq_mask();
	test_dispatch();
	test_flat_tx_route();
	test_flat_rx_route();
	test_halves_in_turn();
	if (failures) {
		printf("hrc7000FmTxTest: %d failures\n", failures);
		return 1;
	}
	printf("hrc7000FmTxTest: all passed\n");
	return 0;
}
