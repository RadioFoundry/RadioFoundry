// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV power hold, power key and supply voltage (docs/spec/hardware-d578uv.md 3.3).
//
// A mobile on a fixed 13.8 V supply: no battery (CAP_NO_BATTERY). PC3 is the power hold: the stock firmware sets it at
// boot and never clears it, even when it switches off, so whether dropping it cuts the supply is to be checked on the
// radio. The stock firmware's "off" is a standby it wakes from on a one second hold of the power key. The supply is
// read on PC1 through an 11:1 divider. The backlight is on the RF bus (radio_d578uv.c). The one LED output found is an
// RF chip's GPIO bit, which radio_d578uv.c lights on transmit, so led() does nothing here. The microphone level for VOX
// comes from RF chip 0 (radio_d578uv.c): no ADC input carries the microphone.
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#include "config.h"

#define POWER_OFF_POLLS		20u	// held this many switch_on polls after a release (the core polls about every 50 ms)
#define SUPPLY_SAMPLES		4u
// mV per count: 3.3 V reference, 12 bits, 11:1 divider (the stock scale, 363 / 4096 in 0.1 V)
#define SUPPLY_MV_NUM		36300u
#define SUPPLY_MV_DEN		4096u

static bool key_released;
static uint8_t held;

static void init(void)
{
	mcu_pin_out(PIN_POWER_LATCH, true, false);
	mcu_pin_in(PIN_POWER_KEY, MCU_PULL_UP);
	mcu_pin_analog(PIN_SUPPLY_SENSE);
	mcu_pin_analog(PIN_VOL_A);
	mcu_pin_analog(PIN_VOL_B);
	mcu_adc_init();
	key_released = false;
	held = 0;
}

static void latch(bool on)
{
	mcu_pin_write(PIN_POWER_LATCH, on);
}

// The A channel's pot, 0..100 (the stock maps it to 33 levels over 0x96..0xF60)
static int8_t volume(void)
{
	uint32_t v = mcu_adc_read(ADC_VOL_A);
	if (v <= 0x96u) {
		return 0;
	}
	if (v >= 0xF60u) {
		return 100;
	}
	return (int8_t)((v - 0x96u) * 100u / (0xF60u - 0x96u));
}

// The key must be let go after power-on before a hold counts, so the press that switched the radio on doesn't
// switch it off again
static bool switch_on(void)
{
	bool down = !mcu_pin_read(PIN_POWER_KEY);
	if (!down) {
		key_released = true;
		held = 0;
	} else if (key_released && held < POWER_OFF_POLLS) {
		held++;
	}
	return held < POWER_OFF_POLLS;
}

static void led(uint8_t which, bool on)
{
	(void)which;
	(void)on;
}

static uint16_t supply_mv(void)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < SUPPLY_SAMPLES; i++) {
		sum += mcu_adc_read(ADC_SUPPLY_SENSE);
	}
	return (uint16_t)(sum * SUPPLY_MV_NUM / (SUPPLY_MV_DEN * SUPPLY_SAMPLES));
}

const hal_power_t d578uv_power = {
	.init = init,
	.latch = latch,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
	.supply_mv = supply_mv,
#if CONFIG_VOX
	.mic_level = d578uv_mic_level,
#endif
};
