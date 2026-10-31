// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A power key and latch, battery and volume knob on the ADC, status LEDs (docs/spec/hardware-gd88.md 9.1).
//
// PTB12 reads high while the power key is pressed; the latch (PTB13) keeps the radio on. The radio is switched on with
// the volume knob, so PTB12 is most likely the knob's switch contact; it is handled as a key all the same: once seen
// low, a high reading switches the radio off. A knob that holds the line high while on never does that. ADC channel 1
// is the knob's position and channel 2 the battery (2S Li-ion): V = (raw + 5.7818) / 7.8551 / 10. A battery reading
// below 50 means the supply is going: the radio switches off. The LEDs are on SoC B (gd88_rx2_led).
#include "gd88.h"
#include "pins.h"
#include "platform.h"
#include "audio_pcm.h"
#include "hal_time.h"
#include "power/battery/battery.h"

// TODO(gd88): unknown, needs a radio: whether PTB12 is the knob's contact or a key
#define KEY_OFF_POLLS		3u	// pressed this many polls in a row (100 ms each) before believing it
#define BAT_GONE_RAW		50u
#define BAT_GONE_SAMPLES	10u	// a second of readings below it
#define BAT_FULL_MV		8400u
#define BAT_EMPTY_MV		6000u
// The knob: 21 positions over the ADC's range, with a little hysteresis so a knob resting on a boundary doesn't
// flicker between two. The knob reads 0 to 1010 end to end (hardware-gd88.md 9)
#define VOL_LEVELS		21u
#define VOL_FULL_RAW		1010u
#define VOL_HYST_RAW		12u

static battery_avg_t bat;
static uint8_t off_count, gone_count;
static bool seen_on;		// the key was seen released
static uint8_t vol_level;
// What the knob does when turned off, for the diagnostics page (the radio stays up on the latch, 2026-10-26): the
// battery channel's lowest reading, the polls PTB12 read high, and the input bits of each GPIO bank that ever changed
static gd88_power_watch_t watch;
static uint32_t port_start[3];
static const uint32_t port_base[3] = { GPIOA_BASE, GPIOB_BASE, GPIOC_BASE };

static void watch_ports(void)
{
	for (unsigned i = 0; i < 3; i++) {
		watch.changed[i] |= REG32(port_base[i] + GPIO_EXT) ^ port_start[i];
	}
}

void gd88_power_watch(gd88_power_watch_t *w)
{
	*w = watch;
}

static uint32_t raw_to_mv(uint16_t raw)
{
	return ((uint32_t)raw * 10000u + 57818u) * 100u / 78551u;	// (raw + 5.7818) / 7.8551 x 100
}

static uint32_t read_mv(void)
{
	return raw_to_mv(hrc_adc_read(GD88_ADC_BATTERY));
}

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_KEY);
	bat = (battery_avg_t){ 0 };
	off_count = 0;
	gone_count = 0;
	seen_on = false;
	vol_level = 0;
	watch = (gd88_power_watch_t){ .bat_min = UINT16_MAX };
	for (unsigned i = 0; i < 3; i++) {
		port_start[i] = REG32(port_base[i] + GPIO_EXT);
	}
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static uint16_t battery_mv(void)
{
	return battery_avg_mv(&bat, hal_millis(), read_mv);
}

static uint8_t battery_pct(void)
{
	return battery_linear_pct(battery_mv(), BAT_EMPTY_MV, BAT_FULL_MV);
}

// The level whose band holds raw; a move of less than VOL_HYST_RAW past the current band's edge keeps it
static int8_t volume(void)
{
	uint32_t raw = hrc_adc_read(GD88_ADC_VOLUME);
	uint32_t band = (VOL_FULL_RAW + VOL_LEVELS - 1u) / VOL_LEVELS;
	uint32_t lo = vol_level * band;
	uint32_t hi = lo + band;
	if (raw + VOL_HYST_RAW < lo || raw >= hi + VOL_HYST_RAW) {
		uint32_t l = raw / band;
		vol_level = (uint8_t)(l >= VOL_LEVELS ? VOL_LEVELS - 1u : l);
	}
	return (int8_t)(vol_level * 100u / (VOL_LEVELS - 1u));
}

static bool switch_on(void)
{
	uint16_t b = hrc_adc_read(GD88_ADC_BATTERY);
	if (b < watch.bat_min) {
		watch.bat_min = b;
	}
	watch_ports();
	if (b < BAT_GONE_RAW) {
		if (gone_count < BAT_GONE_SAMPLES) {
			gone_count++;
		}
	} else {
		gone_count = 0;
	}
	if (gone_count >= BAT_GONE_SAMPLES) {
		return false;
	}
	// The key only counts once it has been seen released (it is held to switch on)
	bool high = hrc_pin_read(PIN_POWER_KEY);
	if (high && watch.key_high < UINT16_MAX) {
		watch.key_high++;
	}
	if (!high) {
		seen_on = true;		// released
		off_count = 0;
	} else if (seen_on && off_count < KEY_OFF_POLLS) {
		off_count++;
	}
	return off_count < KEY_OFF_POLLS;
}

static void led(uint8_t which, bool on)
{
	gd88_rx2_led(which, on);
}

const hal_power_t gd88_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.mic_level = HRC_MIC_LEVEL,	// the mic PCM, as on the other HR_C7000 radios
	.switch_on = switch_on,
	.led = led,
};
