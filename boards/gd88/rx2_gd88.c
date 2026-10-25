// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A: SoC B as the second receiver (hal_rx2_t, docs/spec/rx2-link.md) over UART3 at 115200 (PTA13 / PTA14).
// The link's main side is core/rx2/rx2_link.c; this file moves its bytes, runs it from a task of its own, and routes
// B's audio to the speaker. Text B sends (RX2_LOG) goes to the debug link's log.
//
// A's status LEDs are on B too: led() sends them over the link (RX2_LED).
//
// B's update route (rx2-link.md 4): bridge() sends B to its first stage, then copies bytes between the PC cable
// (UART1) and the link (UART3) in a polled loop with interrupts off and the transmitter off, until the host has been
// quiet 10 s (90 s if it never sent anything), then restarts A.
#include <string.h>
#include "gd88.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rx2/rx2_link.h"
#include "debuglink/dlog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#if CONFIG_WATCHDOG
#include "hal_watchdog.h"
#endif

#define LINK_BAUD		115200u
#define DIPLEX0_UART3		0x6000u		// IO_DIPLEX0 bits 13 / 14 set: UART3 on PTA13 / PTA14
#define TASK_STACK		256		// words
#define TASK_PRIORITY		2
#define POLL_MS			20u
#define STUB_SEND_MS		20u		// the STUB frame (11 bytes, 1 ms on the wire) leaves the FIFO
#define BRIDGE_HOST_QUIET_MS	10000u
#define BRIDGE_NO_HOST_MS	90000u
#define BRIDGE_RING		256u		// power of two, per direction
#define BRIDGE_FEED_MS		500u

static rx2_main_t link;
static SemaphoreHandle_t lock;
static TaskHandle_t task;

static void send(const uint8_t *buf, uint16_t len)
{
	hrc_link_uart_write(buf, len);
}

static void take(void)
{
	xSemaphoreTake(lock, portMAX_DELAY);
}

static void give(void)
{
	xSemaphoreGive(lock);
}

// After a frame completes, the parser still holds it: RX2_LOG text goes to the log
static void log_frame(void)
{
	const rx2_parser_t *p = &link.parser;
	char line[48];
	uint8_t n = p->len < sizeof(line) - 1u ? p->len : (uint8_t)(sizeof(line) - 1u);
	memcpy(line, p->buf, n);
	line[n] = '\0';
	dlog("rx2: %s", line);
}

static void task_fn(void *arg)
{
	(void)arg;
	uint8_t buf[32];
	uint32_t polled = 0;
	for (;;) {
		hrc_link_uart_wait(POLL_MS);
		size_t n;
		while ((n = hrc_link_uart_read(buf, sizeof(buf))) != 0) {
			take();
			for (size_t i = 0; i < n; i++) {
				uint32_t frames = link.frames;
				rx2_main_rx(&link, buf[i], hal_millis());
				if (link.frames != frames && link.parser.type == RX2_LOG) {
					log_frame();
				}
			}
			give();
		}
		uint32_t now = hal_millis();
		if (now - polled >= POLL_MS) {
			polled = now;
			take();
			rx2_main_poll(&link, now);
			give();
		}
	}
}

void gd88_rx2_init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	uint32_t s = hrc_irq_save();
	IO_DIPLEX0 |= DIPLEX0_UART3;
	hrc_irq_restore(s);
	rx2_main_init(&link, send);
	hrc_link_uart_init(UART3_BASE, IRQ_UART3, LINK_BAUD);
	if (task == NULL) {
		xTaskCreate(task_fn, "rx2", TASK_STACK, NULL, TASK_PRIORITY, &task);
	}
}

static bool tune(const rx2_tune_t *t)
{
	take();
	bool ok = rx2_main_tune(&link, t, hal_millis());
	give();
	return ok;
}

static void stop(void)
{
	take();
	rx2_main_stop(&link);
	give();
	gd88_audio_b(false, false);
}

// Never waits on the link: the copy is taken under the lock, which the task holds only for a few bytes
static void status(rx2_status_t *st)
{
	take();
	rx2_main_status(&link, st);
	give();
}

static void audio(bool on)
{
	take();
	rx2_main_audio(&link, on, gd88_radio_volume());
	bool up = link.up;
	bool dmr = link.tune.mode == RADIO_MODE_DMR;
	give();
	gd88_audio_b(on && up, dmr);
}

// A's status LEDs, which are wired to B: LED 1 (PTA16 on B) for TX, LED 2 (PTB17) for receive
void gd88_rx2_led(uint8_t which, bool on)
{
	if (lock == NULL) {
		return;
	}
	uint8_t b = (uint8_t)(1u << (which == LED_RED ? 0 : 1));
	take();
	rx2_main_led(&link, on ? (uint8_t)(link.leds | b) : (uint8_t)(link.leds & ~b));
	give();
}

static void restart(void)
{
	take();
	rx2_main_command(&link, RX2_RESET);
	give();
}

// The bridge's rings: one per direction, so neither side's FIFO filling stalls the other
typedef struct {
	uint8_t b[BRIDGE_RING];
	uint32_t head, tail;
} ring_t;

static ring_t to_b, to_host;

static void ring_put(ring_t *r, uint8_t c)
{
	uint32_t next = (r->head + 1u) & (BRIDGE_RING - 1u);
	if (next != r->tail) {
		r->b[r->head] = c;
		r->head = next;
	}
}

static void ring_drain(ring_t *r, uint32_t base)
{
	while (r->tail != r->head && hrc_uart_poll_tx(base, r->b[r->tail])) {
		r->tail = (r->tail + 1u) & (BRIDGE_RING - 1u);
	}
}

static void bridge(void) __attribute__((noreturn));
static void bridge(void)
{
	take();
	rx2_main_command(&link, RX2_STUB);
	give();
	hal_delay_ms(STUB_SEND_MS);

	__asm__ volatile ("psrclr ie");		// interrupts off: the scheduler and every driver stop here
	hrc_rf_safe();				// transmitter off: chain all zero, PTC20 low, PA drive off
	PIC_MASK = 0xFFFFFFFFu;
	PIC_MASK_1 = 0xFFFFFFFFu;
	hrc_pin_force(PIN_SPK_AMP, false);

	to_b.head = to_b.tail = 0;
	to_host.head = to_host.tail = 0;
	uint32_t start = hal_millis();
	uint32_t host_ms = 0, fed = start;
	bool host_seen = false;
	for (;;) {
		uint8_t c;
		while (hrc_uart_poll_rx(UART1_BASE, &c)) {
			ring_put(&to_b, c);
			host_seen = true;
			host_ms = hal_millis();
		}
		while (hrc_uart_poll_rx(UART3_BASE, &c)) {
			ring_put(&to_host, c);	// B's bytes don't count: its handshake offers go on until the host answers
		}
		ring_drain(&to_b, UART3_BASE);
		ring_drain(&to_host, UART1_BASE);
		uint32_t now = hal_millis();
		if (now - fed >= BRIDGE_FEED_MS) {
			fed = now;
#if CONFIG_WATCHDOG
			hal_watchdog_feed();
#endif
		}
		if (host_seen ? now - host_ms >= BRIDGE_HOST_QUIET_MS : now - start >= BRIDGE_NO_HOST_MS) {
			break;
		}
	}
	// What is still on its way out
	for (uint32_t t = hal_millis(); hal_millis() - t < 50u;) {
		ring_drain(&to_b, UART3_BASE);
		ring_drain(&to_host, UART1_BASE);
	}
	hrc_soft_reset();
}

void gd88_rx2_diag(gd88_rx2_diag_t *d)
{
	take();
	d->up = link.up;
	d->version = link.version;
	d->flags = link.flags;
	d->fw = link.fw;
	d->frames = link.frames;
	d->bad = link.parser.bad;
	d->tunes = link.tunes;
	d->resent = link.resent;
	d->st = link.st;
	give();
	hrc_link_uart_stats(&d->uart);
}

const hal_rx2_t gd88_rx2 = {
	.tune = tune,
	.stop = stop,
	.status = status,
	.audio = audio,
	.bridge = bridge,
	.restart = restart,
};
