// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV receive tap: side A's demodulated receive signal on PC0, ADC channel 10,
// sampled by the MCU. APRS receive reads it at 9600 Hz (afsk_d578uv.c) on an FM channel; P25 and NXDN receive read
// it at 16 kHz on a digital channel through the air tap (hal_baseband_t, core/air/air_tap.h), the only way this radio
// gets their symbols: the SCT3258 baseband gives no symbols of its own. One user at a time. The same file has the
// air tap's transmit side (below): the soft modem's samples go out on PB9, as DTMF and APRS do.
//
// TIM6 (the GD32 manual's TIMER5) interrupts at the rate. Its handler reads the conversion it started on the last tick
// and starts the next on the ADC's sampling channel (mcu_io.h: a channel with its own trigger and data register, so the
// key ladders' and knobs' reads go on beside it), and puts the reading in a ring (drivers/audio/adc_pcm) that a task
// reads as PCM with the bias removed. A DMA scan isn't used: the regular group the DMA would take is the one
// mcu_adc_read shares between tasks.
//
// Readings reach the reader only while side A receives in the reader's mode (d578uv_rx_a): while it transmits, or
// side B has the AM receiver or the baseband, the ring is emptied instead. PC0 carries side A's signal while chip 1's
// GPIO bit 6 is set, which radio_d578uv.c does in receive.
//
// On a digital channel PC0 carries chip 0's demodulator for the baseband (0x47 0x4840, inverted below 300 MHz so the
// polarity is one on both bands): the derivative of the deviation, flat past a few kHz. The
// air tap says so (rx_slope_hz) and the soft modem integrates it. Its scale isn't calibrated: the level is tracked.
// The tap never reaches the speaker: on a digital channel side A's RF audio gate stays shut (radio_d578uv.c), and the
// tap's start fails on any other.
#include "d578uv.h"
#include "config.h"

#if CONFIG_APRS_RX || CONFIG_AIR
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "audio/adc_pcm/adc_pcm.h"
#if CONFIG_AIR
#include "air/air_tap.h"
#endif

#define SAMPLE_TIM_N		6u
#define SAMPLE_TIMER		MCU_TIM(SAMPLE_TIM_N)
// The timer clock: board.c's core clock (the APB1 timers run at it)
#define TIMER_HZ		120000000u
// No RTOS calls in the handler; above the syscall level so no critical section delays a sample, which would show as
// jitter on the tones. The DMR burst and DTMF interrupts share the level: they run only while transmitting
#define SAMPLE_IRQ_PRIORITY	3
// 1024 readings: 64 ms at 16 kHz, 107 ms at 9600 Hz; the readers come every 2 ms (the air tap) or 25 ms (APRS)
#define RING_LEN		1024u

#if CONFIG_AIR
// While the tap is stopped for the air tap's transmit side, its memory holds that side's: PB9's duties and the soft
// modem's transmitter
#define TX_RING_LEN		1024u	// duties: 42 ms at 24 kHz (the air tap keeps 30 ms ahead)
static union {
	uint16_t rx[RING_LEN];
	struct {
		uint8_t duty[TX_RING_LEN];
		air_softmodem_tx_t sm;
	} tx;
} ring_mem;
_Static_assert(sizeof ring_mem.tx <= sizeof ring_mem.rx, "the transmit side fits the receive tap's ring");
#define ring_buf		ring_mem.rx
#else
static uint16_t ring_buf[RING_LEN];
#endif
static adc_pcm_t ring;
static volatile uint32_t ticks;		// readings taken since the start, dropped ones too
static uint32_t start_us, rate_hz, dropped_seen;
static bool running;

// Readings averaged into each sample at OVER_MIN_HZ and up (the air tap's 16 kHz): PC0 carries noise up to and past
// the sample rate, and a single reading folds it into the band (a hump at 19-24 kHz). Four readings at 64 kHz: NXDN
// at both rates and most P25 voice frames decode on the radio. APRS at 9600 Hz keeps single readings
#define OVER_N			4u
#define OVER_MIN_HZ		16000u
static uint8_t over_n, over_ph, over_got;
static bool over_started;		// the tick before started a conversion (mcu_adc_sample_start refuses during a read)
static uint32_t over_acc;
static uint16_t over_last;

// hot: in the GD32F303's fast flash (common.ld). From the slow flash at 64 kHz it starved the tasks: the watchdog
// fired, measured
__attribute__((hot)) void MCU_TIM6_HANDLER(void)
{
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	if (over_started) {
		over_acc += mcu_adc_sample_value();
		over_got++;
	}
	over_started = mcu_adc_sample_start();
	if (++over_ph < over_n) {
		return;
	}
	ticks++;
	// A sample whose readings a task's conversion took (mcu_io.h) repeats the last one
	if (over_got != 0u) {
		over_last = (uint16_t)((over_acc + over_got / 2u) / over_got);
	}
	adc_pcm_put(&ring, over_last);
	over_acc = 0;
	over_ph = 0;
	over_got = 0;
}

static void timer_stop(void)
{
	NVIC_DisableIRQ(MCU_TIM6_IRQn);
	MCU_TIM_DIER(SAMPLE_TIMER) = 0;
	MCU_TIM_CR1(SAMPLE_TIMER) = 0;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	NVIC_ClearPendingIRQ(MCU_TIM6_IRQn);
}

bool d578uv_rxtap_start(uint32_t hz)
{
	static bool pin_ready;
	if (running) {
		return false;
	}
	if (!pin_ready) {
		mcu_pin_analog(PIN_RX_AUDIO);
		mcu_adc_sample_init(ADC_RX_AUDIO);
		pin_ready = true;
	}
	mcu_tim_clock_on(SAMPLE_TIM_N);
	timer_stop();
	adc_pcm_init(&ring, ring_buf, RING_LEN, 12, 2048);
	ticks = 0;
	dropped_seen = 0;
	rate_hz = hz;
	over_started = mcu_adc_sample_start();		// the first tick reads a real conversion
	over_last = 2048u;
	MCU_TIM_CR1(SAMPLE_TIMER) = MCU_TIM_CR1_URS;
	MCU_TIM_PSC(SAMPLE_TIMER) = 0;
	over_n = hz >= OVER_MIN_HZ ? OVER_N : 1u;
	over_ph = 0;
	over_got = 0;
	over_acc = 0;
	MCU_TIM_ARR(SAMPLE_TIMER) = TIMER_HZ / (hz * over_n) - 1u;
	MCU_TIM_EGR(SAMPLE_TIMER) = MCU_TIM_EGR_UG;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	MCU_TIM_DIER(SAMPLE_TIMER) = MCU_TIM_DIER_UIE;
	NVIC_SetPriority(MCU_TIM6_IRQn, SAMPLE_IRQ_PRIORITY);
	NVIC_EnableIRQ(MCU_TIM6_IRQn);
	start_us = hal_micros();	// reading 0 is the conversion started above
	MCU_TIM_CR1(SAMPLE_TIMER) |= MCU_TIM_CR1_CEN;
	running = true;
	return true;
}

uint16_t d578uv_rxtap_read(int16_t *buf, uint16_t max, radio_mode_t mode)
{
	if (!running) {
		return 0;
	}
	// Not receiving in the reader's mode, or readings lost to a full ring: what is held goes. The next read starts
	// after the hole, and d578uv_rxtap_time_us says where
	uint32_t dropped = ring.dropped;
	if (!d578uv_rx_a(mode) || dropped != dropped_seen) {
		dropped_seen = dropped;
		adc_pcm_flush(&ring);
		return 0;
	}
	return adc_pcm_read(&ring, buf, max);
}

uint32_t d578uv_rxtap_time_us(void)
{
	uint32_t t, n;
	do {
		t = ticks;
		n = adc_pcm_count(&ring);
	} while (t != ticks);
	// The oldest reading held is reading t - n; a 64-bit divide, once a read
	return start_us + (uint32_t)((uint64_t)(t - n) * 1000000u / rate_hz);
}

void d578uv_rxtap_stop(void)
{
	if (running && mcu_tim_clocked(SAMPLE_TIM_N)) {
		timer_stop();
	}
	running = false;
}

#if CONFIG_AIR
uint8_t *d578uv_rxtap_mem(uint16_t *len)
{
	*len = (uint16_t)sizeof ring_mem.tx.duty;
	return running ? NULL : ring_mem.tx.duty;
}
#endif

#if CONFIG_AIR
// --- The air tap (hal_baseband_t receive, core/air/air_tap.h) ---
#define TAP_HZ			16000u
// The band PC0 carries on a digital channel, as the soft modem uses it: a derivative from
// about 25 Hz up to a few kHz. The integrator's leak sits at its low corner
#define TAP_LO_HZ		25u
#define TAP_HI_HZ		3000u
#define TAP_SLOPE_HZ		25u
// Which way PC0 swings for a deviation above the carrier is not measured: 1 negates the samples (D578UV_DEFS, as the
// other bench knobs in board.mk)
#ifndef D578UV_TAP_INVERT
#define D578UV_TAP_INVERT	0
#endif

static bool tap_start(void)
{
	// Rule 1 (hal_air.h): only on a digital channel, where side A's audio gate stays shut
	return d578uv_digital() && d578uv_rxtap_start(TAP_HZ);
}

static uint16_t tap_read(int16_t *buf, uint16_t max)
{
	uint16_t n = d578uv_rxtap_read(buf, max, RADIO_MODE_DMR);
#if D578UV_TAP_INVERT
	for (uint16_t i = 0; i < n; i++) {
		buf[i] = (int16_t)(buf[i] == -32768 ? 32767 : -buf[i]);
	}
#endif
	return n;
}

// --- The transmit tap: the soft modem's samples as PWM on PB9 into chip 0's audio input (tone_d578uv.c), the radio
// keyed for it by tx() (radio_d578uv.c: air_line_in). The receive tap is stopped while it runs (the air tap is half
// duplex), so its ring holds the duties and the transmitter (ring_mem) ---
#define TX_HZ			24000u
// The path with the equalizer and the low-end filter below: flat from about 20 Hz to 2.8 kHz
#define TX_LO_HZ		20u
#define TX_HI_HZ		2800u
// Counts per kHz of deviation at 1 kHz (256 counts a PWM step): with the drive air_line_in sets, about 16 PWM steps a
// kHz (measured off air on UHF with the Bluetooth module powered; VHF not measured). With the module off for the
// transmission the path's gain is higher and the samples are scaled down (radio_d578uv.c, d578uv_air_tx_gain). The
// low-end filter's peaks reach about four times the outer level, so the outer level sits near a quarter of PB9's range
#define TX_COUNTS_PER_KHZ	4150u
// The path's equalizer, at 24 kHz, Q14, 1 at 1 kHz: the inverse of the modulator from PB9 above its low end, as
// measured off air on UHF (a demodulator of known scale on the bench received a known C4FM waveform played on PB9;
// least squares, 161 taps with 120 of look-ahead). The path inverts the band and a low-pass starts near 2.4 kHz
// (-6 dB at 2.8 kHz): the equalizer inverts the band and lifts its top
#define TX_EQ_TAPS		161u
#define TX_EQ_AT		120u
static const int16_t tx_eq[TX_EQ_TAPS] = {
	40, -95, -137, -62, 89, 210, 199, 30, -202, -337, -260, 6,
	298, 415, 261, -80, -391, -466, -251, 120, 417, 456, 224, -135,
	-406, -443, -239, 82, 344, 418, 276, -6, -284, -421, -344, -84,
	231, 438, 419, 171, -179, -443, -470, -240, 125, 423, 492, 294,
	-61, -382, -501, -358, -29, 314, 497, 425, 133, -233, -486, -491,
	-238, 147, 462, 537, 323, -71, -436, -574, -407, -19, 383, 584,
	476, 115, -310, -576, -543, -226, 213, 546, 598, 336, -110, -509,
	-648, -443, 8, 471, 699, 558, 114, -404, -718, -651, -230, 327,
	723, 742, 363, -220, -703, -825, -508, 100, 688, 940, 696, 46,
	-691, -1122, -972, -249, 732, 1471, 1513, 675, -852, -2574, -3899, -4392,
	-3940, -2769, -1327, -84, 646, 792, 510, 79, -242, -316, -173, 41,
	167, 132, -12, -136, -136, -11, 139, 187, 83, -105, -235, -203,
	-21, 183, 263, 160, -51, -221, -234, -92, 98, 204, 163, 21,
	-115, -158, -92, 27, 125,
};

// The path's low end: two high-pass corners, 72.6 and 124.6 Hz (measured with the equalizer above, as it): the
// low-end filter for them at 4800 symbols/s (NXDN 4800 takes it in pairs)
#define TX_LF_TAPS		97u
#define TX_LF_AT		48u
#define TX_LF_RATE		4800u
static const int16_t tx_lf[TX_LF_TAPS] = {
	-154, -193, -235, -279, -326, -375, -427, -482, -539, -599, -662, -728,
	-797, -868, -942, -1020, -1100, -1183, -1269, -1358, -1450, -1545, -1644, -1745,
	-1850, -1957, -2068, -2182, -2299, -2420, -2544, -2671, -2801, -2934, -3071, -3211,
	-3354, -3501, -3651, -3804, -3961, -4120, -4284, -4450, -4619, -4792, -4968, -5148,
	13173, -1052, -985, -922, -861, -802, -746, -693, -641, -592, -546, -501,
	-459, -419, -381, -345, -311, -279, -248, -220, -193, -168, -145, -124,
	-104, -85, -68, -52, -38, -25, -14, -4, 6, 14, 21, 26,
	31, 35, 38, 40, 42, 42, 42, 41, 40, 38, 35, 32,
	29,
};

static bool tx_ready;		// the ring is set up (the first write after a stop)

static bool tx_prepare(void)
{
	uint16_t len;
	uint8_t *mem;

	if (tx_ready) {
		return true;
	}
	// Only once the core's tx() keyed the transmitter (rule 3, hal_air.h): it decides the samples' gain
	if (!d578uv_air_keyed()) {
		return false;
	}
	mem = d578uv_rxtap_mem(&len);
	if (mem == NULL) {
		return false;	// the receive tap still runs
	}
	d578uv_air_tx_ring(mem, len, d578uv_air_tx_gain());
	tx_ready = true;
	return true;
}

static uint16_t tx_write(const int16_t *buf, uint16_t n)
{
	return tx_prepare() ? d578uv_air_tx_put(buf, n) : 0;
}

static bool tx_start(void)
{
	// Rule 3 (hal_air.h): only with the transmitter keyed for this path by the core's tx()
	return d578uv_air_keyed() && tx_prepare() && d578uv_air_tx_start(TX_HZ);
}

static void tx_stop(void)
{
	d578uv_air_tx_stop();
	tx_ready = false;
}

const hal_baseband_t d578uv_baseband = {
	.rx_rate_hz = TAP_HZ, .rx_channels = 1, .rx_lo_hz = TAP_LO_HZ, .rx_hi_hz = TAP_HI_HZ, .rx_counts_per_khz = 0,
	.rx_slope_hz = TAP_SLOPE_HZ, .rx_start = tap_start, .rx_read = tap_read, .rx_stop = d578uv_rxtap_stop,
	.tx_rate_hz = TX_HZ, .tx_lo_hz = TX_LO_HZ, .tx_hi_hz = TX_HI_HZ, .tx_counts_per_khz = TX_COUNTS_PER_KHZ,
	.tx_eq = tx_eq, .tx_eq_n = TX_EQ_TAPS, .tx_eq_at = TX_EQ_AT,
	.tx_lf = tx_lf, .tx_lf_n = TX_LF_TAPS, .tx_lf_at = TX_LF_AT, .tx_lf_rate = TX_LF_RATE,
	.tx_start = tx_start, .tx_write = tx_write, .tx_stop = tx_stop, .now_us = hal_micros,
	.rx_time_us = d578uv_rxtap_time_us,
};

// The soft modem's receiver, used only while a session is open (its transmitter is in ring_mem)
static air_softmodem_t softmodem;

static const air_tap_cfg_t tap_cfg = {
	.bb = &d578uv_baseband, .sm = &softmodem, .tx = &ring_mem.tx.sm, .fit_flags = AIR_FIT_UNTESTED,
	.lock = d578uv_modem_lock, .unlock = d578uv_modem_unlock,
};

void d578uv_air_init(void)
{
	air_tap_setup(&tap_cfg);
}
#endif	// CONFIG_AIR

#endif	// CONFIG_APRS_RX || CONFIG_AIR
