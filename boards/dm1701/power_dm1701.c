// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 power latch, battery, volume, LEDs: STUB, to be replaced
#include "dm1701.h"
#include "pins.h"

static void init(void) {}
static void latch(bool on) { PIN_WRITE(PIN_POWER_LATCH, on); }
static uint16_t battery_mv(void) { return 7400; }
static uint8_t battery_pct(void) { return 100; }
static int8_t volume(void) { return 50; }
static bool switch_on(void) { return true; }
static void led(uint8_t which, bool on) { if (which == LED_GREEN) { PIN_WRITE(PIN_LED_GREEN, on); } else { PIN_WRITE(PIN_LED_RED, on); } }

const hal_power_t dm1701_power = { .init = init, .latch = latch, .battery_mv = battery_mv, .battery_pct = battery_pct, .volume = volume, .switch_on = switch_on, .led = led };
