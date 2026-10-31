// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV Bluetooth PTT and keys (docs/spec/hardware-d578uv.md 3.7): the module on UART4 (PC10 TX, PC11 RX), 115200
// 8N1, its AT lines read by drivers/input/bt_at. The module's supply is the shift register's bit 0x02, on from boot
// (radio_d578uv.c). Not every model has the module and no pin tells: as the stock firmware does, the radio asks
// BT_AT_PROBE a few times at start and takes any answer as the module. The APRS add-on board answers on the same UART
// and is told apart by its answer; it isn't used. Pairing, audio and the module's settings aren't written: the module
// keeps the pairings the stock firmware made (inferred)
#include "config.h"
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "input/bt_at/bt_at.h"

#if CONFIG_BT_PTT

#define BT_BAUD			115200u
#define BT_IRQ_PRIORITY		6
#define BT_PROBES		5u	// the stock firmware's count, 200 ms apart
#define BT_PROBE_MS		200u
#define BT_KEY_MS		150u	// a key event holds its key bit this long, past the core's debounce

static bt_at_t bt;
static uint32_t start_ms, key_ms, key_seen;
static uint8_t probes, key_bit_held = 0xFF;

void MCU_UART4_HANDLER(void)
{
	int32_t c = mcu_uart_take(MCU_UART4);
	if (c == MCU_UART_ERROR) {
		bt_at_error(&bt);
	} else if (c != MCU_UART_NONE) {
		bt_at_byte(&bt, (uint8_t)c);
	}
}

void d578uv_bt_init(void)
{
	bt_at_init(&bt);
	mcu_pin_in(PIN_BT_RX, MCU_PULL_UP);
	mcu_pin_af(PIN_BT_TX, BT_TX_AF, false);
	mcu_uart_start(MCU_UART4, BT_BAUD, true);
	NVIC_SetPriority(MCU_UART4_IRQn, BT_IRQ_PRIORITY);
	NVIC_EnableIRQ(MCU_UART4_IRQn);
	start_ms = hal_millis();
}

// The module's key events as the radio's keys: 01 and 02 as the microphone's Up and Down (the stock firmware posts
// its encoder's two codes for them; inferred), 03 and 04 (Monitor in the stock firmware; inferred) have no key here
static int8_t key_bit(uint8_t code)
{
	switch (code) {
	case 1:
		return KEY_MIC_UP;
	case 2:
		return KEY_MIC_DOWN;
	default:
		return -1;
	}
}

// From the key scan: the probe while nothing has answered, then PTT and the key bits
uint32_t d578uv_bt_scan(void)
{
	uint32_t now = hal_millis();
	NVIC_DisableIRQ(MCU_UART4_IRQn);
	bool present = bt.present && !bt.aprs_board;
	bool answered = bt.present;
	bool ptt = bt_at_ptt(&bt);
	uint32_t events = bt.key_events;
	uint8_t key = bt.key;
	NVIC_EnableIRQ(MCU_UART4_IRQn);
	if (!answered && probes < BT_PROBES && now - start_ms >= (uint32_t)probes * BT_PROBE_MS) {
		for (const char *s = BT_AT_PROBE; *s; s++) {
			mcu_uart_put(MCU_UART4, (uint8_t)*s);
		}
		probes++;
	}
	if (!present) {
		return 0;
	}
	if (events != key_seen) {
		key_seen = events;
		key_ms = now;
		int8_t b = key_bit(key);
		key_bit_held = b >= 0 ? (uint8_t)b : 0xFF;
	}
	uint32_t m = ptt ? 1u << KEY_PTT : 0u;
	if (key_bit_held != 0xFF && now - key_ms < BT_KEY_MS) {
		m |= 1u << key_bit_held;
	}
	return m;
}

void d578uv_bt_stats(hal_bt_stats_t *st)
{
	st->present = bt.present && !bt.aprs_board;
	st->ptt = bt_at_ptt(&bt);
	st->key = bt.key;
	st->keys = bt.key_events;
	st->lines = bt.lines;
	st->errors = bt.errors;
}

#endif
