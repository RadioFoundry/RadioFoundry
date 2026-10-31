// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV radio: the four QA4818-class chips as two superhets, side A (receive and transmit) and side B (receive
// only, the second receiver), analog FM and DMR (docs/spec/hardware-d578uv.md 5, 7).
//
// Receive on f: chip 2 (side A's LO) runs at f - 38.85 MHz when that is at least 300 MHz, else at f + 38.85 MHz, and
// chip 0 receives and demodulates at the 38.85 MHz IF. Side B is the same with chips 3 (LO) and 1 at a 38.55 MHz IF
// (5.2): with CONFIG_DUAL_WATCH it is the core's second receiver (hal_rx2.h) for an FM channel, so dual watch hears
// both channels at once; it is off otherwise, and while side A transmits. Its squelch is decided here, by the core's
// noise levels (radio_sm.h). The speaker takes side B's audio through the level device's second channel.
//
// Air band AM (CONFIG_AM_RX, 5.4): side B's hardware as a superhet into the KTMicro AM/FM receiver on PB6/PB7 (I2C),
// the stock firmware's set-up. Chip 3 runs as the LO at f + 29.25 MHz with its output bit 6 set (the receiver's input
// from chip 3's mixer, inferred), the receiver demodulates AM at 29.25 MHz, and chips 0-2 are off. Its squelch readings
// become the RSSI the core's AM squelch uses, scaled so the core's levels open where the stock firmware's do. The
// receiver's audio is muted unless the core gives the main side the speaker; how it reaches the speaker isn't traced
// (both level device channels play it, inferred). Side B (dual watch) waits while AM is on.
//
// Broadcast FM (CONFIG_BCAST_FM, 5.4): the same receiver in its FM mode on its own input, chip 3's output bit 6 clear
// (inferred), as the board's hal_bcast_t (d578uv_bcast). It shares the receiver with AM, so the core's order decides:
// an air band channel's rx_am() comes before the broadcast receiver is switched off, and a broadcast channel switches
// it on without a channel receiver call, so power(true) takes the receiver back from AM and power(false) leaves an AM
// set-up alone. One level for both modes (kt_level). Side A keeps its last set-up, muted by the core, as the core
// expects; dual watch's side B runs on as with an FM channel.
//
// TX tones (5.3): the 1750 Hz burst (tx_tone) from chip 0's tone generator, DTMF (tx_dtmf) synthesized on PB9
// (tone_d578uv.c) into the chip's audio input pin, as the stock firmware sends them, and APRS (d578uv_afsk) the same
// way; chip 0's output bit 0 mutes the microphone meanwhile (inferred).
//
// Transmit on f: both side A chips off, chip 0 retuned to f with its TX constants and drive, then keyed; the PA level is
// PA4's DAC from the factory calibration (NAND, 7). TX is refused without a valid calibration block and outside the
// bands.
//
// DMR transmit (5.1): tx() keys chip 0 once with the digital set and it stays keyed until tx_off(); only the PA follows
// the bursts. The baseband's burst line PD0 interrupts on both edges (EXTI 0): high turns the DAC and PE6 on, low the
// DAC off at once and PE6 after an 800 us one-shot on TIM2, the stock firmware's timer and period. The core calls
// tx() before the call stack starts the modem's transmission, so the RF is keyed before the baseband gets
// DMR_CALL_SLOT with the transmit bit, the stock order.
//
// Around the chips: the 8-bit shift register (PE3 data, PE4 clock) switches the audio and RF paths, PE6 enables the
// transmitter, PA7 mutes the speaker, the two-wire level device on PA13/PA14 sets the speaker level (SWD is off while
// it is in use), and the serial device on PA5 (4-bit address, 8-bit data on the RF bus lines) holds the backlight
// level in its register 8 and values the stock firmware writes once. Each value is the stock firmware's; what a shift
// register bit does is inferred from where the stock firmware sets it.
#include "d578uv.h"
#include "config.h"
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "rf/rfbus/rfbus.h"
#include "rf/qa4818/qa4818.h"
#include "front_io.h"
#include "pa_gate.h"
#include "rf_plan.h"
#include "radio/ref_trim.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_DTMF
#include "hal_dtmf.h"
#endif
#if CONFIG_DUAL_WATCH
#include "radio/radio_sm.h"
#endif
// The KTMicro receiver: air band AM and broadcast FM
#define KT_USED			(CONFIG_AM_RX || CONFIG_BCAST_FM)
#if KT_USED
#include "bcast/ktamfm/ktamfm.h"
#include "bus/i2c_gpio/i2c_gpio.h"
#endif

// The two-wire speaker level device on the SWD pins. 0 leaves SWD on and the device at its power-on level
#ifndef D578UV_LEVEL_IC
#define D578UV_LEVEL_IC		1
#endif

#define IF_A_HZ			38850000u
#define IF_B_HZ			38550000u
#define UHF_FROM_HZ		300000000u
// Chip 0 / 1 register 0x49: 0x26AC on the PRO; the 578UV's stock firmware writes 0x29AA unless a flag (not identified)
// is set (5). No pin tells the models apart, so the PRO value is used
#define R49			0x26ACu
// Chip GPIO (0x25) at init: chip 0, chip 1
#define GPIO_A_INIT		0x0001u
#define GPIO_B_INIT		0x0033u
// Chip 0 GPIO bits: 3/6 side A's complementary band pair (UHF sets 3, VHF 6), 7/4 side B's (UHF sets 7, VHF 4; both
// clear with side B off, as the stock firmware leaves them in some single-receiver cases); 5 set while transmitting
// above 300 MHz, cleared at TX end (inferred); 1 an LED, which the stock firmware lights on transmit by default (which
// color is not known); 2 the GPS module's supply, high = on (d578uv_gps_power, hardware-d578uv.md 3.6); 0 set in
// receive and while a TX tone sounds, clear for the microphone: a microphone mute (inferred)
#define GPIO_BAND_A_MASK	0x0048u
#define GPIO_BAND_A_UHF		0x0008u
#define GPIO_BAND_A_VHF		0x0040u
#define GPIO_BAND_B_MASK	0x0090u
#define GPIO_BAND_B_UHF		0x0080u
#define GPIO_BAND_B_VHF		0x0010u
#define GPIO_MIC_MUTE		0x0001u
#define GPIO_TX_UHF		0x0020u
#define GPIO_LED_TX		0x0002u
#define GPIO_GPS		0x0004u
// Chip 3 output bit 6: set for AM, clear for the receiver's broadcast FM input (inferred, 5.4)
#define GPIO_LO_B_AM		0x0040u

// Shift register bits (roles inferred from the stock firmware's use)
#define SR_BOOT			0x02u	// set at boot
#define SR_ANALOG		0x04u	// analog channel on the main side (INF)
#define SR_RF_AUDIO		0x08u	// speaker from the RF chip, 0 for the DMR codec (INF)
#define SR_TX			0x10u	// transmit path
#define SR_RX_PATH		0x20u	// cleared at the end of TX turn-on (receive front end, INF)
#define SR_RX_AUDIO		0x40u	// receive audio gate
#define SR_RX			(SR_BOOT | SR_ANALOG | SR_RF_AUDIO | SR_RX_PATH | SR_RX_AUDIO)

// PC4: low in receive; the stock firmware sets it when RX and TX frequencies straddle 300 MHz (not done here)
#define PIN_RF_SPLIT		GPIOC, MCU_PIN(4)

// Calibration (7): the current one of two blocks, its UHF and VHF blocks cached, the rest read when needed
#define CAL_A			0x02F60000u
#define CAL_B			0x02F80000u
#define CAL_MARKS		0x1FFF8u	// AA AA 55 55 (superseded), then 55 55 AA AA (written)
#define CAL_UHF			0x000u
#define CAL_VHF			0x040u
#define CAL_BLOCK_LEN		0x20u
#define CAL_PWR_UHF		0x200u		// four 121-byte tables (Low..Turbo) every 0x80, 400-520 MHz in 1 MHz steps
#define CAL_PWR_VHF		0x400u		// four 45-byte tables every 0x40, 136-175 MHz in 1 MHz steps (then 220-225)
#define CAL_PWR_UHF_REF		0x28u
#define CAL_PWR_VHF_REF		9u
#define CAL_AGC_VHF_A		0x500u		// 25 entries, 2 MHz steps from 136 MHz
#define CAL_AGC_UHF_A		0x520u		// 25 entries, 5 MHz steps from 400 MHz
#define CAL_AGC_VHF_B		0x640u		// side B's, laid out as side A's
#define CAL_AGC_UHF_B		0x660u
#define CAL_AGC_VHF_REF		5u
#define CAL_AGC_UHF_REF		8u
// UHF / VHF block bytes
#define BLK_PWR			1u		// 1..4: DAC base Low..Turbo
#define BLK_DRIVE		5u		// 0x40 drive level
#define BLK_TONE_CTCSS		6u		// 0x51 level with CTCSS or none (the UHF block's, on both bands)
#define BLK_TONE_DCS		7u
#define BLK_AGC_A		8u
#define BLK_AGC_B		9u
#define BLK_DTMF_UHF		0x17u		// the 0x40 level for DTMF above 200 MHz (the UHF block's, as both below)
#define BLK_DTMF_VHF		0x18u
// Without calibration: the receive gain code (no stock default found, INF)
#define GAIN_DEFAULT		0x50u

static rfbus_3w_t bus_if, bus_lo;
static qa4818_t chip_if, chip_lo;	// chips 0 and 2

// Side B: chips 1 and 3, run by the radio task (the task of the core's tune() calls), for dual watch and the air band
#define SIDE_B_CHIPS		(CONFIG_DUAL_WATCH || CONFIG_AM_RX)
#if SIDE_B_CHIPS
static rfbus_3w_t bus_b, bus_lo_b;
static qa4818_t chip_b, chip_lo_b;
#endif
#if KT_USED
static ktamfm_t kt;
static bool kt_present;			// the receiver answered at init
static uint8_t kt_level_out = 0xFFu;	// the receiver's level as written; 0xFF unknown

bool d578uv_kt_present(void)
{
	return kt_present;
}
#endif
#if CONFIG_AM_RX
static bool am_on;			// rx_am() took a channel: side B's chips and the receiver are AM's
#define AM_ON			am_on
#else
#define AM_ON			false
#endif
#if CONFIG_BCAST_FM
static bool bc_on;			// the core powered the broadcast receiver: the receiver is in FM
static bool bc_audio;			// the core plays it
static bool bc_silent;			// the core's broadcast volume is 0
static uint32_t bc_khz;			// the frequency it is tuned to
#define BC_HEARD		(bc_on && bc_audio && !transmitting)
#else
#define BC_HEARD		false
#endif

#if CONFIG_DUAL_WATCH
static TaskHandle_t b_task;
static bool b_on;			// tuned (tune() took a channel), running unless side A transmits
static bool b_audio;			// the core gives side B the speaker
static bool b_open;			// its squelch is open
static uint8_t b_close_polls;		// polls in a row above the close level
static uint32_t b_hz;
static int16_t b_trim;			// the reference trim side B was set up with (synth_hz)
static uint8_t b_bw;
static uint8_t b_squelch;
static subtone_t b_tone;
static rx2_status_t b_st = { .up = true };	// what status() gives other tasks
#define B_ON			b_on
#else
#define B_ON			false
#endif

static uint32_t cal_base;		// 0: none
static uint8_t cal_uhf[CAL_BLOCK_LEN], cal_vhf[CAL_BLOCK_LEN];

static radio_mode_t mode = RADIO_MODE_FM;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_25K;
static subtone_t rx_tone;
static bool have_rx;
static bool transmitting;
static uint8_t sr;			// the shift register's outputs
static bool main_audio;			// side A's audio to the speaker (audio())
static int16_t level_code_vol = -1;	// the speaker level code for the volume, -1 until the core sets one
#if D578UV_LEVEL_IC
static int16_t level_last[2] = { -1, -1 };	// level codes written, side A and B; -1 = none
#endif
// The FM transmission tx() set up, for the tones: the voice drive word and its 0x51 level to restore
static uint32_t tx_hz;
static bool tx_narrow;
static uint16_t tx_r40;
static uint8_t tx_level;
typedef enum { AUX_NONE, AUX_TONE, AUX_LINE } aux_t;	// AUX_LINE: PB9's audio (DTMF, APRS) on the input pin
static aux_t aux;			// what replaces the microphone now
static uint8_t rx_threshold;		// RSSI below which the stock firmware calls a signal lost
static volatile uint8_t backlight_reg = 0xFFu;	// the PA5 device's register 8 as last asked (full until the UI sets it)

// ---- The RF bus: shared clock and data, one enable per chip ----

static void bus_clk(bool v) { mcu_pin_write(PIN_RF_SCLK, v); }
static void bus_data_out(bool v) { mcu_pin_out(PIN_RF_SDIO, v, false); }
static void bus_data_release(void) { mcu_pin_in(PIN_RF_SDIO, MCU_PULL_NONE); }
static bool bus_data_in(void) { return mcu_pin_read(PIN_RF_SDIO); }
static void cs0(bool v) { mcu_pin_write(PIN_RF_CS0, v); }
static void cs1(bool v) { mcu_pin_write(PIN_RF_CS1, v); }
static void cs2(bool v) { mcu_pin_write(PIN_RF_CS2, v); }
static void cs3(bool v) { mcu_pin_write(PIN_RF_CS3, v); }

// The radio task drives the chips, but the backlight (the PA5 device) is set from the UI and start-up tasks: a frame
// keeps other tasks off the shared lines until it ends. The scheduler is held, not the interrupts: no interrupt
// touches the bus, and the DMR burst interrupt must not wait (a frame is about 60 us)
static uint32_t bus_lock(void)
{
	vTaskSuspendAll();
	return 0;
}

static void bus_unlock(uint32_t saved)
{
	(void)saved;
	(void)xTaskResumeAll();
}

#define BUS_PINS(cs) { cs, bus_clk, bus_data_out, bus_data_release, bus_data_in, hal_delay_us, bus_lock, bus_unlock, \
		       hal_delay_ms }
static const rfbus_3w_pins_t pins_chip[4] = { BUS_PINS(cs0), BUS_PINS(cs1), BUS_PINS(cs2), BUS_PINS(cs3) };

// The PA5 device: 4-bit address then 8 data bits, MSB first, data set while the clock is low
static void aux_write(uint8_t addr, uint8_t val)
{
	uint16_t frame = (uint16_t)((addr & 0xFu) << 8 | val);
	uint32_t s = bus_lock();
	mcu_pin_write(PIN_AUX_CS, false);
	hal_delay_us(1);
	for (int i = 11; i >= 0; i--) {
		mcu_pin_write(PIN_RF_SCLK, false);
		bus_data_out(((frame >> i) & 1u) != 0);
		hal_delay_us(1);
		mcu_pin_write(PIN_RF_SCLK, true);
		hal_delay_us(1);
	}
	mcu_pin_write(PIN_RF_SCLK, false);
	hal_delay_us(1);
	mcu_pin_write(PIN_AUX_CS, true);
	bus_unlock(s);
}

#define AUX_BACKLIGHT		0x8u

void d578uv_bus_init(void)
{
	mcu_pin_out(PIN_RF_SCLK, false, false);
	mcu_pin_in(PIN_RF_SDIO, MCU_PULL_NONE);
	mcu_pin_out(PIN_RF_CS0, true, false);
	mcu_pin_out(PIN_RF_CS1, true, false);
	mcu_pin_out(PIN_RF_CS2, true, false);
	mcu_pin_out(PIN_RF_CS3, true, false);
	mcu_pin_out(PIN_AUX_CS, true, false);
	// After a reset in transmit (a crash, the watchdog) the chips keep their last set-up: board_rf_safe takes the PA's
	// lines but not the bus. Every chip off now, before the slow start-up (the baseband's boot, the NAND); the radio's
	// init sets them up again
	for (uint8_t i = 0; i < 4; i++) {
		rfbus_3w_t b;
		rfbus_3w_attach(&b, &pins_chip[i]);
		rfbus_write(&b.bus, QA4818_REG_ENABLE, QA4818_ENABLE_OFF);
	}
}

// The stock firmware's five levels (front_io.h); 0 at the core's power-off. The panel stays awake: the stock firmware
// puts it to sleep only for its remote "radio off"
void d578uv_backlight(uint8_t percent)
{
	uint8_t code = backlight_code(percent);
	backlight_reg = code;
	aux_write(AUX_BACKLIGHT, code);
}

// ---- Shift register: 8 bits LSB first, PE5 held high (no latch pulse); the stock firmware releases the lines to
// pull-downs between writes ----

static void sr_pause(void)
{
	for (volatile int d = 0; d < 4; d++) {
	}
}

// force: from board_rf_safe (no RTOS, any clock), the lines stay outputs
static void sr_shift(uint8_t v, bool force)
{
	if (force) {
		mcu_pin_force(PIN_SR_DATA, false);
		mcu_pin_force(PIN_SR_CLK, false);
	} else {
		mcu_pin_out(PIN_SR_DATA, false, false);
		mcu_pin_out(PIN_SR_CLK, false, false);
	}
	for (int i = 0; i < 8; i++) {
		mcu_pin_write(PIN_SR_CLK, false);
		mcu_pin_write(PIN_SR_DATA, ((v >> i) & 1u) != 0);
		sr_pause();
		mcu_pin_write(PIN_SR_CLK, true);
		sr_pause();
	}
	mcu_pin_write(PIN_SR_CLK, false);
	if (!force) {
		mcu_pin_in(PIN_SR_DATA, MCU_PULL_DOWN);
		mcu_pin_in(PIN_SR_CLK, MCU_PULL_DOWN);
	}
}

static void sr_set(uint8_t set, uint8_t clear)
{
	uint8_t v = (uint8_t)((sr & ~clear) | set);
	if (v != sr) {
		sr = v;
		sr_shift(v, false);
	}
}

// ---- Speaker level device (PA13 data, PA14 clock) ----

#if D578UV_LEVEL_IC
#define LEVEL_SEL_A		2u
#define LEVEL_SEL_B		3u

static void level_pins(bool data, bool clk)
{
	mcu_pin_write(PIN_LEVEL_DATA, data);
	mcu_pin_write(PIN_LEVEL_CLK, clk);
	hal_delay_us(2);
}

static void level_bit(bool one)
{
	if (one) {
		level_pins(true, false);
		level_pins(true, true);
		level_pins(false, true);
		level_pins(false, false);
	} else {
		level_pins(false, false);
		level_pins(false, true);
		level_pins(false, false);
	}
}

// Channel select (2 bits) and level (7 bits), LSB first, then a 1 and the stop pattern
static void level_write(uint8_t sel, uint8_t code)
{
	level_pins(false, false);
	for (int i = 0; i < 2; i++) {
		level_bit(((sel >> i) & 1u) != 0);
	}
	level_pins(false, false);
	for (int i = 0; i < 7; i++) {
		level_bit(((code >> i) & 1u) != 0);
	}
	level_pins(true, false);
	level_bit(true);
	level_pins(true, true);
	level_pins(true, false);
	level_pins(false, false);
}

// Level 1..32 to the device's code: an attenuation index (0 = loudest), as 2 fine and 5 coarse bits
static const uint8_t level_index[33] = {
	44, 42, 40, 38, 36, 34, 32, 30, 29, 28, 27, 26, 25, 24, 23, 22, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7,
	6, 4, 0,
};

static uint8_t level_code(uint8_t level)
{
	if (level == 0) {
		return 0;
	}
	uint8_t i = level_index[level > 32 ? 32 : level];
	return (uint8_t)((3u - i % 4u) << 5 | (21u - i / 4u));
}
#endif

// Side (0 = A, 1 = B) at level code, skipped when unchanged
static void level_set(uint8_t side, uint8_t code)
{
#if D578UV_LEVEL_IC
	if (code == level_last[side]) {
		return;
	}
	level_last[side] = code;
	level_write(side != 0 ? LEVEL_SEL_B : LEVEL_SEL_A, code);
#else
	(void)side;
	(void)code;
#endif
}

#if CONFIG_DUAL_WATCH
static bool b_heard(void)
{
	return b_on && b_audio && b_open && !transmitting && !AM_ON;
}
#else
static bool b_heard(void) { return false; }
#endif

// The speaker's sources: PA7 open while either side plays, each side's level the volume while it plays (side A also
// while neither does, as before side B), and the shift register's RF audio bit set for an FM side. The main receiver's
// calls win: the core gives side B the speaker only while side A is quiet. In AM, and while broadcast FM plays, both
// channels take the volume (which one carries the KTMicro receiver isn't traced)
static void audio_route(void)
{
	bool b = b_heard();
	bool kt_out = (AM_ON && main_audio) || BC_HEARD;
	if (level_code_vol >= 0) {
		level_set(0, main_audio || !b ? (uint8_t)level_code_vol : 0);
		level_set(1, b || kt_out ? (uint8_t)level_code_vol : 0);
	}
	bool rf = mode == RADIO_MODE_FM || b || kt_out;
	if (!transmitting) {
		sr_set(rf ? SR_RF_AUDIO : 0, rf ? 0 : SR_RF_AUDIO);
	}
	mcu_pin_write(PIN_SPK_MUTE, !(main_audio || b || kt_out));
}

static void volume(uint8_t percent)
{
#if D578UV_LEVEL_IC
	if (percent > 100) {
		percent = 100;
	}
	level_code_vol = level_code((uint8_t)((percent * 32u + 50u) / 100u));
#else
	(void)percent;
#endif
	audio_route();
}

// ---- Calibration ----

static bool cal_read(uint32_t off, void *buf, uint32_t len)
{
	return cal_base != 0 && d578uv_nand_read(cal_base + off, buf, len);
}

static bool cal_current(uint32_t base)
{
	uint8_t m[8];
	if (!d578uv_nand_read(base + CAL_MARKS, m, sizeof(m))) {
		return false;
	}
	bool superseded = m[0] == 0xAA && m[1] == 0xAA && m[2] == 0x55 && m[3] == 0x55;
	bool written = m[4] == 0x55 && m[5] == 0x55 && m[6] == 0xAA && m[7] == 0xAA;
	return written && !superseded;
}

static void cal_load(void)
{
	cal_base = 0;
	if (!d578uv_nand_ready()) {
		return;
	}
	uint32_t base = cal_current(CAL_A) ? CAL_A : cal_current(CAL_B) ? CAL_B : 0;
	if (base == 0) {
		return;
	}
	cal_base = base;
	if (!cal_read(CAL_UHF, cal_uhf, CAL_BLOCK_LEN) || !cal_read(CAL_VHF, cal_vhf, CAL_BLOCK_LEN)) {
		cal_base = 0;
	}
}

static uint8_t cal_byte(uint32_t off, bool *ok)
{
	uint8_t b = 0;
	if (!cal_read(off, &b, 1)) {
		*ok = false;
	}
	return b;
}

// ---- The PA5 device's trims from the calibration (5.5, rf_plan.h) ----
#define AUX_FQCA		0x1u	// the UHF block's byte 0 ("FQCA"), after every side A trim
#define AUX_TRIM_B		0x2u	// side B's fixed trim: the air band's, broadcast FM's, else 0
#define AUX_TRIM_TX		0x4u	// by the TX band and power, at each key-up
#define AUX_TRIM_A_FIX		0x6u	// side A's fixed trim, by band
#define AUX_TRIM_A		0xAu	// side A's trim, interpolated over the frequency
#define AUX_TRIM_B_TABLE	0xCu	// side B's interpolated trim: the air band's, else 0
#define BLK_FQCA		0x00u
#define BLK_TRIM_A		0x0Au	// 0x0A..0x0C: side A's three points
#define BLK_TRIM_A_FIX		0x10u
#define BLK_TRIM_B		0x11u
#define BLK_TRIM_TX		0x16u
#define BLK_TRIM_AM		0x1Bu	// the UHF block's 0x1B..0x1D: the air band's three points; the VHF block's: register 2

// After each side A retune, in the stock order: register 6 by band, 0xA interpolated over the tuned frequency (UHF
// above 300 MHz, the VHF block's 220 MHz cells from 200 MHz), then FQCA. Without a calibration block the start-up's 0s
// stay. The stock firmware uses the frequency as tuned, before any reference trim
static void trims_a(uint32_t hz)
{
	if (cal_base == 0) {
		return;
	}
	uint32_t f = hz / 10u;
	aux_write(AUX_TRIM_A_FIX, (f >= F10(300) ? cal_uhf : cal_vhf)[BLK_TRIM_A_FIX]);
	aux_write(AUX_TRIM_A, trim_lookup((f > F10(300) ? cal_uhf : cal_vhf) + BLK_TRIM_A, f, f >= F10(200)));
	aux_write(AUX_FQCA, cal_uhf[BLK_FQCA]);
}

#if CONFIG_AM_RX || CONFIG_BCAST_FM
static void trims_b(void);
#endif

// The receive gain code for hz on a side: the band's AGC base plus the AGC table's reference entry minus this
// frequency's. Side B has its own bases and tables (its use of them is inferred from the calibration's layout)
static uint8_t rx_gain_code(uint32_t hz, bool side_b)
{
	if (cal_base == 0) {
		return GAIN_DEFAULT;
	}
	bool ok = true;
	uint8_t code;
	uint8_t blk = side_b ? BLK_AGC_B : BLK_AGC_A;
	if (hz > UHF_FROM_HZ) {
		uint32_t t = side_b ? CAL_AGC_UHF_B : CAL_AGC_UHF_A;
		uint32_t idx = hz >= 400000000u ? (hz - 400000000u) / 5000000u : 0;
		code = (uint8_t)(cal_uhf[blk] + cal_byte(t + CAL_AGC_UHF_REF, &ok) - cal_byte(t + (idx < 25 ? idx : 24), &ok));
	} else {
		uint32_t t = side_b ? CAL_AGC_VHF_B : CAL_AGC_VHF_A;
		uint32_t idx = hz >= 136000000u ? (hz - 136000000u) / 2000000u : 0;
		code = (uint8_t)(cal_vhf[blk] + cal_byte(t + CAL_AGC_VHF_REF, &ok) - cal_byte(t + (idx < 20 ? idx : 19), &ok));
	}
	return ok ? code : GAIN_DEFAULT;
}

// The PA DAC value (8 bits; the DAC gets it << 4) for power level p on hz: the level's base plus the power table's
// entry for hz minus its reference entry. 0 when the calibration can't say
static uint8_t tx_dac(uint32_t hz, uint8_t p)
{
	if (cal_base == 0 || p > 3) {
		return 0;
	}
	bool uhf = hz > UHF_FROM_HZ;
	const uint8_t *blk = uhf ? cal_uhf : cal_vhf;
	uint32_t table, idx, ref;
	if (uhf) {
		if (hz < 400000000u || hz >= 521000000u) {
			return 0;
		}
		table = CAL_PWR_UHF + 0x80u * p;
		idx = (hz - 400000000u) / 1000000u;
		ref = CAL_PWR_UHF_REF;
	} else {
		if (hz < 136000000u || hz >= 176000000u) {
			return 0;
		}
		table = CAL_PWR_VHF + 0x40u * p;
		idx = (hz - 136000000u) / 1000000u;
		ref = CAL_PWR_VHF_REF;
	}
	bool ok = true;
	uint8_t v = (uint8_t)(blk[BLK_PWR + p] + cal_byte(table + idx, &ok) - cal_byte(table + ref, &ok));
	return ok ? v : 0;
}

// ---- Chip 0's outputs ----

// The reference trim (hal_radio_t.ref_trim, radio-features.md 6.1): no reference control is known, so each synthesizer
// is programmed at f x (1 + trim): side A's and B's LOs, the air band LO and the transmitter. The IF chips keep their IF:
// every chip runs from the one reference, so the IF's share of the error cancels
static volatile int16_t trim_ppb;

static uint32_t synth_hz(uint32_t hz)
{
	return ref_trim_tune_hz(hz, trim_ppb);
}

#if CONFIG_REF_TRIM
// Taken at the next tune, so the RF chips' bus stays the radio task's: true asks the core for one
static bool ref_trim(int16_t ppb)
{
	trim_ppb = ppb;
	return true;
}
#endif

// The radio task and the GPS task both change chip 0's output register: each read-modify-write of its shadow runs
// under the bus lock, so neither loses the other's bits
static void chip0_gpio(uint16_t mask, uint16_t val)
{
	uint32_t s = bus_lock();
	qa4818_gpio(&chip_if, mask, val);
	bus_unlock(s);
}

#if CONFIG_GPS
static bool chips_ready;	// init() has set chip 0 up: its output register can be written
static bool gps_power;		// the GPS module's enable as last asked, applied once the chips are ready

void d578uv_gps_power(bool on)
{
	uint32_t s = bus_lock();
	gps_power = on;
	if (chips_ready) {
		qa4818_gpio(&chip_if, GPIO_GPS, on ? GPIO_GPS : 0);
	}
	bus_unlock(s);
}
#endif

// ---- Receive and transmit ----

// Side A's band pair; side B's is b_band_bits()
static void band_bits(uint32_t hz)
{
	chip0_gpio(GPIO_BAND_A_MASK, hz >= UHF_FROM_HZ ? GPIO_BAND_A_UHF : GPIO_BAND_A_VHF);
}

static qa4818_tone_t chip_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS:
		return QA4818_TONE_CTCSS;
	case TONE_DCS:
		return QA4818_TONE_DCS;
	case TONE_DCS_INV:
		return QA4818_TONE_DCS_INV;
	default:
		return QA4818_TONE_NONE;
	}
}

static void pa_off(void)
{
	mcu_pin_write(PIN_TX_ENABLE, false);
	mcu_dac_write(0, 0);
}

#if CONFIG_DMR
// ---- DMR: the PA per burst (pa_gate.h) ----

#define BURST_LINE		(1u << 0)		// EXTI 0: PD0
#define BURST_TIM_N		2u			// TIM2 (the GD32 manual's TIMER1)
#define BURST_TIMER		MCU_TIM(BURST_TIM_N)
// PE6 stays on this long after a burst's end: the stock TIM2 at 120 MHz, prescaler 0x77 and period 0x31F
#define BURST_TAIL_US		800u
// Above the RTOS's syscall level: the handlers make no RTOS calls, and a critical section mustn't delay the PA edge
#define BURST_IRQ_PRIORITY	3

// The guard: true from the key-up until tx_off(). The handlers read it before switching anything on, so once it is
// false (tx_off, board_rf_safe) a late edge or timer can only switch the PA off
static volatile bool burst_keyed;
static volatile uint16_t burst_dac;	// the DAC word while a burst goes out
static volatile uint32_t burst_count;	// rising edges while keyed (diag_d578uv.c)
static uint32_t burst_refused;		// key-ups refused with the burst line high (pa_gate_can_key)

static void burst_timer_stop(void)
{
	MCU_TIM_CR1(BURST_TIMER) &= ~MCU_TIM_CR1_CEN;
	MCU_TIM_SR(BURST_TIMER) = 0;
}

// From the handlers and, with them masked, the task: plain register writes
static void burst_apply(pa_gate_t a)
{
	switch (a) {
	case PA_GATE_ON:
		burst_timer_stop();
		mcu_dac0_put(burst_dac);
		mcu_pin_write(PIN_TX_ENABLE, true);
		break;
	case PA_GATE_DOWN:
		mcu_dac0_put(0);
		MCU_TIM_CNT(BURST_TIMER) = 0;
		MCU_TIM_CR1(BURST_TIMER) |= MCU_TIM_CR1_CEN;
		break;
	case PA_GATE_OFF:
		burst_timer_stop();
		mcu_dac0_put(0);
		mcu_pin_write(PIN_TX_ENABLE, false);
		break;
	case PA_GATE_NONE:
		break;
	}
}

void MCU_EXTI0_HANDLER(void)
{
	mcu_exti_clear(BURST_LINE);
	pa_gate_t a = pa_gate_edge(burst_keyed, mcu_pin_read(PIN_DMR_TX_BURST));
	if (a == PA_GATE_ON) {
		burst_count++;
	}
	burst_apply(a);
}

void MCU_TIM2_HANDLER(void)
{
	MCU_TIM_SR(BURST_TIMER) = 0;
	burst_apply(pa_gate_timer(burst_keyed, mcu_pin_read(PIN_DMR_TX_BURST)));
}

// PD0 an input with pull-up (the stock set-up) on EXTI 0, both edges, its interrupt off until a key-up; TIM2 a
// one-shot of BURST_TAIL_US
static void burst_init(void)
{
	mcu_pin_in(PIN_DMR_TX_BURST, MCU_PULL_UP);
	mcu_exti_route(GPIOD, 0);
	mcu_exti_disable(BURST_LINE);
	mcu_exti_both_edges(BURST_LINE);
	mcu_exti_clear(BURST_LINE);
	NVIC_SetPriority(MCU_EXTI0_IRQn, BURST_IRQ_PRIORITY);
	NVIC_EnableIRQ(MCU_EXTI0_IRQn);

	mcu_tim_clock_on(BURST_TIM_N);
	MCU_TIM_CR1(BURST_TIMER) = MCU_TIM_CR1_URS | MCU_TIM_CR1_OPM;
	MCU_TIM_PSC(BURST_TIMER) = SystemCoreClock / 1000000u - 1u;	// 1 us ticks (the timer clock is the core's)
	MCU_TIM_ARR(BURST_TIMER) = BURST_TAIL_US - 1u;
	MCU_TIM_EGR(BURST_TIMER) = MCU_TIM_EGR_UG;			// load them; URS keeps it from interrupting
	MCU_TIM_SR(BURST_TIMER) = 0;
	MCU_TIM_DIER(BURST_TIMER) = MCU_TIM_DIER_UIE;
	NVIC_SetPriority(MCU_TIM2_IRQn, BURST_IRQ_PRIORITY);
	NVIC_EnableIRQ(MCU_TIM2_IRQn);
}

static void burst_arm(uint16_t dac_word)
{
	burst_dac = dac_word;
	mcu_exti_clear(BURST_LINE);
	__disable_irq();
	burst_keyed = true;
	mcu_exti_enable(BURST_LINE);	// an edge from here on is pending: it follows
	__enable_irq();
}

// The guard first, so a handler that runs before the line is off can only switch off
static void burst_disarm(void)
{
	burst_keyed = false;
	__DSB();
	mcu_exti_disable(BURST_LINE);
	mcu_exti_clear(BURST_LINE);
	burst_apply(PA_GATE_OFF);
}

void d578uv_pa_counts(uint32_t *bursts, uint32_t *refused)
{
	*bursts = burst_count;
	*refused = burst_refused;
}

void d578uv_pa_counts_reset(void)
{
	burst_count = 0;
	burst_refused = 0;
}
#endif

// The stock RX set-up, side A: LO and IF tuned, path constants, tone, gain, both chips on, band bits, audio paths
static void rx_setup(void)
{
	bool digital = mode == RADIO_MODE_DMR;
	qa4818_rx_plan_t plan;
	qa4818_rx_plan(synth_hz(rx_hz), IF_A_HZ, &plan);
	qa4818_tune(&chip_lo, plan.lo_subband, plan.lo_hz);
	qa4818_tune(&chip_if, QA4818_SUBBAND_IF, IF_A_HZ);
	qa4818_path(&chip_if, false, digital, rx_bw == RADIO_BW_12K5, R49);
	qa4818_rx_setup(&chip_if, digital);
	qa4818_rx_tone(&chip_if, digital ? QA4818_TONE_NONE : chip_tone(rx_tone.type), rx_tone.value);
	uint8_t code = rx_gain_code(rx_hz, false);
	qa4818_gain(&chip_if, code);
	rx_threshold = code < QA4818_RSSI_FLOOR + 3u ? QA4818_RSSI_FLOOR : (uint8_t)(code - 3u);
	qa4818_out(&chip_lo, QA4818_OUT_LO);
	qa4818_out(&chip_if, QA4818_OUT_RX);
	qa4818_lo_on(&chip_lo);
	qa4818_rx_on(&chip_if);
	band_bits(rx_hz);
	trims_a(rx_hz);
	chip0_gpio(GPIO_MIC_MUTE, GPIO_MIC_MUTE);
	pa_off();
	mcu_pin_write(PIN_RF_SPLIT, false);
	sr_set((uint8_t)(digital ? (SR_RX & ~(SR_ANALOG | SR_RF_AUDIO)) : SR_RX),
	       (uint8_t)(SR_TX | (digital ? (SR_ANALOG | SR_RF_AUDIO) : 0)));
	audio_route();		// side B may need the RF audio with side A on DMR
}

#if CONFIG_DUAL_WATCH
// ---- Side B (hal_rx2.h) ----

// In AM side B's chips carry the air band, just below VHF: the VHF pair, the nearest filter (inferred; the stock AM
// set-up's band bits aren't traced)
static void b_band_bits(void)
{
	uint16_t v = AM_ON ? GPIO_BAND_B_VHF : !b_on ? 0 : b_hz >= UHF_FROM_HZ ? GPIO_BAND_B_UHF : GPIO_BAND_B_VHF;
	chip0_gpio(GPIO_BAND_B_MASK, v);
}

static void b_chips_off(void)
{
	qa4818_enable(&chip_b, QA4818_ENABLE_OFF);
	qa4818_enable(&chip_lo_b, QA4818_ENABLE_OFF);
}

// The stock side B set-up, FM: side A's receive steps on chips 3 and 1 at side B's IF, the tone generator off, then
// side B's band pair. Its squelch starts closed
static void b_setup(void)
{
	qa4818_rx_plan_t plan;
	b_trim = trim_ppb;
	qa4818_rx_plan(synth_hz(b_hz), IF_B_HZ, &plan);
	qa4818_tune(&chip_lo_b, plan.lo_subband, plan.lo_hz);
	qa4818_tune(&chip_b, QA4818_SUBBAND_IF, IF_B_HZ);
	qa4818_path(&chip_b, false, false, b_bw == RADIO_BW_12K5, R49);
	qa4818_rx_setup(&chip_b, false);
	qa4818_tone_gen(&chip_b, false);
	qa4818_rx_tone(&chip_b, chip_tone(b_tone.type), b_tone.value);
	qa4818_gain(&chip_b, rx_gain_code(b_hz, true));
	qa4818_out(&chip_lo_b, QA4818_OUT_LO);
	qa4818_out(&chip_b, QA4818_OUT_RX);
	qa4818_lo_on(&chip_lo_b);
	qa4818_rx_on(&chip_b);
	b_band_bits();
	b_open = false;
	b_close_polls = 0;
}

// Side A transmits: side B stops until tx_off()
static void b_pause(void)
{
	if (b_on) {
		b_chips_off();
		b_open = false;
	}
}

static void b_resume(void)
{
	if (b_on && !AM_ON) {
		b_setup();
	}
}

// FM only: how the one baseband takes side B's DMR isn't known, nor does the core have a DMR path for it
// (hardware-d578uv.md 5.2, 9). The same settings again write nothing, unless the reference trim changed since: the
// core's refresh then takes it
static bool b_tune(const rx2_tune_t *t)
{
	b_task = xTaskGetCurrentTaskHandle();
	b_squelch = t->squelch;
	if (t->mode != RADIO_MODE_FM || !board_in_band(t->rx_hz, false)) {
		return false;
	}
	if (b_on && t->rx_hz == b_hz && t->bw == b_bw && t->rx_tone.type == b_tone.type &&
			t->rx_tone.value == b_tone.value && b_trim == trim_ppb) {
		return true;
	}
	b_hz = t->rx_hz;
	b_bw = t->bw;
	b_tone = t->rx_tone;
	b_on = true;
	if (!transmitting && !AM_ON) {
		b_setup();
	}
	audio_route();
	return true;
}

static void b_stop(void)
{
	if (!b_on) {
		return;
	}
	b_on = false;
	b_open = false;
	if (!AM_ON) {
		b_chips_off();
	}
	b_band_bits();
	audio_route();
}

static bool tone_found(uint8_t type, uint16_t flags)
{
	switch (type) {
	case TONE_CTCSS:
		return (flags & 0x0001u) != 0;
	case TONE_DCS:
	case TONE_DCS_INV:
		return (flags & 0x0002u) != 0;
	default:
		return true;
	}
}

// The radio task polls the chip and runs the squelch (the core's levels: open below the level's noise, closed after
// RADIO_SQL_CLOSE_POLLS polls RADIO_SQL_HYST_DB above it, or without the tone); any other task gets the last copy
static void b_status(rx2_status_t *st)
{
	if (b_task != NULL && xTaskGetCurrentTaskHandle() == b_task) {
		rx2_status_t n = { .up = true, .tuned = b_on, .rssi_dbm = -160, .noise = 127 };
		if (b_on && !transmitting && !AM_ON) {
			qa4818_status_t s;
			qa4818_status(&chip_b, &s);
			bool was = b_open;
			uint8_t open_below = radio_sm_squelch_open_below(b_squelch);
			bool tone = tone_found(b_tone.type, s.tone_flags);
			if (!b_open) {
				b_open = s.noise < open_below && tone;
				b_close_polls = 0;
			} else if (!tone || s.noise >= open_below + RADIO_SQL_HYST_DB) {
				if (++b_close_polls >= RADIO_SQL_CLOSE_POLLS) {
					b_open = false;
				}
			} else {
				b_close_polls = 0;
			}
			if (b_open != was) {
				audio_route();
			}
			n.rssi_dbm = qa4818_rssi_dbm(s.rssi);
			n.noise = s.noise;
			n.active = b_open;
			n.heard = b_heard();
		}
		uint32_t saved = bus_lock();
		b_st = n;
		bus_unlock(saved);
	}
	uint32_t saved = bus_lock();
	*st = b_st;
	bus_unlock(saved);
}

static void b_audio_on(bool on)
{
	b_audio = on;
	audio_route();
}

const hal_rx2_t d578uv_rx2 = {
	.tune = b_tune,
	.stop = b_stop,
	.status = b_status,
	.audio = b_audio_on,
};
#else
static void b_pause(void) {}
static void b_resume(void) {}
#endif

#if KT_USED
// ---- The KTMicro receiver's bus and level (5.4) ----

#define KT_TRIES		3u	// the stock firmware retries a refused address up to 400 times

static void kt_scl(bool high) { mcu_pin_write(PIN_KT_SCL, high); }
static bool kt_sda_in(void) { return mcu_pin_read(PIN_KT_SDA); }

// Released, the pin's pull-up takes the line high (the stock firmware's input mode)
static void kt_sda(bool high)
{
	if (high) {
		mcu_pin_in(PIN_KT_SDA, MCU_PULL_UP);
	} else {
		mcu_pin_out(PIN_KT_SDA, false, false);
	}
}

static const i2c_gpio_t kt_i2c = {
	.scl = kt_scl, .sda = kt_sda, .sda_in = kt_sda_in, .delay_us = hal_delay_us, .half_us = 4,
};

static bool kt_read(uint8_t reg, uint8_t *val)
{
	for (uint32_t i = 0; i < KT_TRIES; i++) {
		if (i2c_gpio_write_read(&kt_i2c, KTAMFM_I2C_ADDR, &reg, 1, val, 1)) {
			return true;
		}
	}
	return false;
}

static bool kt_write(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	for (uint32_t i = 0; i < KT_TRIES; i++) {
		if (i2c_gpio_write(&kt_i2c, KTAMFM_I2C_ADDR, b, sizeof(b))) {
			return true;
		}
	}
	return false;
}

// The receiver's power-up waits (50 ms the longest) sleep; the short ones spin
static void kt_delay(uint32_t us)
{
	if (us >= 1000u) {
		vTaskDelay(pdMS_TO_TICKS((us + 999u) / 1000u));
	} else {
		hal_delay_us(us);
	}
}

static const ktamfm_bus_t kt_bus = { .read = kt_read, .write = kt_write, .delay_us = kt_delay };

// The pins, a bus recovery and the ID read (none of it waits for long)
static void kt_attach(void)
{
	mcu_pin_out(PIN_KT_SCL, true, false);
	kt_sda(true);
	i2c_gpio_recover(&kt_i2c);
	ktamfm_attach(&kt, &kt_bus);
	kt_present = ktamfm_probe(&kt);
}

// The receiver's level: AM's while AM is on and the core gives the main side the speaker, broadcast FM's while the core
// plays it, muted otherwise (the stock squelch mutes it the same way). Only once it is set up. Radio task only
static void kt_level(void)
{
	if (!kt.ready) {
		return;
	}
	uint8_t want = KTAMFM_VOL_MUTE;
	if (AM_ON) {
		want = main_audio ? KTAMFM_VOL_AM : KTAMFM_VOL_MUTE;
	}
#if CONFIG_BCAST_FM
	else if (bc_on && bc_audio && !bc_silent) {
		want = KTAMFM_VOL_FM;
	}
#endif
	if (want != kt_level_out) {
		kt_level_out = ktamfm_volume(&kt, want) ? want : 0xFFu;	// a refused write is tried again
	}
}

// Set up once (the stock set-up ends muted)
static bool kt_init(void)
{
	bool was = kt.ready;
	if (!ktamfm_init(&kt)) {
		return false;
	}
	if (!was) {
		kt_level_out = KTAMFM_VOL_MUTE;
	}
	return true;
}
#endif

#if CONFIG_AM_RX
// ---- Air band AM: chip 3 and the KTMicro receiver (5.4) ----

// The core's AM squelch level 1 opens above this RSSI (radio_am.h); the stock level 1 opens at the calibrated level
// (VHF block byte 0x1A) and each level adds 3 counts, as the core's add 3 dB
#define AM_LEVEL1_DBM		(-82)
#define BLK_AM_CARRIER		0x19u	// the VHF block's floor for the receiver's 0xEC reading
#define BLK_AM_LEVEL		0x1Au	// its level-1 point for the 0xEA reading

static void chips_off(void);

// The stock AM set-up: side B's chips off, the receiver set up and tuned to the IF, chip 3 to the LO and on. Chips 0
// and 2 are off too: the main receiver is the AM one. The LO below the signal, the stock "AM Offset" default
// (rf_plan.h); shift register bit 0x04 set, as the stock firmware sets it whenever a side is in AM
static bool rx_am(uint32_t hz, radio_bw_t bw)
{
	(void)bw;	// the receiver's AM filter is its own
	if (hz < 108000000u || hz >= 137000000u || !kt_present || cal_base == 0 || transmitting) {
		return false;	// the squelch needs the calibrated floors
	}
	if (!kt_init() || !ktamfm_am(&kt) || !ktamfm_tune_am(&kt, AM_IF_HZ / 1000u)) {
		return false;
	}
	chips_off();
	qa4818_enable(&chip_b, QA4818_ENABLE_OFF);
	qa4818_enable(&chip_lo_b, QA4818_ENABLE_OFF);
	qa4818_tone_gen(&chip_b, false);
	am_on = true;
#if CONFIG_DUAL_WATCH
	b_open = false;
	b_band_bits();
#endif
	qa4818_gpio(&chip_lo_b, GPIO_LO_B_AM, GPIO_LO_B_AM);
	uint32_t lo = am_lo_hz(synth_hz(hz), true);
	qa4818_tune(&chip_lo_b, am_lo_subband(lo), lo);
	qa4818_out(&chip_lo_b, QA4818_OUT_LO);
	qa4818_enable(&chip_lo_b, QA4818_ENABLE_LO);
	rx_hz = hz;
	rx_tone = (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	sr_set(SR_ANALOG, 0);
	trims_b();
	kt_level();
	audio_route();
	return true;
}

// Back from AM before side A tunes again: the receiver muted, chip 3 off and its AM bit clear, side B again if tuned.
// True when AM was on
static bool am_leave(void)
{
	if (!am_on) {
		return false;
	}
	am_on = false;
	trims_b();
	kt_level();
	qa4818_enable(&chip_lo_b, QA4818_ENABLE_OFF);
	qa4818_gpio(&chip_lo_b, GPIO_LO_B_AM, 0);
#if CONFIG_DUAL_WATCH
	b_band_bits();		// side B's own pair again, or none
#endif
	b_resume();
	return true;
}

// The receiver's readings: no carrier below the calibrated floor, else the level relative to the calibrated level-1 point
static void am_status(radio_status_t *st)
{
	ktamfm_status_t k;
	bool carrier = ktamfm_status(&kt, &k) && k.carrier > cal_vhf[BLK_AM_CARRIER];
	st->rssi_dbm = carrier ? (int16_t)(AM_LEVEL1_DBM + (int)k.level - (int)cal_vhf[BLK_AM_LEVEL]) : -160;
	st->noise = 127;
	st->carrier = carrier;
	st->tone_ok = true;
}
#else
static bool am_leave(void) { return false; }
#endif

#if CONFIG_BCAST_FM
// ---- Broadcast FM: the KTMicro receiver's FM mode (hal_bcast.h, 5.4) ----

#define BC_LO_KHZ		87500u	// the stock firmware's band and default
#define BC_HI_KHZ		108000u

// On: taken back from AM first (chip 3 off, side B again), then the stock broadcast set-up: chip 3's output bit 6
// clear, the receiver set up once, FM mode, the last frequency. Muted until audio(true). Off: muted, and the receiver
// stays powered as the stock firmware leaves it (no standby write is known); an AM set-up made since is left alone
static bool bc_power(bool on)
{
	if (!on) {
		bc_on = bc_audio = false;
		trims_b();
		kt_level();
		audio_route();
		return true;
	}
	if (!kt_present) {
		return false;
	}
	am_leave();
#if SIDE_B_CHIPS
	qa4818_gpio(&chip_lo_b, GPIO_LO_B_AM, 0);
#endif
	if (!kt_init() || !ktamfm_fm(&kt)) {
		return false;
	}
	if (bc_khz == 0) {
		bc_khz = BC_LO_KHZ;
	}
	if (!ktamfm_tune_fm(&kt, bc_khz)) {
		return false;
	}
	bc_on = true;
	bc_audio = false;
	trims_b();
	kt_level();
	return true;
}

static bool bc_tune(uint32_t khz)
{
	if (!bc_on || khz < BC_LO_KHZ || khz > BC_HI_KHZ || !ktamfm_tune_fm(&kt, khz)) {
		return false;
	}
	bc_khz = khz;
	return true;
}

// The receiver has no seek the stock firmware uses (its scan steps the frequency); the core doesn't seek
static bool bc_seek(bool up)
{
	(void)up;
	return false;
}

// The level device after the receiver follows the radio's volume (volume()), as for AM: the receiver stays at the stock
// broadcast level, muted only at 0
static void bc_volume(uint8_t level)
{
	bc_silent = level == 0;
	kt_level();
}

// No tune-done or stereo reading is known: a tune counts as done at once, mono. station is the stock test (its 3-poll
// debounce left to the caller), rssi the receiver's 0xE6 reading (its scale against dBuV isn't known)
static bool bc_status(bcast_status_t *st)
{
	ktamfm_fm_status_t f;
	if (!bc_on || !ktamfm_fm_status(&kt, &f)) {
		return false;
	}
	st->khz = bc_khz;
	st->tuning = false;
	st->seek_failed = false;
	st->stereo = false;
	st->station = f.station;
	st->rssi = f.rssi;
	return true;
}

static void bc_audio_on(bool on)
{
	bc_audio = on && bc_on;
	kt_level();
	audio_route();
}

const hal_bcast_t d578uv_bcast = {
	.lo_khz = BC_LO_KHZ,
	.hi_khz = BC_HI_KHZ,
	.grid_khz = KTAMFM_FM_STEP_KHZ,
	.power = bc_power,
	.tune = bc_tune,
	.seek = bc_seek,
	.volume = bc_volume,
	.status = bc_status,
	.audio = bc_audio_on,
};
#endif

static void init(void)
{
	mcu_pin_out(PIN_TX_ENABLE, false, false);
	mcu_dac_init(0);
	mcu_pin_out(PIN_RF_SPLIT, false, false);
	mcu_pin_out(PIN_SPK_MUTE, true, false);
	mcu_pin_out(PIN_SR_LATCH, true, false);
	sr = SR_BOOT | SR_RF_AUDIO;
	sr_shift(sr, false);

	d578uv_bus_init();

	// Chips in the stock order: the two LOs, then side B's and side A's transceivers. Side B stays off until the core
	// tunes it (kept only with CONFIG_DUAL_WATCH or CONFIG_AM_RX)
#if SIDE_B_CHIPS
	rfbus_3w_t *b1 = &bus_b, *b3 = &bus_lo_b;
	qa4818_t *c1 = &chip_b, *c3 = &chip_lo_b;
#else
	rfbus_3w_t bus_b, bus_lo_b;
	qa4818_t chip_b, chip_lo_b;
	rfbus_3w_t *b1 = &bus_b, *b3 = &bus_lo_b;
	qa4818_t *c1 = &chip_b, *c3 = &chip_lo_b;
#endif
	rfbus_3w_attach(&bus_lo, &pins_chip[2]);
	rfbus_3w_attach(b3, &pins_chip[3]);
	rfbus_3w_attach(b1, &pins_chip[1]);
	rfbus_3w_attach(&bus_if, &pins_chip[0]);
	qa4818_attach(&chip_lo, &bus_lo.bus, true);
	qa4818_attach(c3, &b3->bus, true);
	qa4818_attach(c1, &b1->bus, false);
	qa4818_attach(&chip_if, &bus_if.bus, false);
	qa4818_init_lo(&chip_lo);
	qa4818_init_lo(c3);
	qa4818_init_trx(c1, GPIO_B_INIT, R49);
	qa4818_init_trx(&chip_if, GPIO_A_INIT, R49);
#if CONFIG_GPS
	// The GPS task may have asked for the module's power before the chip was set up (gps_d578uv.c)
	uint32_t s = bus_lock();
	chips_ready = true;
	qa4818_gpio(&chip_if, GPIO_GPS, gps_power ? GPIO_GPS : 0);
	bus_unlock(s);
#endif
	qa4818_enable(c1, QA4818_ENABLE_OFF);
	qa4818_enable(c3, QA4818_ENABLE_OFF);

	// The PA5 device's start-up writes, in the stock order. The stock firmware writes register 8 (the backlight) 0
	// here and lights it after its splash; the level the start-up already set is kept instead, so the screen doesn't
	// go dark while the radio starts
	static const uint8_t aux_init[][2] = {
		{ 0x9, 0 }, { 0x3, 0xFF }, { 0xF, 0xFF }, { AUX_BACKLIGHT, 0 }, { 0xC, 0 }, { 0x2, 0 }, { 0xA, 0 },
		{ 0x6, 0 }, { 0xE, 0 }, { 0x1, 0 },
	};
	for (size_t i = 0; i < sizeof(aux_init) / sizeof(aux_init[0]); i++) {
		aux_write(aux_init[i][0], aux_init[i][0] == AUX_BACKLIGHT ? backlight_reg : aux_init[i][1]);
	}

#if D578UV_LEVEL_IC
	mcu_pin_out(PIN_LEVEL_DATA, false, false);
	mcu_pin_out(PIN_LEVEL_CLK, false, false);
	mcu_swd_release();
#endif
	cal_load();
#if CONFIG_DMR
	burst_init();
#endif
#if KT_USED
	kt_attach();
#endif
}

static void set_mode(radio_mode_t m)
{
	if (!am_leave() && m == mode) {
		return;
	}
	mode = m;
	if (have_rx && !transmitting) {
		rx_setup();
	}
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	if (!board_in_band(hz, false)) {
		return false;
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = t != NULL ? *t : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	am_leave();
	if (!transmitting) {
		rx_setup();
	}
	return true;
}

static void chips_off(void)
{
	qa4818_enable(&chip_if, QA4818_ENABLE_OFF);
	qa4818_enable(&chip_lo, QA4818_ENABLE_OFF);
}

#if CONFIG_DMR
// The stock digital TX-on (5.1): chips off, chip 0 to hz with the digital set, keyed, band and TX bits, 10 ms, then the
// PA is the bursts'. Called before the modem's transmission starts (the core's order), held until tx_off()
static bool dmr_tx(uint32_t hz, uint8_t dac)
{
	if (!pa_gate_can_key(mcu_pin_read(PIN_DMR_TX_BURST))) {
		burst_refused++;
		return false;
	}
	transmitting = true;
	sr_set(SR_TX, 0);
	chips_off();
	b_pause();
	qa4818_tune(&chip_if, qa4818_tx_subband(hz), synth_hz(hz));
	qa4818_path(&chip_if, true, true, true, R49);
	qa4818_dmr_tx(&chip_if, hz);
	qa4818_tx_on(&chip_if);
	band_bits(hz);
	chip0_gpio(GPIO_TX_UHF | GPIO_LED_TX | GPIO_MIC_MUTE, (hz > UHF_FROM_HZ ? GPIO_TX_UHF : 0) | GPIO_LED_TX);
	hal_delay_ms(10);
	// The receive path bit before the PA can come on (the stock firmware clears it after its PE6 step)
	sr_set(SR_TX, SR_RX_PATH);
	burst_arm((uint16_t)(dac << 4));
	return true;
}
#endif

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *t)
{
	if (!board_in_band(hz, true)) {
		return false;
	}
	uint8_t dac = tx_dac(hz, power);
	if (dac == 0) {
		return false;
	}
#if CONFIG_DMR
	burst_disarm();		// keyed again without tx_off(): no burst switches the PA of the new set-up
#endif
#if CONFIG_DTMF || CONFIG_APRS
	d578uv_tone_stop();
#endif
	aux = AUX_NONE;
	// The stock TX start's register 4 (by band and level; its turbo one level less with dual receive on isn't done)
	aux_write(AUX_TRIM_TX, trim_tx(hz / 10u, power, cal_uhf[BLK_TRIM_TX], cal_vhf[BLK_TRIM_TX]));
	if (mode == RADIO_MODE_DMR) {
#if CONFIG_DMR
		return dmr_tx(hz, dac);
#else
		return false;
#endif
	}
	bool uhf = hz > UHF_FROM_HZ;
	bool narrow = bw == RADIO_BW_12K5;
	qa4818_tone_t tone = t != NULL ? chip_tone(t->type) : QA4818_TONE_NONE;
	bool dcs = tone == QA4818_TONE_DCS || tone == QA4818_TONE_DCS_INV;
	uint8_t lvl_ctcss = cal_uhf[BLK_TONE_CTCSS], lvl_dcs = cal_uhf[BLK_TONE_DCS];

	uint8_t drive = (uhf ? cal_uhf : cal_vhf)[BLK_DRIVE];
	tx_hz = hz;
	tx_narrow = narrow;
	tx_r40 = qa4818_drive_word(hz, narrow, drive);
	tx_level = dcs ? lvl_dcs : lvl_ctcss;

	transmitting = true;
	sr_set(SR_TX, 0);
	chips_off();
	b_pause();
	qa4818_tune(&chip_if, qa4818_tx_subband(hz), synth_hz(hz));
	qa4818_path(&chip_if, true, false, narrow, R49);
	qa4818_fm_tx(&chip_if, hz);
	qa4818_tone_gen(&chip_if, false);	// the stock analog TX-on: no burst left on from a previous key-up
	qa4818_tx_tone(&chip_if, tone, t != NULL ? t->value : 0, lvl_ctcss, lvl_dcs);
	qa4818_tx_drive(&chip_if, hz, narrow, drive, tx_level);
	qa4818_tx_on(&chip_if);
	band_bits(hz);
	chip0_gpio(GPIO_TX_UHF | GPIO_LED_TX | GPIO_MIC_MUTE, (hz > UHF_FROM_HZ ? GPIO_TX_UHF : 0) | GPIO_LED_TX);
	hal_delay_ms(10);
	mcu_dac_write(0, (uint16_t)(dac << 4));
	sr_set(SR_TX, SR_RX_PATH);
	// The PA enable toggled three times 2 ms apart, then held (the stock sequence)
	for (int i = 0; i < 3; i++) {
		mcu_pin_write(PIN_TX_ENABLE, true);
		hal_delay_ms(2);
		mcu_pin_write(PIN_TX_ENABLE, false);
		hal_delay_ms(2);
	}
	mcu_pin_write(PIN_TX_ENABLE, true);
	return true;
}

// The microphone again after a tone: the stock order, the chip's voice set-up first, then the mute bit
static void mic_again(void)
{
#if CONFIG_DTMF || CONFIG_APRS
	if (aux == AUX_LINE) {
		d578uv_tone_stop();
	}
#endif
	qa4818_tx_mic(&chip_if, tx_r40, tx_level);
	chip0_gpio(GPIO_MIC_MUTE, 0);
	aux = AUX_NONE;
}

static bool fm_keyed(void)
{
	return transmitting && mode == RADIO_MODE_FM;
}

// The stock 1750 Hz burst: tone drive by band, page 1 0x1B = 5, the microphone muted, then the generator (5.3). Any
// tone the generator makes; the core sends 1750 Hz
static bool tx_tone(uint16_t hz)
{
	if (!fm_keyed()) {
		return false;
	}
	if (hz == 0) {
		if (aux != AUX_NONE) {
			mic_again();
		}
		return true;
	}
	if (aux != AUX_TONE) {
		if (aux != AUX_NONE) {
			mic_again();
		}
		qa4818_tx_aux(&chip_if, false, qa4818_tone_drive_word(tx_hz, tx_narrow));
		chip0_gpio(GPIO_MIC_MUTE, GPIO_MIC_MUTE);
		aux = AUX_TONE;
	}
	qa4818_tone_start(&chip_if, hz);
	return true;
}

// A silent carrier between the marks: the generator off, the microphone still muted
static bool tx_tone_key(bool down)
{
	if (!fm_keyed() || aux != AUX_TONE) {
		return false;
	}
	qa4818_tone_gen(&chip_if, down);
	return true;
}

#if CONFIG_DTMF || CONFIG_APRS
// The stock DTMF set-up: the chip's audio input pin at the calibrated DTMF drive, the microphone muted, for PB9's
// audio (tone_d578uv.c). APRS sends its tones the same way; the deviation that gives them is inferred
static void line_in(void)
{
	if (aux != AUX_LINE) {
		if (aux != AUX_NONE) {
			mic_again();
		}
		uint8_t level = cal_uhf[tx_hz > 200000000u ? BLK_DTMF_UHF : BLK_DTMF_VHF];
		qa4818_tx_aux(&chip_if, true, qa4818_line_drive_word(tx_hz, tx_narrow, level));
		chip0_gpio(GPIO_MIC_MUTE, GPIO_MIC_MUTE);
		aux = AUX_LINE;
	}
}
#endif

#if CONFIG_APRS
bool d578uv_line_in(bool on)
{
	if (!on) {
		if (aux == AUX_LINE && fm_keyed()) {
			mic_again();
		}
		return true;
	}
	if (!fm_keyed()) {
		return false;
	}
	line_in();
	return true;
}
#endif

#if CONFIG_DTMF
// The stock DTMF: the line input and the pair from PB9. A new digit only changes the pair
static bool tx_dtmf(char digit)
{
	if (!fm_keyed()) {
		return false;
	}
	if (digit == 0) {
		if (aux != AUX_NONE) {
			mic_again();
		}
		return true;
	}
	uint16_t lo, hi;
	if (!dtmf_pair(digit, &lo, &hi)) {
		return false;
	}
	line_in();
	d578uv_dtmf_start(lo, hi);
	return true;
}
#endif

static void tx_off(void)
{
#if CONFIG_DMR
	burst_disarm();
#endif
	sr_set(0, SR_TX);
	pa_off();
	if (aux != AUX_NONE) {
		mic_again();	// the chip's voice path for the next key-up, and the tone generator off
	}
	sr_set(0, SR_RX_AUDIO);
	qa4818_tx_drive_off(&chip_if);
	chip0_gpio(GPIO_TX_UHF | GPIO_LED_TX | GPIO_MIC_MUTE, GPIO_MIC_MUTE);
	chips_off();
	transmitting = false;
	if (have_rx) {
		rx_setup();
	}
	b_resume();
}

// Side A's last raw RSSI (bits 15:0), noise (30:16) and signal flag (31), as status() read them for the radio task
static volatile uint32_t diag_rx;

void d578uv_rx_raw(uint16_t *rssi, uint16_t *noise, bool *signal)
{
	uint32_t v = diag_rx;
	*rssi = (uint16_t)v;
	*noise = (uint16_t)((v >> 16) & 0x7FFFu);
	*signal = (v >> 31) != 0;
}

static void status(radio_status_t *st)
{
	if (transmitting || !have_rx) {
		st->rssi_dbm = -160;
		st->noise = 127;
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
#if CONFIG_AM_RX
	if (am_on) {
		am_status(st);
		return;
	}
#endif
	qa4818_status_t s;
	qa4818_status(&chip_if, &s);
	diag_rx = (uint32_t)s.rssi | (uint32_t)(s.noise & 0x7FFFu) << 16 | (s.signal ? 1u << 31 : 0u);
	st->rssi_dbm = qa4818_rssi_dbm(s.rssi);
	st->noise = s.noise;
	st->carrier = s.signal && s.rssi >= rx_threshold;
	switch (rx_tone.type) {
	case TONE_CTCSS:
		st->tone_ok = (s.tone_flags & 0x0001u) != 0;
		break;
	case TONE_DCS:
	case TONE_DCS_INV:
		st->tone_ok = (s.tone_flags & 0x0002u) != 0;
		break;
	default:
		st->tone_ok = true;
		break;
	}
}

static void audio(bool on)
{
	main_audio = on;
#if KT_USED
	kt_level();
#endif
	audio_route();
}

#if CONFIG_APRS_RX
bool d578uv_fm_rx_a(void)
{
	return have_rx && mode == RADIO_MODE_FM && !transmitting && !AM_ON;
}
#endif

// Side B's trims as the stock firmware sets them with side A the main channel: in AM register 2 the VHF block's 0x1B and
// 0xC the air band table at the AM frequency; with broadcast FM and no dual receive register 2 side B's 0x11; else
// both 0. (The stock firmware sets them by side B's frequency only with side B the main channel or keyed, which
// RadioFoundry doesn't do: side B is only ever a second receiver.) Radio task
#if CONFIG_AM_RX || CONFIG_BCAST_FM
static void trims_b(void)
{
	uint8_t r2 = 0, rc = 0;
	if (cal_base != 0 && AM_ON) {
		r2 = cal_vhf[BLK_TRIM_AM];
		rc = trim_am(cal_uhf + BLK_TRIM_AM, rx_hz / 10u);
	}
#if CONFIG_BCAST_FM
	else if (cal_base != 0 && bc_on && !B_ON) {
		r2 = cal_vhf[BLK_TRIM_B];
	}
#endif
	aux_write(AUX_TRIM_B, r2);
	aux_write(AUX_TRIM_B_TABLE, rc);
}
#endif

void d578uv_rf_safe(void)
{
#if CONFIG_DMR
	burst_keyed = false;	// a burst edge or the timer after this only switches off
	mcu_exti_disable(BURST_LINE);
#endif
	mcu_pin_force(PIN_TX_ENABLE, false);
#if CONFIG_DTMF || CONFIG_APRS
	d578uv_tone_stop();
#endif
	if (mcu_dac_clocked()) {
		mcu_dac_write(0, 0);
	}
	sr = (uint8_t)(sr & ~SR_TX);
	sr_shift(sr, true);
}

const hal_radio_t d578uv_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.tx_tone = tx_tone,
	.tx_tone_key = tx_tone_key,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
	.status = status,
	.audio = audio,
	.volume = volume,
#if CONFIG_AM_RX
	.rx_am = rx_am,
#endif
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
};
