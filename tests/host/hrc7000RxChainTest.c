// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Register model -> C7000 engine -> board event drain -> call stack -> vocoder input.
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_dmr.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"

void dlog(const char *fmt, ...) { }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static uint32_t regs[0x600 / 4], now;
static uint8_t buf[0xA00], lc[2][9];
static hrc7000_t dev;
static hrc7000_dmr_t eng;
static dmr_event_cb_t callback;
static void *callback_ctx;
static unsigned frames[2], wrong_caller, lost;
static uint32_t wanted_src;
static uint8_t family, chip_flip;
#define REG(off) regs[(off) / 4]

static uint32_t time_us(void *ctx) { return now; }
// Arrays do not implement write-one-to-clear; the event notification clears the modeled IRQ.
static void notify(void *ctx) { REG(C7_SYS_INTERP_LIST) = 0; }
static const hrc7000_dmr_ops_t ops = { .now_us = time_us, .notify = notify };
static void configure(const dmr_config_t *cfg) { hrc7000_dmr_configure(&eng, cfg); }
static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	configure(cfg);
	callback = cb;
	callback_ctx = ctx;
	return true;
}
static void stop(void) { callback = NULL; }
static void cc_filter(int8_t cc) { hrc7000_dmr_set_cc_filter(&eng, cc); }
static void slot_filter(int8_t ts) { hrc7000_dmr_rx_lock_slot(&eng, ts); }
static const hal_dmr_modem_t modem = {
	.start = start, .stop = stop, .configure = configure, .set_cc_filter = cc_filter, .rx_lock_slot = slot_filter,
};
static void frame(const uint8_t f[9], void *ctx)
{
	CHECK(f[0] == 0x11 || f[0] == 0x22);
	frames[f[0] == 0x22]++;
}
static void begin(const dmr_caller_t *c, void *ctx) { wrong_caller += c->src != wanted_src; }
static void conceal(void *ctx) { lost++; }
static const dmr_voice_sink_t sink = { .begin = begin, .frame = frame, .lost = conceal };

static void drain(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc7000_dmr_pop(&eng, &ev, &b))
		callback(&ev, callback_ctx);
	dmr_call_poll(now / 1000);
	const dmr_call_view_t *v = dmr_call_view();
	if (v->state == DMR_CALL_STATE_RX && v->caller.src != 0)
		wrong_caller += v->caller.src != wanted_src;
}

static uint32_t info(unsigned ts, unsigned vb, unsigned kind)
{
	return vb << 12 | kind << 8 | 1u << 4 | (uint32_t)family << 2 | (vb ? (vb == 1 ? 1u : 0u) : 2u) |
	       ((ts ^ chip_flip) << 19) | ts << 22;
}
static void interrupt(uint32_t status, uint32_t flags)
{
	REG(C7_RX_TYPE_INFO) = status;
	REG(C7_SYS_INTERP_LIST) = flags;
	hrc7000_dmr_isr_sys(&eng);
	REG(C7_SYS_INTERP_LIST) = 0;
	drain();
}
static void tick(unsigned ts)
{
	now += 30000;
	REG(C7_LAYER2_STATUS) = (ts ^ chip_flip) << 30;
	hrc7000_dmr_isr_timeslot(&eng, false);
	drain();
}
static void header(unsigned ts)
{
	tick(ts);
	dmr_lc_encode_full(lc[ts], DMR_RS_MASK_VOICE_HEADER, &buf[C7_RX_RAM]);
	interrupt(info(ts, 0, DMR_BURST_VOICE_LC_HEADER), 1u << 3);
}
static void embedded(unsigned ts)
{
	memcpy(&buf[C7_RX_RAM], lc[ts], 9);
	interrupt(info(ts, 6, 0), 1u << 2);
}
static void voice(unsigned ts, unsigned index, bool with_lc)
{
	tick(ts);
	memset(&buf[C7_VOICE_RAM], ts ? 0x22 : 0x11, 27);
	if (with_lc)
		memcpy(&buf[C7_RX_RAM], lc[ts], 9);
	interrupt(info(ts, index % 6 + 1, 0), (1u << 3) | (with_lc ? 1u << 2 : 0));
}
static void setup(unsigned selected, bool direct, bool monitor, unsigned flip)
{
	memset(regs, 0, sizeof regs);
	memset(buf, 0, sizeof buf);
	memset(frames, 0, sizeof frames);
	wrong_caller = lost = 0;
	now = 1000000;
	family = direct ? 0 : 1;
	chip_flip = (uint8_t)flip;
	wanted_src = selected ? 222 : 111;
	for (unsigned ts = 0; ts < 2; ts++) {
		dmr_lc_t l = { .src = ts ? 222 : 111, .dst = monitor ? 92 : 91 };
		dmr_lc_pack(&l, lc[ts]);
	}
	hrc7000_attach(&dev, regs, buf);
	hrc7000_dmr_init(&eng, &dev, &ops);
	hrc7000_dmr_run(&eng);
	dmr_call_init(&modem);
	dmr_call_set_voice(&sink, NULL);
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.rmo = !direct;
	cfg.ts = (uint8_t)(selected + 1);
	cfg.talk_target.id = 91;
	cfg.listen.hear_all = monitor;
	dmr_call_configure(&cfg);
	CHECK(dmr_call_start());
}

static void two_slots(unsigned selected, bool late, bool monitor, unsigned flip)
{
	setup(selected, false, monitor, flip);
	unsigned other = selected ^ 1u;
	if (!late)
		header(selected);
	for (unsigned n = 0; n < 18; n++) {
		// TS2 starts mid-TS1-call; on late entry it is already sending when TS1's LC arrives.
		if (n == (late ? 0u : 6u))
			header(other);
		if (late && n == 0) {
			embedded(selected);
			// Both reports arrive before the next selected burst: neither LC may overwrite the other.
			embedded(other);
		}
		voice(selected, n, false);
		if (late || n >= 6) {
			voice(other, n, n % 6 == 5);
		} else {
			tick(other);
			interrupt(info(other, 0, DMR_BURST_IDLE), 1u << 3);
		}
	}
	printf("  TS%u %s monitor=%u chip_flip=%u: wanted=%u other=%u wrong_caller=%u lost=%u\n",
	       selected + 1, late ? "late" : "header", monitor, flip, frames[selected], frames[other], wrong_caller, lost);
	CHECK(frames[selected] == 54);
	CHECK(frames[other] == 0);
	CHECK(wrong_caller == 0);
	CHECK(lost == 0);
	CHECK(dmr_call_view()->caller.src == wanted_src);
	dmr_call_stop();
}

int main(void)
{
	for (unsigned ts = 0; ts < 2; ts++)
		for (unsigned flip = 0; flip < 2; flip++)
			for (unsigned late = 0; late < 2; late++)
				for (unsigned monitor = 0; monitor < 2; monitor++)
					two_slots(ts, late, monitor, flip);
	// Direct mode has no CACH: retain chip-slot association and late entry.
	setup(0, true, false, 0);
	embedded(0);
	voice(0, 0, false);
	voice(0, 1, false);
	CHECK(frames[0] == 6 && frames[1] == 0 && wrong_caller == 0);
	dmr_call_stop();
	printf("hrc7000RxChainTest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
