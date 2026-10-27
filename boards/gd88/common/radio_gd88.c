// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 radio, either SoC: a main BK4819 and an LO chip on one bit-banged 3-wire bus, the superhet's RX paths and
// analog FM (docs/spec/hardware-gd88.md 11). The same scheme and chip set-up as hardware-db25d.md 4.
//
// Receive on f: the LO chip synthesizes f + IF (GD88_IF_HZ: 45 MHz on A, 51.55 MHz on B), the main chip receives and
// demodulates at the IF. The LO chip is a second BK4819 or an AT1141 (ID 0x1100), told apart by its ID as the original
// firmware does; the AT1141 runs low-side, f - IF, from 400 to 440 MHz. On A the LO chip's GPIO4 / GPIO5 also select
// the band's path (GD88_BAND_LO_GPIO). DAC A isn't used.
//
// Analog FM transmit (GD88_TX=1, A only): the main chip is tuned to f and modulates the microphone, the LO chip is
// off. The PA is B's (hardware-gd88.md 10): A sends B the drive from A's page and the page's delays (RX2_TX, before
// every transmission), then raises its TX line PTB2; B keys the PA on that edge and ramps its DAC B, and drops it on
// the falling edge. A never writes DAC B. TX is refused without a valid page, outside the bands, with a tone other
// than CTCSS, or when B doesn't take the drive. DMR transmit runs the same sequence once per burst (gd88_burst_on /
// gd88_burst_off, from the RF task in dmr_gd88.c).
#include "gd88_rf.h"
#include "config.h"
#include "radio/radio.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/bk4819/bk4819.h"
#include "rf/at1141/at1141.h"
#include "modem/hrc7000/hrc7000.h"

// TODO(gd88): unknown, needs a radio: DAC C's role on each SoC. These are hardware-db25d.md's levels
#define DAC_C_INIT		1762u			// at init, and 1780 once receive is set up
#define DAC_C_RX		1780u

#define IF_HZ			GD88_IF_HZ
#define LOW_SIDE_FROM_HZ	400000000u	// AT1141: f - IF from here to LOW_SIDE_TO_HZ, as the original firmware
#define LOW_SIDE_TO_HZ		440000000u
#define UHF_FROM_HZ		280000000u

// GD88_TX_DRIVE_PCT scales the page's PA drive (100 = the page's value)
#ifndef GD88_TX_DRIVE_PCT
#define GD88_TX_DRIVE_PCT	100u
#endif
#ifndef GD88_TX_TONE_GAIN
#define GD88_TX_TONE_GAIN	BK4819_TONE_GAIN_STOCK
#endif
// Speaker volume steps 1..RADIO_VOLUME_MAX -> the AF DAC gain nibble (register 0x48 bits 3:0), step 1 = 0 up to
// GD88_VOL_GAIN_TOP. Step 0 is mute
#ifndef GD88_VOL_GAIN_TOP
#define GD88_VOL_GAIN_TOP	15u
#endif
#define GD88_VOL_GAIN_DEFAULT	3u
#define TX_RX_OFF_US		50u		// RX paths off, then this long before the transmitter is tuned
#define TX_SETTLE_MS		3u		// the chip's VCO calibration, before the PA is selected
#define TX_SUPPLY_US		200u		// B: PA select, then this long before the TX supply
#define PA_RAMP_US		(GD88_APC_STEPS * GD88_APC_STEP_US)	// B's ramp up or down
#define PA_SLACK_US		200u		// B's task wake-up after the edge, with room
#define TX_END_PATH_US		300u		// PA select off, then this long before the receiver is retuned
#define TX_END_RX_US		200u		// after the RX path is switched on again
#define BIAS_DEFAULT_HZ		450000000u	// the modulation balance's frequency before one is known
#define CTCSS_MIN_X10		600u
#define CTCSS_MAX_X10		2600u

static bk4819_t rf_main, rf_lo;
static at1141_t lo_at;		// the LO when it answers as an AT1141: register access through rf_lo's bus
static bool lo_is_at1141;
static hrc7000_t modem;
static bool supported;		// the main chip answered as a BK4819, the LO as a BK4819 or an AT1141
static uint16_t main_id, lo_id;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static bool asleep;		// rx_sleep(true): the receiver is off until rx_sleep(false) or the next rx()
static bool transmitting;
static bool audio_open;		// what audio() was last asked, restored after a transmission
static bool vol_muted;		// volume step 0: the chip output stays off whatever audio() asks
static uint8_t vol_gain = GD88_VOL_GAIN_DEFAULT;
static bool dmr_armed;		// DMR TX armed by tx(): the bursts key the RF (dmr_gd88.c)
#if GD88_TX
static uint16_t tx_off_us;
#endif
#if CONFIG_DMR && GD88_TX
static uint16_t tx_on_us;
static uint32_t tx_hz;
static void dmr_disarm(void);
#endif
static radio_mode_t mode = RADIO_MODE_FM;
static uint16_t last_rssi_raw, last_noise_raw;

// ---- 3-wire bus: clock and data shared, one enable per chip ----

static void rf_clk(bool level) { hrc_pin_write(GD88_PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(GD88_PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(GD88_PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(GD88_PIN_RF_DATA); }
static void cs_main(bool level) { hrc_pin_write(GD88_PIN_RF_SEN_MAIN, level); }
static void cs_lo(bool level) { hrc_pin_write(GD88_PIN_RF_SEN_LO, level); }

static const bk4819_bus_t bus_main = {
	.cs = cs_main,
	.clk = rf_clk,
	.data_out = rf_data_out,
	.data_release = rf_data_release,
	.data_in = rf_data_in,
	.delay_us = hal_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

static const bk4819_bus_t bus_lo = {
	.cs = cs_lo,
	.clk = rf_clk,
	.data_out = rf_data_out,
	.data_release = rf_data_release,
	.data_in = rf_data_in,
	.delay_us = hal_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

// ---- LO chip: a BK4819 or an AT1141 on the LO enable ----

static void at_write(uint8_t reg, uint16_t val) { bk4819_write(&rf_lo, reg, val); }
static uint16_t at_read(uint8_t reg) { return bk4819_read(&rf_lo, reg); }
static const at1141_bus_t bus_at = { .write = at_write, .read = at_read, .delay_ms = hal_delay_ms };

static uint32_t lo_hz_for(uint32_t hz)
{
	if (lo_is_at1141 && hz >= LOW_SIDE_FROM_HZ && hz <= LOW_SIDE_TO_HZ) {
		return hz - IF_HZ;
	}
	return hz + IF_HZ;
}

static void lo_tune(uint32_t hz)
{
	if (lo_is_at1141) {
		at1141_tune(&lo_at, lo_hz_for(hz));
	} else {
		bk4819_lo_tune(&rf_lo, lo_hz_for(hz));
	}
}

#if GD88_TX || CONFIG_BATTERY_SAVE
static void lo_off(void)
{
	if (lo_is_at1141) {
		at1141_off(&lo_at);
	} else {
		bk4819_lo_off(&rf_lo);
	}
}
#endif

#if CONFIG_DMR && GD88_TX
static void lo_resume(void)
{
	if (lo_is_at1141) {
		at1141_restart(&lo_at);
	} else {
		bk4819_lo_resume(&rf_lo);
	}
}
#endif

#if GD88_BAND_LO_GPIO
static void lo_gpio(uint8_t pin, bool high)
{
	if (lo_is_at1141) {
		at1141_gpio(&lo_at, pin, high);
	} else {
		bk4819_gpio(&rf_lo, pin, high);
	}
}
#endif

// ---- RF paths ----

// The TX line low (A: B drops the PA on its edge). DAC B is never touched here: on B it is the PA drive, which B's PA
// code owns (a retune of B's receiver while A transmits mustn't cut it)
static void tx_paths_off(void)
{
	gd88_path_tx_off();
}

// The C7000 modem's DC bias at every receive set-up, in FM too: the page's TX center in SIG_CENTER and the two-point
// bias level with the modulation balance in RF_MOD_BIAS_CTRL. It is the voltage on the TCXO's control pin, so it sets
// the frequency of both BK4819s
static void ref_trim(uint32_t hz)
{
	uint8_t c = gd88_cal_tx_center();
	hrc7000_wr(&modem, C7_SIG_CENTER, hrc7000_sig_center_dc(c));
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias_dc(c, gd88_cal_mod(hz)));
}

static void init(void)
{
	hrc_pin_out(GD88_PIN_RF_SEN_MAIN, true);
	hrc_pin_out(GD88_PIN_RF_SEN_LO, true);
	hrc_pin_out(GD88_PIN_RF_CLK, true);
	hrc_pin_in(GD88_PIN_RF_DATA);
	gd88_path_init();
	// The modem's analog set-up with the bias DAC at its init value. DAC B: the PA drive on B (pa_gd88b.c); A doesn't
	// use it
	hrc_modem_analog_init(DAC_C_INIT);
	hrc_dac_b_power(false);

	// The modem block powered as the DMR glue does, so the reference trim reaches the TCXO in FM builds too
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	hrc7000_power_on_init(&modem, gd88_cal_tx_center(), GD88_MIC_GAIN_DB);
	ref_trim(BIAS_DEFAULT_HZ);

	bk4819_attach(&rf_main, &bus_main);
	bk4819_attach(&rf_lo, &bus_lo);
#if CONFIG_DMR
	// Before the chip check: the modem's PCM audio (beeps, DMR voice) needs it attached even on a radio whose RF chips
	// aren't driven here. A beep on a NULL modem faulted the radio task at every PTT press (LO chip 0x1100, 2026-10-26).
	// set_mode(), rx() and tx() still refuse to use DMR or RF on such a radio
	gd88_dmr_init(&rf_main, &modem);
#endif
	main_id = bk4819_read(&rf_main, BK4819_REG_ID);
	lo_id = bk4819_read(&rf_lo, BK4819_REG_ID);
	// The original firmware also knows an FD6818 main chip and a 0x7813 LO; only the main BK4819 with a BK4819 or an
	// AT1141 LO is driven here (a GD-88 A on the bench has the AT1141, 2026-10-26), anything else leaves the radio
	// receive-less
	lo_is_at1141 = lo_id == AT1141_ID;
	supported = main_id == BK4819_ID && (lo_id == BK4819_ID || lo_is_at1141);
	if (!supported) {
		return;
	}
	bk4819_init_main(&rf_main);
	bk4819_write(&rf_main, BK4819_REG_PA, BK4819_PA_MAX);	// 0xBF once, never changed
	if (lo_is_at1141) {
		at1141_attach(&lo_at, &bus_at);
		at1141_init(&lo_at);
	} else {
		bk4819_init_lo(&rf_lo, BK4819_LO_PA_MAX);
	}
	bk4819_analog_mode(&rf_main);
}

static void volume_apply(void);

#if GD88_BAND_LO_GPIO
#define LO_GPIO_UHF		4u
#define LO_GPIO_VHF		5u
// The band's path: VHF = GPIO5 high, GPIO4 low; UHF the reverse
static void lo_band(uint32_t hz)
{
	bool uhf = hz >= UHF_FROM_HZ;
	lo_gpio(LO_GPIO_UHF, uhf);
	lo_gpio(LO_GPIO_VHF, !uhf);
}
#endif

static volatile bool pcm_on;	// the modem plays CPU samples (a beep, DMR voice): gd88_speaker_pcm()

// The speaker: on for an open squelch (audio()) or while the modem plays samples, from the modem's line out in DMR
// and for samples, else from the main chip's AF
static void speaker(void)
{
	bool on = (audio_open || pcm_on) && !transmitting;
	uint32_t s = hrc_irq_save();	// also called from the audio task (gd88_speaker_pcm)
	gd88_path_speaker(on, mode == RADIO_MODE_DMR || pcm_on);
	hrc_irq_restore(s);
}

#if CONFIG_DMR
void gd88_speaker_pcm(bool on)
{
	if (on != pcm_on) {
		pcm_on = on;
		speaker();
	}
}
#endif

static void set_mode(radio_mode_t m)
{
#if CONFIG_DMR
	if (!supported || m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR) {
		gd88_dmr_enter(rx_hz);
		mode = RADIO_MODE_DMR;
	} else {
#if GD88_TX
		dmr_disarm();
#endif
		gd88_dmr_leave();
		mode = RADIO_MODE_FM;
		volume_apply();
	}
	speaker();
	have_rx = false;	// the next rx() retunes
	asleep = false;
#else
	(void)m;	// FM only without CONFIG_DMR
#endif
}

static bool tx_stop(void);

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone_in)
{
	if (!supported || !board_in_band(hz, false)) {
		return false;
	}
#if CONFIG_DMR && GD88_TX
	dmr_disarm();	// a retune ends DMR TX
#endif
	bool from_tx = tx_stop();
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = rx_tone_in != NULL ? *rx_tone_in : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	asleep = false;

	ref_trim(hz);
	tx_paths_off();
	bk4819_set_state(&rf_main, BK4819_STATE_IDLE);
	lo_tune(hz);
	bk4819_set_freq(&rf_main, IF_HZ);
#if CONFIG_DMR
	if (mode == RADIO_MODE_DMR) {
		gd88_dmr_lo_side(lo_hz_for(hz) > hz);
	}
#endif
	if (mode != RADIO_MODE_DMR) {	// DMR: the bandwidth and sub-audio are the digital set's
		bk4819_set_bandwidth(&rf_main, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
		bk4819_set_rx_tone(&rf_main, rx_tone.type == TONE_CTCSS ? BK4819_TONE_CTCSS : BK4819_TONE_NONE, rx_tone.value);
	}
#if GD88_BAND_LO_GPIO
	lo_band(hz);
#endif
	DAC_DATA_C = DAC_C_RX;
	if (from_tx) {
		hal_delay_us(TX_END_PATH_US);
	}
	gd88_path_rx(hz, true, mode == RADIO_MODE_DMR);
	bk4819_set_state(&rf_main, BK4819_STATE_RX);
	if (from_tx) {
		hal_delay_us(TX_END_RX_US);
	}
	return true;
}

static void audio(bool on);

#if CONFIG_DMR && GD88_TX
// DMR TX, armed at the key-up. The RF stays receiving until the first burst's RF TX interrupt; the modem's bias gets
// the modulation level for the TX frequency, the main chip the digital TX words (deviation off)
static bool dmr_tx(uint32_t hz, uint8_t power)
{
	gd88_tx_plan_t plan;
	if (!supported || !board_in_band(hz, true) || !gd88_cal_tx_plan(hz, power >= 1, GD88_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	if (dmr_armed) {
		gd88_dmr_tx_disarm();
	}
	if (!gd88_pa_arm(hz, plan.pa, plan.on_us, plan.off_us)) {
		return false;
	}
	tx_on_us = plan.on_us;
	tx_off_us = plan.off_us;
	tx_hz = hz;
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias_dc(gd88_cal_tx_center(), gd88_cal_mod(hz)));
	bk4819_dmr_tx_prepare(&rf_main);
	dmr_armed = true;
	gd88_dmr_tx_arm();
	return true;
}

static void dmr_disarm(void)
{
	if (dmr_armed) {
		gd88_dmr_tx_disarm();
		dmr_armed = false;
		gd88_pa_disarm();
	}
}

// One burst: the TX line high (B starts its PA sequence: the page's TX-on delay, PA select, 200 us, supply, ramp),
// 50 us, main chip to f and the LO off, then as long as B's sequence takes
void gd88_burst_on(gd88_wait_fn wait)
{
	gd88_path_rx_front_off();
	wait(TX_RX_OFF_US);
	bk4819_burst_tune(&rf_main, tx_hz, BK4819_STATE_TX_MAIN);
	lo_off();
	wait(tx_on_us + TX_SUPPLY_US + PA_RAMP_US);
}

// After a burst: the hold until the TX-off point (none when now), the TX line low (B waits the page's TX-off delay
// and ramps down), that long, 300 us, the main chip back to the IF and the LO on again, 300 us, the band's RX path,
// 200 us. Returns how late the TX line dropped, in us
uint32_t gd88_burst_off(gd88_wait_fn wait, uint32_t elapsed_us, bool now)
{
	uint32_t late = now ? 0 : gd88_dmr_late_us(elapsed_us, 0);
	if (!now) {
		wait(gd88_dmr_hold_us(elapsed_us, 0));
	}
	tx_paths_off();
	wait(tx_off_us + PA_RAMP_US + PA_SLACK_US);
	wait(TX_END_PATH_US);
	bk4819_burst_tune(&rf_main, IF_HZ, BK4819_STATE_RX);
	lo_resume();
	wait(TX_END_PATH_US);
	gd88_path_rx(rx_hz, true, true);
	wait(TX_END_RX_US);
	return late;
}
#endif

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)bw;	// the deviation isn't switched by bandwidth yet: the chip's init value is used
#if GD88_TX
	if (mode == RADIO_MODE_DMR) {
#if CONFIG_DMR
		return dmr_tx(hz, power);
#else
		return false;
#endif
	}
	uint16_t ctcss = 0;
	if (tx_tone != NULL && tx_tone->type == TONE_CTCSS) {
		if (tx_tone->value < CTCSS_MIN_X10 || tx_tone->value > CTCSS_MAX_X10) {
			return false;
		}
		ctcss = tx_tone->value;
	} else if (tx_tone != NULL && tx_tone->type != TONE_NONE) {
		return false;	// no DCS transmit yet
	}
	gd88_tx_plan_t plan;
	// Power index 1 ("H") is table 0x045; "L" the other table
	if (!supported || !board_in_band(hz, true) || !gd88_cal_tx_plan(hz, power >= 1, GD88_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	tx_stop();
	if (!gd88_pa_arm(hz, plan.pa, plan.on_us, plan.off_us)) {
		return false;
	}
	tx_off_us = plan.off_us;
	bk4819_tx_prepare(&rf_main, ctcss != 0 ? BK4819_TONE_CTCSS : BK4819_TONE_NONE, ctcss);	// deviation and sub-audio words while the chip still receives

	// Receive audio muted at the chip, then the speaker off
	bk4819_set_af(&rf_main, BK4819_AF_MUTE);
	transmitting = true;
	speaker();
	gd88_path_rx_front_off();
	hal_delay_us(TX_RX_OFF_US);
	bk4819_tx_tune(&rf_main, hz);
	lo_off();
	hal_delay_ms(TX_SETTLE_MS);
	hal_delay_us(plan.on_us + TX_SUPPLY_US + PA_RAMP_US + PA_SLACK_US);	// B's PA sequence, begun at the edge
	return true;
#else
	// Without GD88_TX (gd88_rf.h): A's power control isn't known yet, and B is a receiver only
	(void)hz;
	(void)power;
	(void)tx_tone;
	return false;
#endif
}

// End of a transmission without going back to receive: the TX line low (B waits the page's delay, ramps the PA down
// and switches it off), that long, then the main chip to its RX state (still on the TX frequency), and B disarmed.
// True when there was one; rx() then retunes
static bool tx_stop(void)
{
	if (!transmitting) {
		return false;
	}
#if GD88_TX
	tx_paths_off();
	hal_delay_us(tx_off_us + PA_RAMP_US + PA_SLACK_US);
	bk4819_tx_end(&rf_main);
	gd88_pa_disarm();
#endif
	transmitting = false;
	audio(audio_open);
	return true;
}

static void tx_off(void)
{
#if CONFIG_DMR && GD88_TX
	dmr_disarm();	// the RF task finishes the burst it is in; rx() then sets the receiver up again
#endif
	if (have_rx) {
		rx(rx_hz, rx_bw, &rx_tone);	// stops the transmission first
	} else {
		tx_stop();
	}
}

// FM TX tone (1750 Hz burst, roger beep, CW ID): the chip's tone generator instead of the microphone
static bool tx_tone(uint16_t hz)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	bk4819_tone_start(&rf_main, hz, GD88_TX_TONE_GAIN);
	return true;
}

static bool tx_tone_key(bool down)
{
	if (!transmitting || mode != RADIO_MODE_FM || rf_main.tone_hz == 0) {
		return false;
	}
	bk4819_tone_key(&rf_main, down);
	return true;
}

static void status(radio_status_t *st)
{
	if (!supported || asleep) {
		st->rssi_dbm = -160;
		st->noise = UINT8_MAX;		// all noise: 0 reads as a quiet channel, the squelch opens and the screen says receiving
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
	uint16_t r67 = bk4819_read(&rf_main, BK4819_REG_RSSI);
	uint16_t r65 = bk4819_read(&rf_main, BK4819_REG_NOISE);
	last_rssi_raw = r67;
	last_noise_raw = r65;
	st->rssi_dbm = bk4819_rssi_dbm(r67);	// at the main chip's input, the front end's gain not taken out
	st->noise = (uint8_t)(r65 & 0x7Fu);
	st->carrier = false;
	st->tone_ok = bk4819_rx_tone_ok(&rf_main, rx_tone.type == TONE_CTCSS ? BK4819_TONE_CTCSS : BK4819_TONE_NONE);
}

bool gd88_radio_squelch_open(uint8_t level)
{
	if (!supported || !have_rx || transmitting) {
		return false;
	}
	radio_status_t st;
	status(&st);
	if (!st.tone_ok) {
		return false;
	}
	return level == 0 || st.rssi_dbm >= -(int16_t)gd88_cal_squelch_db(rx_hz, level);
}

static void audio(bool on)
{
	audio_open = on;
	if (transmitting) {
		return;		// the speaker stays muted until tx_stop() restores this
	}
	if (supported) {
		// In DMR the modem samples the chip's audio whether or not the speaker is on, so the output stays on
		if (mode == RADIO_MODE_DMR) {
			bk4819_set_af(&rf_main, BK4819_AF_DMR);
		} else {
			bk4819_set_af(&rf_main, on && !vol_muted ? BK4819_AF_FM : BK4819_AF_MUTE);
		}
	}
	speaker();
}

// The speaker volume as the chip's AF DAC gain. DMR: the chip's output is the modem's input, so it keeps its gain
static void volume_apply(void)
{
	if (!supported || mode == RADIO_MODE_DMR || transmitting) {
		return;
	}
	bk4819_set_af_gain(&rf_main, vol_gain);
	bk4819_set_af(&rf_main, audio_open && !vol_muted ? BK4819_AF_FM : BK4819_AF_MUTE);
}

static uint8_t vol_pct = 50;

uint8_t gd88_radio_volume(void)
{
	return vol_pct;
}

static void volume(uint8_t percent)
{
	vol_pct = percent;
	unsigned step = (percent * RADIO_VOLUME_MAX + 50u) / 100u;
	if (step > RADIO_VOLUME_MAX) {
		step = RADIO_VOLUME_MAX;
	}
	vol_muted = step == 0;
	vol_gain = step == 0 ? 0 : (uint8_t)(((step - 1u) * GD88_VOL_GAIN_TOP + (RADIO_VOLUME_MAX - 1u) / 2u) / (RADIO_VOLUME_MAX - 1u));
	volume_apply();
}

#if CONFIG_BATTERY_SAVE
// Battery save, FM only: the main chip idle, the LO off and A's receive path dropped, which B also hears (RX2_PATH) and
// switches the shared front end off. Waking retunes with the last rx() settings
static void rx_sleep(bool sleep)
{
	if (!supported || mode != RADIO_MODE_FM || transmitting || !have_rx || sleep == asleep) {
		return;
	}
	if (sleep) {
		bk4819_set_state(&rf_main, BK4819_STATE_IDLE);
		lo_off();
		gd88_path_rx(rx_hz, false, false);
		asleep = true;
	} else {
		subtone_t t = rx_tone;
		(void)rx(rx_hz, rx_bw, &t);
	}
}
#endif

void gd88_radio_diag(gd88_radio_diag_t *d)
{
	d->main_id = main_id;
	d->lo_id = lo_id;
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->rf_writes = rf_main.writes + rf_lo.writes + lo_at.writes;
	d->hz = rx_hz;
	d->transmitting = transmitting || dmr_armed;
	d->supported = supported;
	gd88_tx_plan_t plan;
	d->pa_low = GD88_TX && have_rx && gd88_cal_tx_plan(rx_hz, false, GD88_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
	d->pa_high = GD88_TX && have_rx && gd88_cal_tx_plan(rx_hz, true, GD88_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
}

const hal_radio_t gd88_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.volume = volume,
	.tx_tone = tx_tone,
	.tx_tone_key = tx_tone_key,
#if CONFIG_BATTERY_SAVE
	.rx_sleep = rx_sleep,
#endif
};
