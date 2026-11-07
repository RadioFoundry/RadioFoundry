// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 radio: the FD6818 on its two-wire bus (hardware-rt10.md 7) with the RT10's register set, FM receive and
// transmit. The FD6818 modulates the microphone itself; the PA is keyed through the supply pins and DAC B with the
// factory calibration's level (calibration_rt10.c). TX is refused without a calibration, outside the band, or with a
// tone the chip can't send.
//
// DMR (CONFIG_DMR, hardware-rt10.md 7.1): the FD6818's digital set, the modem on the shared glue (dmr_rt10.c). tx()
// only arms the transmitter; each burst then runs the FM key-up and key-down from the RF task (rt10_burst_on /
// rt10_burst_off), as the stock firmware does
//
// APRS (CONFIG_APRS, hardware-rt10.md 7.2): the tones take DMR's path onto the reference, the modem's FM transmitter
// playing them on MOD1 with the FD6818's microphone off; receive takes the FD6818's FM audio through the modem
#include "rt10.h"
#include "dmr_rt10.h"
#include "config.h"
#include "hal_dtmf.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "radio/apc_ramp.h"
#include "radio/ref_trim.h"
#include "modem/hrc7000/hrc7000.h"
#include "rf/rfbus/rfbus_i2c.h"
#include "rf/fd6818/fd6818.h"
#include "rf/fd6818/fd6818sh.h"
#if CONFIG_APRS
#include "hrc_afsk.h"
#include "audio_pcm.h"
#include "fm_rx_audio.h"
#endif

#define FD_ADDR7	0x71u	// 0xE2 / 0xE3 on the wire
#define FD_REG_ID	0x00u
#define FD_REG_NOISE	0x65u
#define FD_REG_RSSI	0x67u
#define FD_VOLUME	14u	// fd6818sh_volume_word step: the radio's volume is the analog knob after the chip
#define TX_TONE_GAIN	48u	// FM TX tone generator level (0..127), the stock single tone's
#define RX_QUIET_MS	150u	// after an FM key-down the chip reads as a carrier for a moment: the squelch stays shut
#define SPK_SWITCH_MS	10u	// the AF gate before the amplifier on, after it off (the original firmware's)
#define APC_STEPS	180u	// the stock ramp's length: 180 steps of 2 us (hardware-rt10.md 5)
#define APC_STEP_US	2u
#define TX_END_US	100u	// stock key-down: between the PA pins, the receiver's retune and the RX supply
#define CTCSS_MIN_X10	600u
#define CTCSS_MAX_X10	2600u
#define DAC_C_INIT	1762u	// the stock DAC set-up's channel C level, as on the other HR_C7000 radios
#define MODEM_MIC_DB	0u	// the modem's ADC gain: the microphone reaches the FD6818, not the modem

// The RT10's power-on set (hardware-rt10.md 7). Page 1 is addressed from 0x80 on this bus
static const rfbus_regval_t rt10_page0[] = {
	{ 0x17, 0x06E4 }, { 0x19, 0x930F }, { 0x1A, 0x1F80 }, { 0x1F, 0x8258 }, { 0x22, 0x3C14 }, { 0x20, 0x5204 },
	{ 0x23, 0x9C0F }, { 0x2A, 0x2202 }, { 0x35, 0xF008 }, { 0x37, 0x0601 }, { 0x3A, 0x8000 }, { 0x40, 0x3AD0 },
	{ 0x43, 0x4048 }, { 0x44, 0x42CC }, { 0x45, 0x6762 }, { 0x49, 0x282C }, { 0x4A, 0x5446 }, { 0x4B, 0x223C },
	{ 0x4F, 0x2D2B }, { 0x53, 0x77DD }, { 0x58, 0xE217 }, { 0x73, 0x4642 }, { 0x74, 0x110F }, { 0x77, 0x80EF },
	{ 0x78, 0x504C }, { 0x7D, 0x4000 }, { 0x7E, 0x3425 },
};
static const rfbus_regval_t rt10_page1[] = {
	{ 0x10, 0x98A2 }, { 0x16, 0x81E0 }, { 0x17, 0x02BE }, { 0x18, 0x8FD8 }, { 0x19, 0x4840 }, { 0x1B, 0x2000 },
	{ 0x1C, 0x076F }, { 0x1D, 0xB9FD }, { 0x1E, 0x6392 }, { 0x1F, 0xC1D1 }, { 0x20, 0x220A }, { 0x21, 0xE7C0 },
	{ 0x22, 0x0D16 }, { 0x23, 0xD1E7 }, { 0x24, 0x0A0D }, { 0x27, 0x18DA }, { 0x28, 0xB9B3 }, { 0x29, 0x2113 },
	{ 0x2A, 0x84E2 }, { 0x2B, 0x3B6B }, { 0x36, 0x8106 },
};
static const rfbus_regval_t rt10_agc[] = {
	{ 0x10, 0x0038 }, { 0x11, 0x025A }, { 0x12, 0x037B }, { 0x13, 0x03DE }, { 0x7B, 0xB89E }, { 0x7C, 0xE6D0 },
	{ 0x7D, 0x8000 },
};
static const rfbus_regval_t rt10_after[] = {
	{ 0x3E, 0x7410 }, { 0x25, 0x0000 }, { 0x27, 0x0000 }, { 0x26, 0x0000 }, { 0x53, 0x765D },
};
// The stock receive set-up, written by the analog mode: the RT10's AFC, DC, filter and page-1 words (the microphone
// gain among them) in place of the driver's. It undoes the digital set; 0x48 goes back to its power-on word, the one
// FM was measured with (the stock writes it per bandwidth instead, hardware-rt10.md 7)
static const rfbus_regval_t rt10_analog[] = {
	{ 0x73, 0x4642 }, { 0x04, 0x0300 }, { 0x7E, 0x3425 }, { 0x44, 0x42CC }, { 0x4B, 0x223C }, { 0x48, 0x6C4C },
	{ 0x35, 0xF008 }, { 0x77, 0x80EF },
	{ 0x23, 0x9C0F }, { FD6818SH_REG_PAGE, 1 }, { 0x17, 0x02BE }, { 0x18, 0x8FD8 }, { 0x19, 0x4840 },
	{ 0x1C, 0x076F }, { 0x1D, 0xB9FD }, { 0x27, 0x18DA }, { FD6818SH_REG_PAGE, 0 },
};
// 902-928 MHz is one band, the chip's above 600 MHz: 0x1A and 0x40 as the stock's band select writes them. A band
// word from the chip's lower bands leaves the receiver deaf (hardware-rt10.md 7, gotchas.md 3.15)
static const uint16_t rt10_band_edge[] = { 1120 };
static const uint16_t rt10_band_word[] = { 0x1F80 };
static const uint8_t rt10_dev_high[] = { 0x3A };

// The digital set (hardware-rt10.md 7.1), the stock's DMR entry, then its digital bandwidth word and the band select's
// 0x40, which the stock's every tune writes. 0x4B = 0xE442 is what puts the demodulated 4FSK on the AF output: without
// it the modem's AF ADC sees a few millivolts and never syncs. Receive audio 0x4140 goes to the modem; at a burst's TX
// 0x6740. 0x77 is the stock's DMR word (0x80EF in FM, the analog set)
static const rfbus_regval_t rt10_digital[] = {
	{ 0x04, 0x0300 }, { 0x48, 0x6C5C }, { 0x4B, 0xE442 }, { 0x73, 0x569A }, { 0x7E, 0x349E }, { 0x23, 0x8F87 },
	{ 0x43, 0x7E08 }, { 0x40, 0x3AD0 }, { 0x77, 0xC0FF },
};

#define N(t)	(uint8_t)(sizeof(t) / sizeof(t[0]))
static const fd6818sh_cfg_t rt10_fd = {
	.page0 = rt10_page0, .page0_n = N(rt10_page0), .page1 = rt10_page1, .page1_n = N(rt10_page1),
	.agc = rt10_agc, .agc_n = N(rt10_agc), .after = rt10_after, .after_n = N(rt10_after),
	.park_hz = 915000000u,
	.band_edge = rt10_band_edge, .band_word = rt10_band_word, .dev_high = rt10_dev_high, .bands = 1,
	.tx_state = 0xC1FE, .tx_tone_state = 0xC1FA,
	.page1_or = 0x80,
	.analog = rt10_analog, .analog_n = N(rt10_analog),
	.bw_word = { 0x4048, 0x3028 },
	.tx_dev = 0x3AD0,	// the band select's word, kept in TX with or without sub-audio
	.pa_both = true,
	.digital = rt10_digital, .digital_n = N(rt10_digital),
	.af_dmr = 0x4140, .af_dmr_tx = 0x6740,
};

static void fd_scl(bool high) { hrc_pin_write(PIN_FD_SCL, high); }
// Open drain: released to the pull-up for a 1
static void fd_sda(bool high)
{
	if (high) {
		hrc_pin_in(PIN_FD_SDA);
	} else {
		hrc_pin_out(PIN_FD_SDA, false);
	}
}
static bool fd_sda_in(void) { return hrc_pin_read(PIN_FD_SDA); }

static const i2c_gpio_t fd_i2c = {
	.scl = fd_scl, .sda = fd_sda, .sda_in = fd_sda_in, .delay_us = hal_delay_us, .half_us = 2,
};
static rfbus_i2c_t fd_bus;
static fd6818sh_t fd;
static uint16_t fd_id;
static bool fd_ok;		// the chip answered with its ID: the radio is driven
static bool audio_open;
static bool transmitting;
static uint32_t tx_end_ms;	// when the last FM transmission ended (tx_stop): the squelch reads closed for a while
static bool have_rx;
static bool asleep;		// rx_sleep(true): the receiver is off until rx_sleep(false) or the next rx()
static volatile int16_t trim_ppb;	// hal_radio_t.ref_trim: taken at the next tune
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static rt10_tx_plan_t plan;
static hrc7000_t modem;
static volatile bool pcm_on;	// the modem plays CPU samples (a beep, DMR voice): rt10_speaker_pcm()
static radio_mode_t mode = RADIO_MODE_FM;	// DMR only with CONFIG_DMR
static volatile bool dmr_armed;	// DMR TX armed by tx(): the bursts key the RF
static bool tx_wide;		// the FM transmission is at 25 kHz (the modem's deviation for APRS)
#if CONFIG_DMR
static uint32_t tx_hz;		// the armed transmission's frequency
static void dmr_disarm(void);
#endif
#if CONFIG_APRS
static bool afsk_tx;		// APRS tones on the reference (afsk_tx_route)
static bool aprs_listen;	// APRS takes the FD6818's FM audio through the modem (aprs_listen_route)
static bool afsk_tx_route(bool on);
static void aprs_listen_route(bool on);
static void modem_fm(void);

// PCM amplitude per AFSK level step (level 50 is the nominal 3 kHz peak): the reference's modulation slope, set on the
// SDR bench (hardware-rt10.md 7.2)
static const hrc_afsk_board_t afsk_board = {
	.tx_route = afsk_tx_route, .listen = aprs_listen_route, .amp_per_level = 55,
};
#endif

rfbus_t *rt10_rf_bus(void)
{
	return &fd_bus.bus;
}

// The shared PTT line's gate trusts no new press while this is set (drivers/input/sharedptt). DMR TX keys the RF per
// burst while armed, without setting transmitting
bool rt10_radio_transmitting(void)
{
	return transmitting || dmr_armed;
}

void rt10_rf_info(uint16_t *id, uint32_t *nacks)
{
	*id = fd_id;
	*nacks = fd_bus.nacks;
}

static bool cal_read(uint32_t addr, void *buf, uint32_t len)
{
	return hrc_flash_spi0.read(addr, buf, len);
}

// The modem's DC bias: the TX center byte and the step's modulation byte in RF_MOD_BIAS_CTRL, as on the other HR_C7000
// radios. Its MOD1 level is the TCXO's control voltage, so it sets the frequency of the FD6818 (hardware-rt10.md 4.1);
// unwritten, the radio sat 28 ppm low. Written at start-up and at every receive set-up, for the step's byte
static void write_bias(uint32_t hz)
{
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias(rt10_cal_tx_center(), rt10_cal_mod(hz)));
}

static void init(void)
{
	board_rf_safe();
	rt10_cal_load(cal_read);
	// DAC B (the PA control) and C to their pins, as the stock DAC set-up does; without it the PA gets no drive
	hrc_modem_analog_init(DAC_C_INIT);
	uint32_t irq = hrc_irq_save();
	IO_DIPLEX2 &= ~IO_DIPLEX2_DAC_C;
	hrc_irq_restore(irq);
	hrc_dac_b_power(false);	// powered only for a transmission
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	hrc7000_power_on_init(&modem, rt10_cal_tx_center(), MODEM_MIC_DB);
	write_bias(RT10_BIAS_DEFAULT_HZ);
#if CONFIG_DMR
	rt10_dmr_init(&modem);
#endif
	hrc_pin_in(PIN_RF_CSS);
	hrc_pin_in(PIN_RF_SQ);
	hrc_pin_out(PIN_FD_SCL, true);
	rfbus_i2c_attach(&fd_bus, &fd_i2c, FD_ADDR7, hal_delay_ms);
	fd_bus.crit_enter = hrc_irq_save;
	fd_bus.crit_exit = hrc_irq_restore;
	fd_id = rfbus_read(&fd_bus.bus, FD_REG_ID);
	fd_ok = fd_id == FD6818SH_ID;
	if (!fd_ok) {
		return;
	}
	fd6818sh_attach(&fd, &fd_bus.bus, FD6818SH_MAIN);
	fd6818sh_set_cfg(&fd, &rt10_fd);
	fd6818sh_init_main(&fd);
	fd6818sh_set_af_gain(&fd, FD_VOLUME);
	fd6818sh_analog_mode(&fd);
#if CONFIG_APRS
	hrc_fm_rx_attach(&modem);
	hrc_afsk_attach(&modem, &afsk_board);
#endif
}

// The FD6818's AF output: the speaker's audio with the squelch open; in DMR the modem's input, and while APRS listens
// the modem's copy for the demodulator, both whatever the squelch does
static bool af_on(void)
{
#if CONFIG_APRS
	if (aprs_listen) {
		return true;
	}
#endif
	return audio_open || mode == RADIO_MODE_DMR;
}

// The speaker (hardware-rt10.md 3): the amplifier (PTB8) is on for an open squelch (audio()) or while the modem plays
// samples (a beep, DMR voice), off while keyed. The FD6818's AF reaches it only through PTB13, high for an open squelch
// alone, so a beep or a call doesn't come with the receiver's noise. As the original firmware does it, the AF passes
// 10 ms before the amplifier turns on and stops 10 ms after it turns off, when the caller may wait (the radio task:
// audio()); the modem's PCM hook switches at once
static bool amp_out, af_out;

static void amp_update(bool may_wait)
{
	bool af = audio_open && !transmitting;
	bool amp = (audio_open || pcm_on) && !transmitting;
	if (!amp && amp_out) {
		hrc_pin_write(PIN_AMP_EN, false);
		amp_out = false;
		if (may_wait && af_out && !af) {
			hal_delay_ms(SPK_SWITCH_MS);
		}
	}
	if (af != af_out) {
		hrc_pin_write(PIN_SPK_FD, af);
		af_out = af;
		if (may_wait && af && amp && !amp_out) {
			hal_delay_ms(SPK_SWITCH_MS);
		}
	}
	if (amp && !amp_out) {
		hrc_pin_write(PIN_AMP_EN, true);
		amp_out = true;
	}
}

#if CONFIG_DMR
void rt10_speaker_pcm(bool on)
{
	if (on != pcm_on) {
		pcm_on = on;
		amp_update(false);
	}
}
#endif

static void set_mode(radio_mode_t m)
{
#if CONFIG_DMR
	if (!fd_ok || m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR) {
		fd6818sh_dmr_mode(&fd);
		DAC_DATA_C = RT10_DMR_DAC_C;
		rt10_dmr_enter(hrc7000_mod_bias(rt10_cal_tx_center(), rt10_cal_mod(have_rx ? rx_hz : RT10_BIAS_DEFAULT_HZ)));
		mode = RADIO_MODE_DMR;
	} else {
		dmr_disarm();
		rt10_dmr_leave();
		DAC_DATA_C = DAC_C_INIT;
		fd6818sh_analog_mode(&fd);
		mode = RADIO_MODE_FM;
#if CONFIG_APRS
		modem_fm();	// leaving DMR left the modem idle
#endif
	}
	have_rx = false;	// the next rx() retunes
#else
	(void)m;	// FM only without CONFIG_DMR
#endif
}

static fd6818sh_tone_t fd_tone(const subtone_t *t)
{
	if (t == NULL) {
		return FD6818SH_TONE_NONE;
	}
	switch (t->type) {
	case TONE_CTCSS:
		return FD6818SH_TONE_CTCSS;
	case TONE_DCS:
		return FD6818SH_TONE_DCS;
	case TONE_DCS_INV:
		return FD6818SH_TONE_DCS_INV;
	default:
		return FD6818SH_TONE_NONE;
	}
}

static bool tx_stop(void);

// The frequency the synthesizer is set to: the reference trim moves receive and transmit together
static uint32_t synth_hz(uint32_t hz)
{
	return ref_trim_tune_hz(hz, trim_ppb);
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone_in)
{
	if (!board_in_band(hz, false)) {
		return false;
	}
#if CONFIG_DMR
	dmr_disarm();	// a retune ends DMR TX (the hotspot's rx() disarms too)
#endif
	bool from_tx = tx_stop();
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = rx_tone_in != NULL ? *rx_tone_in : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	asleep = false;
	if (!fd_ok) {
		return true;
	}
	write_bias(hz);
	fd6818sh_set_state(&fd, FD6818SH_STATE_IDLE);
	fd6818sh_tune(&fd, synth_hz(hz));
	if (mode != RADIO_MODE_DMR) {	// DMR: the bandwidth is the digital set's, no sub-audio
		fd6818sh_set_bandwidth(&fd, bw == RADIO_BW_25K);
		fd6818sh_set_rx_tone(&fd, fd_tone(&rx_tone), rx_tone.value);
	}
	fd6818sh_set_af(&fd, af_on() ? FD6818SH_AF_RX : FD6818SH_AF_MUTE);
	fd6818sh_set_state(&fd, FD6818SH_STATE_RX);
	if (from_tx) {
		hal_delay_us(TX_END_US);	// stock key-down: the retune, then the RX supply
	}
	hrc_pin_write(PIN_RX_SUPPLY, true);
	return true;
}

static void apc_ramp(uint16_t target, bool up)
{
	for (uint16_t i = 0; i <= APC_STEPS; i++) {
		DAC_DATA_B = apc_ramp_level(target, up ? i : (uint16_t)(APC_STEPS - i), APC_STEPS);
		hal_delay_us(APC_STEP_US);
	}
}

static void audio_set(bool on, bool may_wait);

#if CONFIG_DMR
// DMR TX, armed at the key-up (hardware-rt10.md 7.1). The RF stays receiving until the first burst's RF TX interrupt;
// the modem's bias gets the step's modulation byte for the TX frequency, the FD6818 the PA gain
static bool dmr_tx(uint32_t hz, uint8_t power)
{
	rt10_tx_plan_t p;
	if (!fd_ok || !board_in_band(hz, true) || !rt10_cal_tx_plan(hz, power >= 1, &p)) {
		return false;
	}
	dmr_disarm();
	plan = p;
	tx_hz = hz;
	write_bias(hz);
	fd6818sh_dmr_tx_prepare(&fd, p.pa_gain);
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	dmr_armed = true;
	hrc_rfburst_arm();
	return true;
}

static void dmr_disarm(void)
{
	if (dmr_armed) {
		hrc_rfburst_disarm();
		dmr_armed = false;
		DAC_DATA_B = 0;
		hrc_dac_b_power(false);
		write_bias(rx_hz);
	}
}

// One burst's TX on, the stock's (hardware-rt10.md 7.1): the key-up delay, the RX supply off, the FD6818 to the TX
// frequency (its TX AF word first) in its TX state, PA select and TX supply on, DAC B ramped up
void rt10_burst_on(hrc_rfburst_wait_fn wait)
{
	wait(plan.on_us);
	hrc_pin_write(PIN_RX_SUPPLY, false);
	fd6818sh_burst_tune(&fd, synth_hz(tx_hz), true);
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_PA_SELECT, true);
	hrc_pin_write(PIN_TX_SUPPLY, true);
	apc_ramp(plan.dac, true);
}

// The burst's TX off: the hold until its last symbol (none when now), the ramp down, TX supply and PA select off,
// 100 us, the FD6818 back to the receive frequency, 100 us, the RX supply on. The stock's key-down delay counts from
// its RF RX interrupt; the hold to the burst's end takes its place. Returns how late the ramp down started, in us
uint32_t rt10_burst_off(hrc_rfburst_wait_fn wait, uint32_t elapsed_us, bool now)
{
	uint32_t late = now ? 0 : hrc_rfburst_late_us(elapsed_us, RT10_DMR_RF_TX_LEAD);
	if (!now) {
		wait(hrc_rfburst_hold_us(elapsed_us, RT10_DMR_RF_TX_LEAD));
	}
	apc_ramp(plan.dac, false);
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_TX_SUPPLY, false);
	hrc_pin_write(PIN_PA_SELECT, false);
	wait(TX_END_US);
	fd6818sh_burst_tune(&fd, synth_hz(rx_hz), false);
	wait(TX_END_US);
	hrc_pin_write(PIN_RX_SUPPLY, true);
	return late;
}
#endif

// The stock key-up (hardware-rt10.md 5): the TX words while still receiving, speaker off, PA select and TX supply
// low; then the calibration's delay, the RX supply off, the chip to f in its TX state, PA select and TX supply on, and
// DAC B ramped up to the calibration's level
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	if (mode == RADIO_MODE_DMR) {
#if CONFIG_DMR
		return dmr_tx(hz, power);
#else
		return false;
#endif
	}
	subtone_t tone = tx_tone != NULL ? *tx_tone : (subtone_t){ TONE_NONE, 0 };
	if (tone.type == TONE_CTCSS && (tone.value < CTCSS_MIN_X10 || tone.value > CTCSS_MAX_X10)) {
		return false;
	}
	if ((tone.type == TONE_DCS || tone.type == TONE_DCS_INV) && tone.value > 0777u) {
		return false;
	}
	rt10_tx_plan_t p;
	// Power index 1 ("H") is the stock's high set, "L" its low one
	if (!fd_ok || !board_in_band(hz, true) || !rt10_cal_tx_plan(hz, power >= 1, &p)) {
		return false;
	}
	tx_stop();
	asleep = false;
	plan = p;
	tx_wide = bw == RADIO_BW_25K;	// the FD6818's deviation goes by band; the modem's (APRS) by bandwidth
	fd6818sh_tx_prepare(&fd, fd_tone(&tone), tone.value, hz, p.pa_gain);
	fd6818sh_set_af(&fd, FD6818SH_AF_MUTE);
	hrc_pin_write(PIN_AMP_EN, false);
	hrc_pin_write(PIN_SPK_FD, false);
	amp_out = false;
	af_out = false;
	hrc_pin_write(PIN_PA_SELECT, false);
	hrc_pin_write(PIN_TX_SUPPLY, false);

	hal_delay_us(p.on_us);
	hrc_pin_write(PIN_RX_SUPPLY, false);
	fd6818sh_tx_tune(&fd, synth_hz(hz));
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	hrc_pin_write(PIN_PA_SELECT, true);
	hrc_pin_write(PIN_TX_SUPPLY, true);
	transmitting = true;
	apc_ramp(p.dac, true);
	return true;
}

// The stock key-down without the retune: the chip back to RX (still on the TX frequency), the calibration's delay,
// the ramp down, TX supply and PA select off. True when there was a transmission; rx() then retunes and turns the RX
// supply on
static bool tx_stop(void)
{
	if (!transmitting) {
		return false;
	}
#if CONFIG_APRS
	if (afsk_tx) {
		hrc_afsk.tx_stop();	// the core stops the tones first; this is the safety net
	}
#endif
	fd6818sh_tx_end(&fd);
	hal_delay_us(plan.off_us);
	apc_ramp(plan.dac, false);
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_TX_SUPPLY, false);
	hrc_pin_write(PIN_PA_SELECT, false);
	hrc_dac_b_power(false);
	hal_delay_us(TX_END_US);
	transmitting = false;
	tx_end_ms = hal_millis();
	audio_set(audio_open, false);
	return true;
}

static void tx_off(void)
{
#if CONFIG_DMR
	dmr_disarm();	// the RF task finishes the burst it is in; rx() then sets the receiver up again
#endif
	if (have_rx) {
		rx(rx_hz, rx_bw, &rx_tone);	// stops the transmission first
	} else if (tx_stop()) {
		hrc_pin_write(PIN_RX_SUPPLY, true);
	}
}

// FM TX tone (1750 Hz burst, roger beep, CW ID): the chip's tone generator instead of the microphone
static bool tx_tone(uint16_t hz)
{
	if (!transmitting) {
		return false;
	}
	fd6818sh_tone_start(&fd, hz, TX_TONE_GAIN);
	return true;
}

#if CONFIG_DTMF
static bool tx_dtmf(char digit)
{
	uint16_t low = 0, high = 0;
	if (!transmitting || (digit != 0 && !dtmf_pair(digit, &low, &high))) {
		return false;
	}
	fd6818sh_dtmf_start(&fd, low, high);
	return true;
}
#endif

// The sub-audio turn-off tail; the core holds the carrier for it
static bool tx_tail(bool on)
{
	return transmitting && fd6818sh_tx_tail(&fd, on);
}

static bool tx_tone_key(bool down)
{
	if (!transmitting) {
		return false;
	}
	fd6818sh_tone_key(&fd, down);
	return true;
}

// Battery save: the chip idle and the receiver's supply off. Waking retunes with the last rx() settings
static void rx_sleep(bool sleep)
{
	if (!fd_ok || transmitting || dmr_armed || !have_rx || sleep == asleep) {
		return;
	}
	if (sleep) {
		fd6818sh_set_state(&fd, FD6818SH_STATE_IDLE);
		hrc_pin_write(PIN_RX_SUPPLY, false);
		asleep = true;
	} else {
		rx(rx_hz, rx_bw, &rx_tone);
	}
}

#if CONFIG_REF_TRIM
// Taken at the next tune, so the chip's bus stays the radio task's: true asks the core for one
static bool ref_trim(int16_t ppb)
{
	trim_ppb = ppb;
	return true;
}
#endif

static void status(radio_status_t *st)
{
	if (!fd_ok || asleep) {
		st->rssi_dbm = -160;
		st->noise = 0;
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
	// The same scale as the BK4819's 0x67: half dB from -160 dBm, at the chip's input
	uint16_t r67 = rfbus_read(&fd_bus.bus, FD_REG_RSSI);
	st->rssi_dbm = (int16_t)((int16_t)((r67 & 0x1FFu) >> 1) - 160);
	// The chip's floor (38-44 raw at 902-928 MHz) lifted above the core's squelch levels, as on the other FD6818
	// radios (fd6818.h): read raw, every level opened on noise
	st->noise = fd6818_noise(rfbus_read(&fd_bus.bus, FD_REG_NOISE));
	if (hal_millis() - tx_end_ms < RX_QUIET_MS) {
		st->noise = 0xFF;	// back in receive the chip reads as a carrier for a moment: no burst of noise at the unkey
	}
	st->carrier = false;
	// The chip compares the sub-audio set by set_rx_tone and puts the result on its GPIO0 (fd6818sh.h). A match stays
	// up after the carrier and through the next one, whatever its tone, until the sub-audio set is written again
	// (hardware-rt10.md 7): with the chip's squelch (GPIO6) closed and the match up, write it again, which clears it
	bool sq = hrc_pin_read(PIN_RF_SQ), css = hrc_pin_read(PIN_RF_CSS);
	if (mode != RADIO_MODE_DMR && rx_tone.type != TONE_NONE && !sq && css && !transmitting) {	// 0x51 holds the TX sub-audio while keyed
		fd6818sh_set_rx_tone(&fd, fd_tone(&rx_tone), rx_tone.value);
	}
	st->tone_ok = rx_tone.type == TONE_NONE || mode == RADIO_MODE_DMR || (sq && css);
}

// may_wait: the radio task's call (hal_radio_t.audio) may take the speaker's 10 ms steps; tx_stop(), on the burst
// path too, doesn't
static void audio_set(bool on, bool may_wait)
{
	audio_open = on;
	if (transmitting) {
		return;		// muted until tx_stop() restores this
	}
	// While DMR TX is armed the RF task owns 0x47 (its TX word in a burst, the digital receive word after it); DMR's
	// AF is never muted, so there is nothing to write here, and a write could land inside a burst
	if (fd_ok && !dmr_armed) {
		fd6818sh_set_af(&fd, af_on() ? FD6818SH_AF_RX : FD6818SH_AF_MUTE);
	}
	amp_update(may_wait);
}

static void audio(bool on)
{
	audio_set(on, true);
}

#if CONFIG_APRS
// The modem in FM: receiving while APRS listens (its FM playback off), else idle. DMR keeps the modem's own state
static void modem_fm(void)
{
	if (mode != RADIO_MODE_FM) {
		return;
	}
	if (aprs_listen) {
		hrc7000_fm_rx(&modem, rx_bw == RADIO_BW_25K);
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);
	} else {
		hrc7000_fm_idle(&modem);
	}
}

// APRS AFSK (hrc_afsk.c). The FD6818 modulates only its microphone, so the tones go the way DMR's 4FSK does: the
// modem's FM transmitter plays the prompt FIFO on MOD1, the reference's control voltage (hardware-rt10.md 7.1), around
// the receive set-up's bias. The FD6818 keeps the carrier in its tone state with no tone (microphone off), and the
// modem's own microphone input is muted. False when the radio isn't transmitting FM
static bool afsk_tx_route(bool on)
{
	if (on && (!transmitting || mode != RADIO_MODE_FM)) {
		return false;
	}
	if (on == afsk_tx) {
		return true;
	}
	afsk_tx = on;
	if (on) {
		fd6818sh_mic_off(&fd, true);
		hrc7000_codec_mute(&modem, true, false);
		hrc7000_fm_tx(&modem, tx_wide);
		hrc_audio_pcm_ring(&modem, true);
	} else {
		hrc_audio_pcm_ring(&modem, pcm_on);	// back to what the speaker PCM last asked for
		modem_fm();
		hrc7000_codec_mute(&modem, false, false);
		fd6818sh_mic_off(&fd, false);
	}
	return true;
}

// APRS receive: the modem in FM receive copies its input, the FD6818's demodulated audio, to the CPU (fm_rx_audio.c).
// The FD6818's AF output stays on with the squelch shut (the amplifier gate keeps the speaker quiet), and the modem's
// own FM playback stays off: the speaker hears the FD6818 directly
static void aprs_listen_route(bool on)
{
	aprs_listen = on;
	if (!afsk_tx) {
		modem_fm();
	}
	if (fd_ok && !transmitting && !dmr_armed) {	// armed: the RF task owns 0x47 (audio_set)
		fd6818sh_set_af(&fd, af_on() ? FD6818SH_AF_RX : FD6818SH_AF_MUTE);
	}
}
#endif

// The debug link's CHIP_REG (board.chip_reg): chip 0 is the FD6818. Page 1 is selected around the access and its
// registers go out at 0x80 up, as rt10_fd's page1_or. Bench use: a radio task write between the page selects would land
// on page 1
bool rt10_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	if (chip != 0 || page > 1 || reg > 0x7Fu || !fd_ok) {
		return false;
	}
	uint32_t nacks = fd_bus.nacks;
	uint8_t at = page == 1 ? (uint8_t)(reg | rt10_fd.page1_or) : reg;
	if (page == 1) {
		rfbus_write(&fd_bus.bus, FD6818SH_REG_PAGE, 1);
	}
	if (write) {
		rfbus_write(&fd_bus.bus, at, (uint16_t)*val);
	}
	*val = rfbus_read(&fd_bus.bus, at);
	if (page == 1) {
		rfbus_write(&fd_bus.bus, FD6818SH_REG_PAGE, 0);
	}
	return fd_bus.nacks == nacks;
}

const hal_radio_t rt10_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.tx_tone = tx_tone,
	.tx_tail = tx_tail,
	.tx_tone_key = tx_tone_key,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
	.rx_sleep = rx_sleep,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
};
