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
#include "hal_dtmf.h"
#include "radio/radio.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/rfbus/rfbus.h"
#include "rf/bk4819/bk4819.h"
#include "rf/at1141/at1141.h"
#include "rf/superhet/superhet.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/at1141/at1141_superhet.h"
#include "modem/hrc7000/hrc7000.h"
#include "audio/afsk_tonegen/afsk_tonegen.h"
#if CONFIG_DMR && GD88_TX && CONFIG_SFR
#include "rx2/rx2_dmr.h"
#endif

// TODO(gd88): unknown, needs a radio: DAC C's role on each SoC. These are hardware-db25d.md's levels. Not receive gain
// or front-end tuning: on A, 0 to 4095 leave the RSSI at 146, 162 and 461 MHz unchanged (2026-10-27); a reference
// frequency trim or a transmit-only role is left
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
// AFSK at level 50: about 3 kHz peak (aprs.md 5.2) through 0x40's deviation scale (bk4819_tone_deviation), not the
// tone's gain (the stock word gave 4.8 kHz at any gain from 13 to 48). The deviation rises about 10 Hz per count from
// GD88_AFSK_DEV_ZERO (SDR bench, 0x448 to 0x4D9); level 50 adds GD88_AFSK_DEV_SPAN (3.0 kHz measured; level 25 2.1 kHz).
// Never above the stock voice word (4.8 kHz): level 100 would give 6.8 kHz
#ifndef GD88_AFSK_DEV_SPAN
#define GD88_AFSK_DEV_SPAN	0x129u
#endif
#define GD88_AFSK_DEV_ZERO	0x2FAu
#define GD88_AFSK_DEV_MAX	0x4D0u
// Speaker volume steps 1..RADIO_VOLUME_MAX -> the AF DAC gain nibble (register 0x48 bits 3:0), step 1 = 0 up to
// GD88_VOL_GAIN_TOP. Step 0 is mute
#ifndef GD88_VOL_GAIN_TOP
#define GD88_VOL_GAIN_TOP	15u
#endif
#define GD88_VOL_GAIN_DEFAULT	3u
// What the modem plays (beeps, DMR voice) leaves its line out, not the main chip's AF, so the knob sets the line out's
// gain too, as the original firmware does: 3 dB a step, 0 dB at the middle step and GD88_PCM_VOL_TOP_DB at the top.
// At 0 dB a beep was too quiet to hear over the microphone at a loud knob; +16 dB was as loud as the receiver's noise
// (bench 2026-10-27)
#define GD88_PCM_VOL_DEF_STEP	(RADIO_VOLUME_MAX / 2u)
#ifndef GD88_PCM_VOL_TOP_DB
#define GD88_PCM_VOL_TOP_DB	24
#endif
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

static rfbus_3w_t bus_main, bus_lo;
static bk4819_t rf_main, rf_lo;
static at1141_t lo_at;		// the LO when it answers as an AT1141
static bool lo_is_at1141;
static superhet_lo_t sh_lo;	// rf_lo or lo_at, bound at init
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
static uint8_t vol_pct = 50;

static unsigned vol_step(uint8_t percent)
{
	unsigned step = (percent * RADIO_VOLUME_MAX + 50u) / 100u;
	return step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step;
}

static uint8_t pcm_gain(unsigned step)
{
	return hrc7000_dac_code(hrc7000_volume_db(step, GD88_PCM_VOL_DEF_STEP, RADIO_VOLUME_MAX, GD88_PCM_VOL_TOP_DB));
}
static bool dmr_armed;		// DMR TX armed by tx(): the bursts key the RF (dmr_gd88.c)
#if GD88_TX || GD88_DMR_TX
static uint16_t tx_off_us;
#endif
#if CONFIG_DMR && GD88_DMR_TX
static uint16_t tx_on_us;
static uint32_t tx_hz;
static void dmr_disarm(void);
#endif
#if CONFIG_DMR && GD88_TX && CONFIG_SFR
static bool dmr_remote;		// DMR TX armed on B's transmitter (gd88_dmr_split)
static uint8_t tx_power;
#endif
static radio_mode_t mode = RADIO_MODE_FM;
static uint16_t last_rssi_raw, last_noise_raw;
#if CONFIG_APRS && GD88_TX
static volatile bool afsk_on;	// AFSK tones playing: the chip's bus belongs to the symbol interrupt
static const afsk_tonegen_board_t afsk_board;
#endif

// ---- 3-wire bus: clock and data shared, one enable per chip ----

static void rf_clk(bool level) { hrc_pin_write(GD88_PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(GD88_PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(GD88_PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(GD88_PIN_RF_DATA); }
static void cs_main(bool level) { hrc_pin_write(GD88_PIN_RF_SEN_MAIN, level); }
static void cs_lo(bool level) { hrc_pin_write(GD88_PIN_RF_SEN_LO, level); }

static const rfbus_3w_pins_t pins_main = {
	.cs = cs_main, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

static const rfbus_3w_pins_t pins_lo = {
	.cs = cs_lo, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

// ---- LO chip: a BK4819 or an AT1141 on the LO enable ----

static bk4819_tone_t bk_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return BK4819_TONE_CTCSS;
	case TONE_DCS: return BK4819_TONE_DCS;
	case TONE_DCS_INV: return BK4819_TONE_DCS_INV;
	default: return BK4819_TONE_NONE;
	}
}

static uint32_t lo_hz_for(uint32_t hz)
{
	if (lo_is_at1141 && hz >= LOW_SIDE_FROM_HZ && hz <= LOW_SIDE_TO_HZ) {
		return hz - IF_HZ;
	}
	return hz + IF_HZ;
}

static void lo_tune(uint32_t hz)
{
	sh_lo.ops->tune(sh_lo.chip, lo_hz_for(hz));
}

#if GD88_TX || GD88_DMR_TX || CONFIG_BATTERY_SAVE
static void lo_off(void)
{
	sh_lo.ops->off(sh_lo.chip);
}
#endif

#if CONFIG_DMR && GD88_DMR_TX
static void lo_resume(void)
{
	sh_lo.ops->resume(sh_lo.chip);
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
	modem.dac_gain = pcm_gain(vol_step(vol_pct));	// the power-on init writes it
	hrc7000_power_on_init(&modem, gd88_cal_tx_center(), GD88_MIC_GAIN_DB);
	ref_trim(BIAS_DEFAULT_HZ);

	rfbus_3w_attach(&bus_main, &pins_main);
	rfbus_3w_attach(&bus_lo, &pins_lo);
	bk4819_attach(&rf_main, &bus_main.bus);
#if CONFIG_DMR
	// Before the chip check: the modem's PCM audio (beeps, DMR voice) needs it attached even on a radio whose RF chips
	// aren't driven here. A beep on a NULL modem faulted the radio task at every PTT press (LO chip 0x1100, 2026-10-26).
	// set_mode(), rx() and tx() still refuse to use DMR or RF on such a radio
	gd88_dmr_init(&rf_main, &modem);
#endif
	main_id = rfbus_read(&bus_main.bus, BK4819_REG_ID);
	lo_id = rfbus_read(&bus_lo.bus, BK4819_REG_ID);
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
	// The AT1141 goes off for a transmission and restarts after it (hardware-gd88.md 11.1)
	if (lo_is_at1141) {
		at1141_attach(&lo_at, &bus_lo.bus);
		at1141_init(&lo_at);
		sh_lo = (superhet_lo_t){ &at1141_superhet_lo_restart, &lo_at };
	} else {
		bk4819_attach(&rf_lo, &bus_lo.bus);
		bk4819_init_lo(&rf_lo, BK4819_LO_PA_MAX);
		sh_lo = (superhet_lo_t){ &bk4819_superhet_lo, &rf_lo };
	}
	bk4819_analog_mode(&rf_main);
#if CONFIG_APRS && GD88_TX
	afsk_tonegen_attach(&afsk_board);
#endif
}

static void volume_apply(void);

#if GD88_BAND_LO_GPIO
#define LO_GPIO_UHF		4u
#define LO_GPIO_VHF		5u
// The LO chip's GPIO: the superhet LO ops don't drive a BK4819's, so that one directly
static void lo_gpio(uint8_t pin, bool high)
{
	if (lo_is_at1141) {
		superhet_lo_gpio(&sh_lo, pin, high);
	} else {
		bk4819_gpio(&rf_lo, pin, high);
	}
}

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
#if GD88_DMR_TX
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
#if CONFIG_DMR && GD88_DMR_TX
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
		// An LO above the signal inverts the IF: DCS arrives inverted
		rf_main.rx_inverted = lo_hz_for(hz) > hz;
		bk4819_set_rx_tone(&rf_main, bk_tone(rx_tone.type), rx_tone.value);
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

#if CONFIG_DMR && GD88_DMR_TX
// DMR TX, armed at the key-up. The RF stays receiving until the first burst's RF TX interrupt; the modem's bias gets
// the modulation level for the TX frequency, the main chip the digital TX words (deviation off). With an SFR's split
// on (A), B's transmitter is armed instead: it keys the bursts the modem's caller sends it (core/rx2/rx2_dmr.h)
#if !GD88_TX
// B's own transmitter: the main chip's GPIO5 selects its output to the PA (the original firmware's path 1), GPIO4 the
// band (high: VHF). Per burst like the PA: held high, B's receiver hears nothing, and an SFR's repeat needs B's modem
// to keep the inbound slot timing between bursts
#define OWN_GPIO_PATH		5u
#define OWN_GPIO_VHF		4u
static void own_gpio(bool on, uint32_t hz)
{
	bk4819_gpio(&rf_main, OWN_GPIO_VHF, on && hz < UHF_FROM_HZ);
	bk4819_gpio(&rf_main, OWN_GPIO_PATH, on);
}
#endif

static bool dmr_tx(uint32_t hz, uint8_t power)
{
	gd88_tx_plan_t plan;
	if (!supported || !board_in_band(hz, true) || !gd88_cal_tx_plan(hz, power >= 1, GD88_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	if (dmr_armed) {
		gd88_dmr_tx_disarm();
	}
#if GD88_TX && CONFIG_SFR
	if (rx2_dmr_main_split()) {
		if (dmr_armed) {
			dmr_armed = false;
			gd88_pa_disarm();
		}
		if (!rx2_dmr_main_arm(hz, power)) {
			return false;
		}
		tx_hz = hz;
		tx_power = power;
		dmr_remote = true;
		return true;
	}
	tx_power = power;
#endif
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
#if GD88_TX && CONFIG_SFR
	if (dmr_remote) {
		rx2_dmr_main_arm(0, 0);
		dmr_remote = false;
	}
#endif
}

#if GD88_TX && CONFIG_SFR
bool gd88_dmr_listen(uint32_t *hz, uint8_t *power)
{
	*hz = rx_hz;
	*power = tx_power;
	return mode == RADIO_MODE_DMR && rx_hz != 0;
}

void gd88_dmr_split(bool on)
{
	(void)on;	// dmr_tx reads it
	if (dmr_armed || dmr_remote) {
		uint32_t hz = tx_hz;
		dmr_disarm();
		dmr_tx(hz, tx_power);
	}
}
#endif

#if GD88_TX
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
#else
// B's own burst (the original firmware's path 1): its path pins to its chip in the TX band and its RX path off, 50 us,
// the chip to f and the LO off, the page's TX-on delay, then its PA: select, 200 us, supply, the ramp
void gd88_burst_on(gd88_wait_fn wait)
{
	gd88_path_own_tx(true, tx_hz);
	own_gpio(true, tx_hz);
	wait(TX_RX_OFF_US);
	bk4819_burst_tune(&rf_main, tx_hz, BK4819_STATE_TX_MAIN);
	lo_off();
	wait(tx_on_us);
	gd88_pa_own(true);
}

// After it: the hold, the page's TX-off delay, the PA down, 300 us, the chip back to the IF and the LO on, 300 us, the
// path pins and the RX path back, 200 us
uint32_t gd88_burst_off(gd88_wait_fn wait, uint32_t elapsed_us, bool now)
{
	uint32_t late = now ? 0 : gd88_dmr_late_us(elapsed_us, 0);
	if (!now) {
		wait(gd88_dmr_hold_us(elapsed_us, 0));
		wait(tx_off_us);
	}
	gd88_pa_own(false);
	wait(TX_END_PATH_US);
	bk4819_burst_tune(&rf_main, IF_HZ, BK4819_STATE_RX);
	lo_resume();
	wait(TX_END_PATH_US);
	own_gpio(false, tx_hz);
	gd88_path_own_tx(false, tx_hz);
	gd88_path_rx(rx_hz, true, true);
	wait(TX_END_RX_US);
	return late;
}
#endif
#endif

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)bw;	// the deviation isn't switched by bandwidth yet: the chip's init value is used
#if GD88_DMR_TX
	if (mode == RADIO_MODE_DMR) {
#if CONFIG_DMR
		return dmr_tx(hz, power);
#else
		return false;
#endif
	}
#endif
#if GD88_TX
	subtone_t tone = tx_tone != NULL ? *tx_tone : (subtone_t){ TONE_NONE, 0 };
	if (tone.type == TONE_CTCSS && (tone.value < CTCSS_MIN_X10 || tone.value > CTCSS_MAX_X10)) {
		return false;
	}
	if ((tone.type == TONE_DCS || tone.type == TONE_DCS_INV) && tone.value > 0777u) {
		return false;
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
	bk4819_tx_prepare(&rf_main, bk_tone(tone.type), tone.value);	// deviation and sub-audio words while the chip still receives

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
	// Without GD88_TX (gd88_rf.h): B is a receiver only
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
#if CONFIG_DMR && GD88_DMR_TX
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

#if CONFIG_APRS && GD88_TX
// APRS AFSK (afsk_tonegen.h): the tone generator again, retuned at each symbol by timer 4. level 50 is the tone
// burst's level
static bool afsk_begin(uint16_t hz, uint8_t level)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	uint32_t scale = GD88_AFSK_DEV_ZERO + (uint32_t)GD88_AFSK_DEV_SPAN * level / 50u;
	bk4819_tone_start(&rf_main, hz, GD88_TX_TONE_GAIN);
	bk4819_tone_deviation(&rf_main, (uint16_t)(scale > GD88_AFSK_DEV_MAX ? GD88_AFSK_DEV_MAX : scale));
	afsk_on = true;
	return true;
}

static void afsk_tone(uint16_t hz)
{
	bk4819_tone_freq(&rf_main, hz);
}

static void afsk_end(void)
{
	afsk_on = false;
	bk4819_tone_stop(&rf_main);
}

static const afsk_tonegen_board_t afsk_board = {
	.begin = afsk_begin, .tone = afsk_tone, .end = afsk_end,
	.timer_start = hrc_symbol_timer_start, .timer_stop = hrc_symbol_timer_stop,
};
#endif

#if CONFIG_DTMF
// DTMF (radio-features.md 3.7): the main chip's two tone generators. Dropped at the end of the transmission like a tone
static bool tx_dtmf(char digit)
{
	uint16_t low = 0, high = 0;
	if (!transmitting || mode != RADIO_MODE_FM || (digit != 0 && !dtmf_pair(digit, &low, &high))) {
		return false;
	}
	bk4819_dtmf_start(&rf_main, low, high);
	return true;
}
#endif

// The DCS turn-off tail; the core holds the carrier for it
static bool tx_tail(bool on)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	if (on) {
		return bk4819_tx_tail_start(&rf_main);
	}
	bk4819_tx_tail_end(&rf_main);
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
#if CONFIG_APRS && GD88_TX
	if (afsk_on) {
		// The AFSK interrupt owns the bus: the last reading
		st->rssi_dbm = bk4819_rssi_dbm(last_rssi_raw);
		st->noise = (uint8_t)(last_noise_raw & 0x7Fu);
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
#endif
	uint16_t r67 = bk4819_read(&rf_main, BK4819_REG_RSSI);
	uint16_t r65 = bk4819_read(&rf_main, BK4819_REG_NOISE);
	last_rssi_raw = r67;
	last_noise_raw = r65;
	st->rssi_dbm = bk4819_rssi_dbm(r67);	// at the main chip's input, the front end's gain not taken out
	st->noise = (uint8_t)(r65 & 0x7Fu);
	st->carrier = false;
	st->tone_ok = bk4819_rx_tone_ok(&rf_main, bk_tone(rx_tone.type));
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

uint8_t gd88_radio_volume(void)
{
	return vol_pct;
}

static void volume(uint8_t percent)
{
	vol_pct = percent;
	unsigned step = vol_step(percent);
	vol_muted = step == 0;
	vol_gain = step == 0 ? 0 : (uint8_t)(((step - 1u) * GD88_VOL_GAIN_TOP + (RADIO_VOLUME_MAX - 1u) / 2u) / (RADIO_VOLUME_MAX - 1u));
	if (modem.regs != NULL) {
		hrc7000_set_dac_gain(&modem, pcm_gain(step));	// the init sets it from vol_pct before then
	}
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
	d->rf_writes = bus_main.bus.writes + bus_lo.bus.writes;
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
	.tx_tail = tx_tail,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
#if CONFIG_BATTERY_SAVE
	.rx_sleep = rx_sleep,
#endif
};
