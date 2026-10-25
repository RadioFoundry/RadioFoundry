// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B: the second receiver's application (docs/spec/rx2-link.md, hardware-gd88.md 10). There is no radio core
// here: app_start (called by platform/hrc7000/main.c) starts one task that runs the link's receiver side (core/rx2) on
// UART3, the shared radio code (boards/gd88/common) on B's own BK4819 pair, the FM squelch and, on DMR channels, the
// call stack and codec (core/dmr). The calls rx2_ops_t makes all come from rx2_recv_rx() in this task.
#include "config.h"
#include "board.h"
#include "gd88_rf.h"
#include "gd88b.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#include "rx2/rx2_link.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
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

static bool op_tune(const rx2_tune_t *t)
{
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

static const rx2_ops_t ops = {
	.tune = op_tune,
	.stop = op_stop,
	.audio = op_audio,
	.status = op_status,
	.stub = op_stub,
	.reset = op_reset,
	.led = gd88b_leds,
};

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
	board_init();
	gd88_radio.init();
#if CONFIG_DMR
	dmr_call_init(&hrc_dmr);
	ambeVoiceInit(&hrc_audio_pcm, &hrc_vocoder);
	dmr_call_set_voice(ambeVoiceSink(), NULL);
#endif
	IO_DIPLEX0 |= DIPLEX0_UART3;
	hrc_link_uart_init(UART3_BASE, IRQ_UART3, LINK_BAUD);
	rx2_recv_init(&link, send, &ops, CONFIG_DMR ? RX2_HELLO_DMR : 0u, RFOUNDRY_GIT);
	rx2_recv_log(&link, "gd88b up");
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
	}
}

void app_start(void)
{
	xTaskCreate(task_fn, "rx2b", TASK_STACK, NULL, TASK_PRIORITY, NULL);
}

#if CONFIG_WATCHDOG
// FreeRTOS calls it on every pass of the idle loop (configUSE_IDLE_HOOK): the watchdog is fed while the receiver task
// keeps running
void vApplicationIdleHook(void);
void vApplicationIdleHook(void)
{
	if (hal_millis() - alive_ms < WD_STALL_MS) {
		hal_watchdog_feed();
	}
}
#endif
