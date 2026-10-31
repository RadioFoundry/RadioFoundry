// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B: the second receiver's application (docs/spec/rx2-link.md, hardware-gd88.md 10). There is no radio core
// here: app_start (called by platform/hrc7000/main.c) starts one task that runs the link's receiver side (core/rx2) on
// UART3, the shared radio code (boards/gd88/common) on B's own BK4819 pair, the FM squelch and, on DMR channels, the
// call stack and codec (core/dmr). The calls rx2_ops_t makes all come from rx2_recv_rx() in this task, the debug link
// server's (core/rx2/rx2_dl.c, reached through A) among them. A second task keys the PA of A's transmitter
// (pa_gd88b.c, armed by RX2_TX).
//
// With GD88_DMR_TX B is also A's second DMR transmitter (RX2_DMR, core/rx2/rx2_dmr.h): for an SFR's repeat A goes on
// receiving while B sends the bursts A hands it on its own chip. The role stops B's own receiving (its call stack)
// and gives the modem to rx2_dmr.c; A's RX2_TUNE meanwhile is kept for when the role ends.
#include "config.h"
#include "board.h"
#include "gd88_rf.h"
#include "gd88_cal.h"
#include "gd88b.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#include "rx2/rx2_link.h"
#include "rx2/rx2_dl.h"
#if CONFIG_DMR && GD88_DMR_TX
#include "rx2/rx2_dmr.h"
#endif
#if CONFIG_DMR
#include "dmr_glue.h"
#endif
#include "debuglink/dlog.h"
#if CONFIG_CRASH_LOG
#include "crash/crash.h"
#include "hal_crash.h"
#include "app/watchdog.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define UART_FMT_RD(base, off)		REG32((base) + (off))
#define UART_FMT_WR(base, off, v)	(REG32((base) + (off)) = (v))
#include "uart_format.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#endif

#define TASK_STACK		384u	// words
#define TASK_PRIORITY		3
#define POLL_MS			10u
#define LINK_BAUD		115200u
#define DIPLEX0_UART3		0x6000u		// IO_DIPLEX0 bits 13 / 14: PTA13 / PTA14 are UART3 TXD / RXD
#define SQ_CLOSE_POLLS		15u		// FM: the squelch closes after this many closed readings (150 ms)
#define WD_STALL_MS		2000u		// the idle task feeds the watchdog only while the task runs this recently

// The first stage's long window (boot/gd88b): STAY at STAY_ADDR, then a jump to the stage
#define STAY_ADDR		0x0004FFF8u
#define STAY_MAGIC		0x53544159u
#define STAGE_UART_DIV		19u		// 36 MHz APB at the loader's clock / 16 / 115200 (boot/hrc7000/stage.c)
#define STAGE_ADDR		0x0300C000u
#define WDT_KEY			0x5ADA7200u

static rx2_recv_t link;
static rx2_tune_t cur;
static bool tuned;
static bool audio_on;
static uint8_t sq_closed;
static bool sq_open;
static volatile uint32_t alive_ms;

static void send(const uint8_t *buf, uint16_t len)
{
	hrc_link_uart_write(buf, len);
}

#if CONFIG_DMR
static bool dmr_on;
static uint32_t groups[RX2_GROUPS];

static void dmr_stop(void)
{
	if (dmr_on) {
		dmr_call_stop();
		ambeVoiceStop();
		dmr_on = false;
	}
}

// The call stack on the channel's color code and slot, playing calls to the talk group, the listen list or our ID
static bool dmr_start(const rx2_tune_t *t)
{
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = t->own_id;
	cfg.cc = (uint8_t)(t->color_code & 0x0Fu);
	cfg.ts = t->timeslot == 2u ? 2u : 1u;
	cfg.talk_target.id = t->talk_group;
	cfg.talk_target.private_call = false;
	memcpy(groups, t->groups, sizeof(groups));
	cfg.listen.groups = groups;
	cfg.listen.group_count = t->group_count <= RX2_GROUPS ? t->group_count : RX2_GROUPS;
	dmr_call_configure(&cfg);
	if (!dmr_on) {
		dmr_on = dmr_call_start();
	}
	return dmr_on;
}
#endif

#if CONFIG_DMR && GD88_DMR_TX
static bool split;		// A's second transmitter (rx2_dmr.h): the modem is rx2_dmr.c's
static int8_t split_cc;
#endif

static bool op_tune(const rx2_tune_t *t)
{
#if CONFIG_DMR && GD88_DMR_TX
	if (split) {
		cur = *t;	// taken when the role ends
		tuned = true;
		return true;
	}
#endif
	if (t->mode != RADIO_MODE_FM && (t->mode != RADIO_MODE_DMR || !CONFIG_DMR)) {
		return false;
	}
#if CONFIG_DMR
	if (t->mode != RADIO_MODE_DMR) {
		dmr_stop();
	}
#endif
	gd88_radio.audio(false);
	gd88_radio.set_mode((radio_mode_t)t->mode);
	if (!gd88_radio.rx(t->rx_hz, (radio_bw_t)t->bw, &t->rx_tone)) {
		tuned = false;
		return false;
	}
	cur = *t;
	sq_open = false;
	sq_closed = 0;
#if CONFIG_DMR
	if (t->mode == RADIO_MODE_DMR && !dmr_start(t)) {
		tuned = false;
		return false;
	}
#endif
	tuned = true;
	return true;
}

static void op_stop(void)
{
#if CONFIG_DMR && GD88_DMR_TX
	if (split) {
		tuned = false;
		return;
	}
#endif
#if CONFIG_DMR
	dmr_stop();
#endif
	gd88_radio.audio(false);
	gd88_path_rx(cur.rx_hz, false, false);
	tuned = false;
	sq_open = false;
}

static void op_audio(bool on, uint8_t volume)
{
	audio_on = on;
	gd88_radio.volume(volume);
	gd88b_path_mute(!on);
}

static void op_status(rx2_status_t *st)
{
	radio_status_t rs;
	gd88_radio.status(&rs);
	st->tuned = tuned;	// the link reports tuned only while this says so: without it A gave up on B after 1.5 s
	st->rssi_dbm = rs.rssi_dbm;
	st->noise = rs.noise;
	st->active = false;
	st->dmr = false;
	st->group = false;
	st->src = 0;
	st->dst = 0;
	if (!tuned) {
		return;
	}
#if CONFIG_DMR
	if (cur.mode == RADIO_MODE_DMR) {
		const dmr_call_view_t *v = dmr_call_view();
		if (v->phase == DMR_CALL_PHASE_RX_VOICE) {
			st->active = true;
			st->dmr = true;
			st->group = v->caller.group;
			st->src = v->caller.src;
			st->dst = v->caller.dst;
		}
		if (v->rssi_dbm != 0) {
			st->rssi_dbm = v->rssi_dbm;
		}
		return;
	}
#endif
	st->active = sq_open;
}

// Into the first stage's long window, for an update through A (rx2-link.md 4): from RAM with interrupts off, the
// transmitter off, the watchdog off and the clocks and flash controller as the loader left them, then the stage, which
// sets up UART3 at 115200 for the boot ROM's handshake again
static HRC_RAMFUNC __attribute__((noreturn)) void enter_stage(void)
{
	__asm__ volatile ("psrclr ie");
	hrc_rf_safe();
	PIC_MASK = 0xFFFFFFFFu;
	PIC_MASK_1 = 0xFFFFFFFFu;
	REG32(WDT_BASE + 0x00) = WDT_KEY;
	REG32(WDT_BASE + 0x10) = 0;
	REG32(WDT_BASE + 0x00) = 0;
	REG32(STAY_ADDR) = STAY_MAGIC;
#if HRC_LOADER_CPU_HZ != HRC_CPU_HZ
	hrc_clock_restore_loader();
#endif
	// UART3 at 115200 for the loader's clock: the stage keeps a divisor already set, and ours is for our clock, so its
	// handshake went out at the wrong speed and the host never heard it
	(void)hrc_uart_format(UART3_BASE, STAGE_UART_DIV);
	QUAD_ENABLE = 0;
	LCSFC_BAUDR = 4;
	hrc_barrier();
	((void (*)(void))(uintptr_t)STAGE_ADDR)();
	for (;;) {
	}
}

static void op_stub(void)
{
	vTaskDelay(pdMS_TO_TICKS(20));	// the link's last frame out
	enter_stage();
}

static void op_reset(void)
{
	vTaskDelay(pdMS_TO_TICKS(20));
	hrc_soft_reset();
}

#if CONFIG_DMR && GD88_DMR_TX
// ---- A's second transmitter (rx2_dmr.h) ----

static void tx_event(const dmr_event_t *ev, void *ctx)
{
	(void)ctx;
	rx2_dmr_recv_event(ev);
}

static bool tx_split(bool on, int8_t cc)
{
	if (on) {
		dmr_stop();
		gd88_radio.audio(false);
		sq_open = false;
		split = true;
		split_cc = cc;
		return true;
	}
	split = false;
	if (tuned) {
		rx2_tune_t t = cur;
		op_tune(&t);	// back to what A asked of the receiver
	} else {
		gd88_path_rx(cur.rx_hz, false, false);
	}
	return true;
}

// Receive on hz in DMR (the modem's slot timing comes from what it hears there) and arm the bursts at the power level
static bool tx_arm(uint32_t hz, uint8_t power)
{
	gd88_radio.tx_off();
	hrc_dmr.stop();
	if (hz == 0) {
		return true;
	}
	static const subtone_t none = { TONE_NONE, 0 };
	gd88_radio.set_mode(RADIO_MODE_DMR);
	if (!gd88_radio.rx(hz, RADIO_BW_12K5, &none)) {
		return false;
	}
	dmr_config_t c = { .own_id = cur.own_id, .cc = split_cc, .access = DMR_ACCESS_IMPOLITE, .repeater = false };
	if (!hrc_dmr.start(&c, tx_event, NULL)) {
		return false;
	}
	if (!gd88_radio.tx(hz, RADIO_BW_12K5, power, &none)) {
		hrc_dmr.stop();
		return false;
	}
	return true;
}

static void tx_ev_send(const uint8_t *p, uint8_t len)
{
	rx2_recv_dmr_ev(&link, p, len);
}
#endif

static const rx2_ops_t ops = {
	.tune = op_tune,
	.stop = op_stop,
	.audio = op_audio,
	.status = op_status,
	.stub = op_stub,
	.reset = op_reset,
	.led = gd88b_leds,
	.path = gd88b_path_main,
	.tx = gd88b_pa_arm,
	.speaker = gd88b_speaker,
#if CONFIG_DMR && GD88_DMR_TX
	.dmr = rx2_dmr_recv,
#endif
#if CONFIG_RX2_DL_SERVER
	.dl = rx2_dl_handle,
#endif
};

#if CONFIG_RX2_DL_SERVER
// The debug link's view section of this SoC (rx2_dl.c; from the link task, so the stack is this task's)
static void view(dl_text_t *t)
{
	radio_status_t rs;
	gd88_radio.status(&rs);
	dl_text(t, "rx.tuned=%u\nrx.hz=%u\nrx.mode=%u\nrx.bw=%u\nrx.squelch=%u\nrx.sq_open=%u\nrx.audio=%u\n", tuned,
		(unsigned)cur.rx_hz, cur.mode, cur.bw, cur.squelch, sq_open, audio_on);
	dl_text(t, "rx.rssi_dbm=%d\nrx.noise=%u\nrx.carrier=%u\nrx.tone_ok=%u\n", rs.rssi_dbm, rs.noise, rs.carrier,
		rs.tone_ok);
	dl_text(t, "spk.switch=%u\n", gd88b_speaker_on());
#if CONFIG_DMR
	const dmr_call_view_t *v = dmr_call_view();
	dl_text(t, "dmr.on=%u\ndmr.phase=%u\ndmr.cc=%u\ndmr.ts=%u\ndmr.src=%u\ndmr.dst=%u\n", dmr_on, v->phase,
		cur.color_code, cur.timeslot, (unsigned)v->caller.src, (unsigned)v->caller.dst);
#if GD88_DMR_TX
	// A's second transmitter (rx2_dmr.h)
	const rx2_dmr_recv_state_t *x = rx2_dmr_recv_state();
	dl_text(t, "dmrtx.split=%u\ndmrtx.armed=%u\ndmrtx.tx=%u\ndmrtx.hz=%u\ndmrtx.requests=%u\ndmrtx.bursts=%u\n", x->split,
		x->armed, x->tx, (unsigned)x->hz, (unsigned)x->requests, (unsigned)x->bursts);
	dl_text(t, "dmrtx.refused=%u\ndmrtx.dropped=%u\ndmrtx.arm_fails=%u\n", (unsigned)x->refused, (unsigned)x->dropped,
		(unsigned)x->arm_fails);
	uint32_t paths, path_refused, keys, skips;
	gd88b_own_tx_counts(&paths, &path_refused, &keys, &skips);
	dl_text(t, "dmrtx.paths=%u\ndmrtx.path_refused=%u\ndmrtx.pa_keys=%u\ndmrtx.pa_skips=%u\n", (unsigned)paths,
		(unsigned)path_refused, (unsigned)keys, (unsigned)skips);
#endif
#endif
	hrc_link_uart_stats_t u;
	hrc_link_uart_stats(&u);
	dl_text(t, "uart.rx=%u\nuart.tx=%u\nuart.overruns=%u\nuart.line_errors=%u\nuart.tx_stalls=%u\n",
		(unsigned)u.rx_bytes, (unsigned)u.tx_bytes, (unsigned)u.overruns, (unsigned)u.line_errors,
		(unsigned)u.tx_stalls);
	dl_text(t, "task.stack_free=%u\nheap.free=%u\n", (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
		(unsigned)xPortGetFreeHeapSize());
}
#endif

// The debug link's diagnostic lines of this SoC (board.diag_line, through A: rfoundry_debug.py --rx2 diag)
bool gd88b_diag_line(uint8_t i, char *buf, uint8_t len)
{
	radio_status_t rs;
	hrc_link_uart_stats_t u;
	switch (i) {
	case 0:
		gd88_radio.status(&rs);
		dl_fmt(buf, len, "b rx %s %u.%05u %s sq %s rssi %d nz %u", tuned ? "on" : "off",
		       (unsigned)(cur.rx_hz / 1000000u), (unsigned)((cur.rx_hz % 1000000u) / 10u),
		       cur.mode == RADIO_MODE_DMR ? "DMR" : "FM", sq_open ? "open" : "shut", rs.rssi_dbm, rs.noise);
		return true;
	case 1:
		dl_fmt(buf, len, "b link frames %u bad %u dl %u audio %u", (unsigned)link.frames,
		       (unsigned)link.parser.bad, (unsigned)link.dl_requests, audio_on);
		return true;
	case 2:
		hrc_link_uart_stats(&u);
		dl_fmt(buf, len, "b uart rx %u tx %u ovr %u err %u stall %u", (unsigned)u.rx_bytes, (unsigned)u.tx_bytes,
		       (unsigned)u.overruns, (unsigned)u.line_errors, (unsigned)u.tx_stalls);
		return true;
	case 3:
		return gd88b_pa_diag_line(buf, len);
	case 4:
		dl_fmt(buf, len, "b cal %s #%u nand %s", gd88_cal_source() == GD88_CAL_FLASH ? "flash"
		       : gd88_cal_source() == GD88_CAL_NAND ? "nand" : "none", (unsigned)gd88_cal_counter(),
		       gd88_nand_ready() ? "up" : "down");
		return true;
	default:
#if CONFIG_DMR
		// Then the DMR modem's lines, as A's (its TX starts' timing while it is A's second transmitter)
		return hrc_dmr_stats_line((uint8_t)(i - 5u), buf, len);
#else
		return false;
#endif
	}
}

// FM: the squelch with a short hold, and the chip's audio while it is open
static void fm_poll(void)
{
	bool open = gd88_radio_squelch_open(cur.squelch);
	if (open) {
		sq_closed = 0;
	} else if (sq_closed < SQ_CLOSE_POLLS) {
		sq_closed++;
	}
	bool now = open || (sq_open && sq_closed < SQ_CLOSE_POLLS);
	if (now != sq_open) {
		sq_open = now;
		gd88_radio.audio(now);
	}
}

static void task_fn(void *arg)
{
	(void)arg;
#if CONFIG_CRASH_LOG
	crash_init();		// the record of a crash before this start, before anything can crash again
#endif
	board_init();
	gd88_radio.init();
	gd88b_pa_init();
#if CONFIG_DMR
	dmr_call_init(&hrc_dmr);
	ambeVoiceInit(&hrc_audio_pcm, &hrc_vocoder);
	dmr_call_set_voice(ambeVoiceSink(), NULL);
#endif
	IO_DIPLEX0 |= DIPLEX0_UART3;
	hrc_link_uart_init(UART3_BASE, IRQ_UART3, LINK_BAUD);
#if CONFIG_DMR && GD88_DMR_TX
	static const rx2_dmr_recv_cfg_t tx_cfg = { &hrc_dmr, tx_split, tx_arm, tx_ev_send };
	rx2_dmr_recv_init(&tx_cfg);
#define HELLO_DMR_TX	RX2_HELLO_DMR_TX
#else
#define HELLO_DMR_TX	0u
#endif
	rx2_recv_init(&link, send, &ops, (CONFIG_DMR ? RX2_HELLO_DMR : 0u) | RX2_HELLO_STAY | HELLO_DMR_TX, RFOUNDRY_GIT);
#if CONFIG_RX2_DL_SERVER
	rx2_dl_init(&link, view);
#endif
	rx2_recv_log(&link, "gd88b up");
	dlog("gd88b up, %s", RFOUNDRY_GIT);
	uint8_t buf[32];
	for (;;) {
		hrc_link_uart_wait(POLL_MS);
		uint32_t now = hal_millis();
		alive_ms = now;
		size_t n;
		while ((n = hrc_link_uart_read(buf, sizeof(buf))) != 0) {
			for (size_t i = 0; i < n; i++) {
				rx2_recv_rx(&link, buf[i], now);
			}
		}
#if CONFIG_RX2_DL_SERVER
		rx2_dl_service();	// a restart or crash test asked for, its reply out
#endif
#if CONFIG_CRASH_LOG
		crash_alive(now);	// the tick does this too, with the watchdog
#endif
#if CONFIG_DMR && GD88_DMR_TX
		rx2_dmr_recv_poll(now);
		if (split) {
			// the modem is A's transmitter's
		} else
#endif
		if (tuned) {
#if CONFIG_DMR
			if (cur.mode == RADIO_MODE_DMR) {
				dmr_call_poll(now);
			} else
#endif
			{
				fm_poll();
			}
		}
		rx2_recv_poll(&link, now);
		gd88b_leds_poll(now);
	}
}

void app_start(void)
{
	xTaskCreate(task_fn, "rx2b", TASK_STACK, NULL, TASK_PRIORITY, NULL);
}

#if CONFIG_WATCHDOG
// FreeRTOS calls it on every pass of the idle loop (configUSE_IDLE_HOOK): the watchdog is fed while the receiver task
// keeps running
static volatile uint32_t fed_ms;

void vApplicationIdleHook(void);
void vApplicationIdleHook(void)
{
	uint32_t now = hal_millis();
	if (now - alive_ms < WD_STALL_MS) {
		hal_watchdog_feed();
		fed_ms = now;
	}
}

#if CONFIG_CRASH_LOG
// From the tick interrupt (platform port.c), as core/app/watchdog.c's on A: the uptime the next start reports, and a
// record of the hang shortly before the watchdog's reset (crash-log.md 1.1)
void watchdog_tick(void)
{
	uint32_t now = hal_millis();
	uint32_t timeout = hal_watchdog_timeout_ms();
	crash_alive(now);
	if (fed_ms != 0 && timeout != 0 && now - fed_ms >= timeout - timeout / 4u) {
		hal_crash_hang(now - fed_ms);
	}
}
#endif
#endif
