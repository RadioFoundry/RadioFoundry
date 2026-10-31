// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D radio: the main transceiver and the LO on one bit-banged 3-wire bus, the superhet's RX paths and analog FM
// receive (docs/spec/hardware-db25d.md 3.2, 4). The chips are the PRO's two BK4819 or an older batch's FD6818 with its
// LO (4.8); rfchip_db25d.c drives whichever pair the radio has, the sequences here are the same for all.
//
// Receive on f: the LO synthesizes f + 51.55 MHz, the main chip receives and demodulates at the 51.55 MHz IF, and DAC
// A tunes the front end from the calibration page (6.4).
//
// Analog FM transmit: the main chip is tuned to f itself and modulates the microphone (the stock firmware never uses
// the C7000's FM modulator, 4.1), the LO chip is switched off, and the PA is keyed through the RF switch pins and DAC B
// with its drive from the page (3.2, 6.4). TX is refused without a valid page, outside the bands, or with a tone other
// than CTCSS. A chip that identifies as none of the known parts isn't driven at all.
//
// DMR transmit (make DB25D_DMR_TX=1 with CONFIG_DMR, 4.5): tx() only arms it. The modem's 4FSK goes on the reference
// oscillator, the main chip sends a carrier with its deviation off, and the whole TX sequence above runs once per burst
// (db25d_burst_on / db25d_burst_off, from the RF task in dmr_db25d.c).
#include "db25d.h"
#include "dmr_db25d.h"
#include "config.h"
#include "hal_dtmf.h"
#include "radio/radio.h"
#include "radio/ref_trim.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rfchip_db25d.h"
#include "rf/bk4819/bk4819.h"
#include "modem/hrc7000/hrc7000.h"

#define DAC_C_INIT		1762u			// 3.2: at init, and 1780 once receive is set up
#define DAC_C_RX		1780u

#define IF_HZ			51550000u
#define UHF_FROM_HZ		280000000u

// TX tuning knobs. DB25D_TX_DRIVE_PCT scales the page's PA drive (100 = the stock value, which the SDR bench measured as
// working). The main chip's own output level (0x36) is the stock's 0xBF, written once at init
#ifndef DB25D_TX_DRIVE_PCT
#define DB25D_TX_DRIVE_PCT	100u
#endif
// FM TX tone level, the tone generator's gain (0..127): the stock single tone's 0x30 (48)
#ifndef DB25D_TX_TONE_GAIN
#define DB25D_TX_TONE_GAIN	BK4819_TONE_GAIN_STOCK
#endif
// Speaker volume steps 1..RADIO_VOLUME_MAX -> the AF DAC gain nibble (register 0x48 bits 3:0): the nibble rises with
// the step, step 1 = 0 up to DB25D_VOL_GAIN_TOP at the top step; with 16 steps and TOP 15 that is nibble = step - 1.
// The other fields of 0x48 stay at the stock value. Step 0 is mute. The stock firmware leaves the nibble at 3 after
// its init and has no volume of its own that was found (hardware-db25d.md 3.3). The dB per nibble step is not
// measured (INF): lower DB25D_VOL_GAIN_TOP if the top steps distort at the speaker
#ifndef DB25D_VOL_GAIN_TOP
#define DB25D_VOL_GAIN_TOP	15u
#endif
#define DB25D_VOL_GAIN_DEFAULT	3u
// The same steps on the PCM path (DMR voice, beeps), which leaves the modem's codec line out and never passes the
// main chip's AF gain (4.5): the line out gain, 0 dB at the default step as before the volume reached it, 3 dB a step
// below and up to this many dB at the top step (hrc7000_volume_db). Not measured at the speaker (INF)
#ifndef DB25D_PCM_VOL_TOP_DB
#define DB25D_PCM_VOL_TOP_DB	12
#endif
#define TX_RX_OFF_US		50u		// 3.2: RX paths off, then this long before the transmitter is tuned
#define TX_SETTLE_MS		3u		// ours: the chip's VCO calibration, before the PA is selected. The stock has none beyond the page's delay
#define TX_SUPPLY_US		200u		// 3.2: PA select and PTA23 low, then this long before the TX supply
#define TX_END_PATH_US		300u		// 3.2: PA select off and PTA23 high, then this long before the receiver is retuned
#define TX_END_RX_US		200u		// after the RX path is switched on again
#define BIAS_DEFAULT_HZ		450000000u	// the modulation balance's frequency before one is known
#define CTCSS_MIN_X10		600u
#define CTCSS_MAX_X10		2600u

static hrc7000_t modem;
static bool supported;		// both chips answered as parts rfchip_db25d.c drives
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static bool transmitting;
static bool audio_open;		// what audio() was last asked, restored after a transmission
static bool vol_muted;		// volume step 0: the chip output stays off whatever audio() asks
static uint8_t vol_gain = DB25D_VOL_GAIN_DEFAULT;	// the AF DAC gain nibble the volume step selects
static uint8_t vol_dac;		// the modem's line out gain code the volume step selects (0 = 0 dB, the default step)
static bool tx_uhf;
static uint16_t tx_pa;
static uint16_t tx_off_us;
static bool dmr_armed;		// DMR TX armed by tx(): the bursts key the RF (dmr_db25d.c)
#if CONFIG_DMR && DB25D_DMR_TX
static uint16_t tx_on_us;
static uint8_t tx_lead;		// the RF TX interrupt's lead, 100 us steps (db25d_dmr_rf_tx_lead)
static uint32_t tx_hz;
static void dmr_disarm(void);
#endif
static radio_mode_t mode = RADIO_MODE_FM;	// DMR only with CONFIG_DMR (transmit with DB25D_DMR_TX, dmr_db25d.c)
static uint16_t last_rssi_raw, last_noise_raw, last_css_raw;

// ---- RF paths (3.2) ----

static void tx_paths_off(void)
{
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_TX_SUPPLY_A, false);
	hrc_pin_write(PIN_TX_SUPPLY_B, false);
	hrc_pin_write(PIN_UHF_TX, false);
	hrc_pin_write(PIN_VHF_TX, false);
}

static void rx_paths(uint32_t hz, bool on)
{
	bool uhf = hz >= UHF_FROM_HZ;
	hrc_pin_write(PIN_RX_SUPPLY, on);
	hrc_pin_write(PIN_RX_FRONT_END, on);
	hrc_pin_write(PIN_UHF_RX, on && uhf);
	hrc_pin_write(PIN_VHF_RX, on && !uhf);
}

// The PA control DAC is powered only for a transmission
static void apc_ramp(uint16_t target, bool up)
{
	for (uint16_t i = 0; i <= DB25D_APC_STEPS; i++) {
		DAC_DATA_B = db25d_apc_level(target, up ? i : (uint16_t)(DB25D_APC_STEPS - i));
		hal_delay_us(DB25D_APC_STEP_US);
	}
}

// The reference trim (hal_radio_t.ref_trim): both BK4819s run from the TCXO, so the trim retunes them instead of
// moving its control voltage. Receive: the LO at f x (1 + trim) + IF, which leaves the signal on the main chip's IF
// (the IF's own share of the error cancels, both chips running from the TCXO). Transmit: the main chip at f x (1 + trim)
static volatile int16_t trim_ppb;
// The trim the LO was tuned with at the last rx(): DMR bursts use it too, so a change during the hotspot moves
// transmit and receive together, at the next tune
static int16_t tuned_ppb;

static uint32_t synth_hz(uint32_t hz)
{
	return ref_trim_tune_hz(hz, tuned_ppb);
}

#if CONFIG_REF_TRIM
// Taken at the next tune, so the RF chips' bus stays the radio task's: true asks the core for one
static bool ref_trim(int16_t ppb)
{
	trim_ppb = ppb;
	return true;
}
#endif

// The C7000 modem's DC bias, as the stock firmware writes it at every receive set-up, in FM too (4.6): the TX center byte
// of the page in SIG_CENTER and the two-point bias level with the modulation balance in RF_MOD_BIAS_CTRL. It is the
// voltage on the TCXO's control pin, so it sets the frequency of both RF chips (the factory's tune; the user's trim
// retunes instead: synth_hz)
static void write_bias(uint32_t hz)
{
	uint8_t c = db25d_cal_tx_center();
	hrc7000_wr(&modem, C7_SIG_CENTER, hrc7000_sig_center_dc(c));
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias_dc(c, db25d_cal_mod(hz)));
}

static void init(void)
{
	db25d_rf_bus_init();
	tx_paths_off();
	// As the stock init, except that the PA control stays powered down until a transmission
	DAC_PD_MODE_EN = 0;
	DAC_PD_CTRL = DAC_B_OFF;
	DAC_DATA_A = 0;
	DAC_DATA_C = DAC_C_INIT;
	rx_paths(0, false);

	// The modem block powered as the DMR glue does, so the reference trim reaches the TCXO in FM builds too
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	hrc7000_set_dac_gain(&modem, vol_dac);	// before the init, which writes it: a volume given before init() holds
	hrc7000_power_on_init(&modem, db25d_cal_tx_center(), DB25D_MIC_GAIN_DB);
	write_bias(BIAS_DEFAULT_HZ);

	supported = db25d_rf_init();
	if (!supported) {
		return;
	}
#if CONFIG_DMR
	db25d_dmr_init(&modem);
#endif
}

static void volume_apply(void);

#ifndef DB25D_DMR_SPK_SWITCH
#define DB25D_DMR_SPK_SWITCH	0	// 1: PTC18 stays high in DMR (the build before the stock trace)
#endif

static volatile bool pcm_on;	// the modem plays CPU samples (a beep, DMR voice): db25d_speaker_pcm()

// PTC18 picks what the speaker amplifier hears (3.3): high for the main chip's AF (FM), low for the modem's line out
// (DMR voice and beeps, as the stock firmware's call start and beeps do)
static void speaker_source(void)
{
	hrc_pin_write(PIN_AUDIO_SWITCH, (mode != RADIO_MODE_DMR && !pcm_on) || DB25D_DMR_SPK_SWITCH != 0);
}

// The speaker pins: on for an open squelch (audio()) or while the modem plays samples. DMR voice never opens the
// squelch path (the radio core leaves the speaker to the PCM stream), so without the PCM term a call played silently.
// Volume step 0 keeps the amplifier off: the line out's least gain (-31 dB) isn't silent.
// Off: the amplifier first, so the source switch isn't heard; on: the source first
static void speaker_pins(void)
{
	bool on = (audio_open || pcm_on) && !transmitting && !vol_muted;
	uint32_t s = hrc_irq_save();	// also called from the audio task (db25d_speaker_pcm): the pin writes are read-modify-write
	if (!on) {
		hrc_pin_write(PIN_SPK_BODY, true);
	}
	speaker_source();
	hrc_pin_write(PIN_SPK_SOURCE, !on);
	hrc_pin_write(PIN_MIC_PATH, on);
	if (on) {
		hrc_pin_write(PIN_SPK_BODY, false);
	}
	hrc_irq_restore(s);
}

void db25d_speaker_pcm(bool on)
{
	if (on != pcm_on) {
		pcm_on = on;
		speaker_pins();
	}
}

static void set_mode(radio_mode_t m)
{
#if CONFIG_DMR
	if (!supported || m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR) {
		db25d_dmr_enter(rx_hz);
		mode = RADIO_MODE_DMR;
	} else {
#if DB25D_DMR_TX
		dmr_disarm();
#endif
		db25d_dmr_leave();
		mode = RADIO_MODE_FM;
		volume_apply();
	}
	speaker_source();
	have_rx = false;	// the next rx() retunes
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
#if CONFIG_DMR && DB25D_DMR_TX
	dmr_disarm();	// a retune ends DMR TX (the hotspot's rx() disarms too)
#endif
	bool from_tx = tx_stop();
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = rx_tone_in != NULL ? *rx_tone_in : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;

	write_bias(hz);
	tx_paths_off();
	db25d_main_idle();
	tuned_ppb = trim_ppb;
	db25d_lo_tune(synth_hz(hz) + IF_HZ);	// the LO carries the trim; the main chip stays on the IF
	db25d_main_to_if();
	if (mode != RADIO_MODE_DMR) {	// DMR: the bandwidth and sub-audio are the digital set's (4.5)
		db25d_main_bandwidth(bw);
		db25d_main_rx_tone(rx_tone.type, rx_tone.value);
	}
	DAC_DATA_A = db25d_cal_rx_tune(hz);	// 0 without a page, as before
	DAC_DATA_C = DAC_C_RX;
	if (from_tx) {
		hal_delay_us(TX_END_PATH_US);	// stock TX end: the retune, this long, then the RX path
	}
	rx_paths(hz, true);
	db25d_main_rx_state();
	if (from_tx) {
		hal_delay_us(TX_END_RX_US);
	}
	return true;
}

static void audio(bool on);

#if CONFIG_DMR && DB25D_DMR_TX
// DMR TX, armed at the key-up (4.5). The RF stays as it is, receiving, until the first burst's RF TX interrupt; the
// modem's bias gets the modulation level for the TX frequency, the main chip the digital TX words (deviation off)
static bool dmr_tx(uint32_t hz, uint8_t power)
{
	db25d_tx_plan_t plan;
	if (!supported || !board_in_band(hz, true) || !db25d_cal_tx_plan(hz, power >= 1, DB25D_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	if (dmr_armed) {
		db25d_dmr_tx_disarm();
	}
	tx_uhf = hz >= UHF_FROM_HZ;
	tx_pa = plan.pa;
	tx_on_us = plan.on_us;
	tx_lead = db25d_dmr_rf_tx_lead(plan.on_us);
	tx_hz = hz;
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias_dc(db25d_cal_tx_center(), db25d_cal_mod(hz)));
	hrc7000_set_rf_tx_lead(&modem, tx_lead);
	db25d_main_dmr_tx_prepare(hz);
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	dmr_armed = true;
	db25d_dmr_tx_arm();
	return true;
}

static void dmr_disarm(void)
{
	if (dmr_armed) {
		db25d_dmr_tx_disarm();
		dmr_armed = false;
		DAC_DATA_B = 0;
		hrc_dac_b_power(false);
	}
}

// One burst, the stock TX on (3.2, 4.4 steps 2-7 without the audio): RX paths off, 50 us, main chip to f and the LO
// off, the page's TX-on delay, PA select and the RX supply off, 200 us, the band's TX supply, the ramp up
void db25d_burst_on(db25d_wait_fn wait)
{
	hrc_pin_write(PIN_RX_FRONT_END, false);
	hrc_pin_write(PIN_UHF_RX, false);
	hrc_pin_write(PIN_VHF_RX, false);
	wait(TX_RX_OFF_US);
	db25d_main_burst(synth_hz(tx_hz), true);
	db25d_lo_off();
	wait(tx_on_us);
	DAC_DATA_B = 0;
	hrc_pin_write(tx_uhf ? PIN_VHF_TX : PIN_UHF_TX, false);
	hrc_pin_write(tx_uhf ? PIN_UHF_TX : PIN_VHF_TX, true);
	hrc_pin_write(PIN_RX_SUPPLY, false);
	wait(TX_SUPPLY_US);
	hrc_pin_write(tx_uhf ? PIN_TX_SUPPLY_B : PIN_TX_SUPPLY_A, false);
	hrc_pin_write(tx_uhf ? PIN_TX_SUPPLY_A : PIN_TX_SUPPLY_B, true);
	apc_ramp(tx_pa, true);
}

// TX off after a burst: the hold until the burst's last symbol (db25d_dmr_hold_us; none when now), the ramp down,
// supplies and PA selects off with PTA23 high, the main chip back to the IF and the LO on again, the RX path of the
// band. The stock's waits between these steps (300, 300 and 200 us) are left out: they kept the receiver off for the
// start of the next slot's burst, which an SFR repeater has to hear (hardware-db25d.md 4.5). Returns how late the ramp
// down started, in us
uint32_t db25d_burst_off(db25d_wait_fn wait, uint32_t elapsed_us, bool now)
{
	uint32_t late = now ? 0 : db25d_dmr_late_us(elapsed_us, tx_lead);
	if (!now) {
		wait(db25d_dmr_hold_us(elapsed_us, tx_lead));
	}
	apc_ramp(tx_pa, false);
	tx_paths_off();
	hrc_pin_write(PIN_RX_SUPPLY, true);
	db25d_main_burst(IF_HZ, false);
	db25d_lo_resume();
	rx_paths(rx_hz, true);
	return late;
}
#endif

// 3.2 "To TX", with the PA ramp after the supply (the stock ramps up from the end of its sequence too, INF)
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	if (mode == RADIO_MODE_DMR) {
#if CONFIG_DMR && DB25D_DMR_TX
		return dmr_tx(hz, power);
#else
		return false;	// DMR TX isn't built (make DB25D_DMR_TX=1)
#endif
	}
	subtone_t tone = tx_tone != NULL ? *tx_tone : (subtone_t){ TONE_NONE, 0 };
	if (tone.type == TONE_CTCSS && (tone.value < CTCSS_MIN_X10 || tone.value > CTCSS_MAX_X10)) {
		return false;
	}
	if ((tone.type == TONE_DCS || tone.type == TONE_DCS_INV) && tone.value > 0777u) {
		return false;
	}
	db25d_tx_plan_t plan;
	// Power index 1 ("H") is the stock's level 1, table 0x045; "L" the other table
	if (!supported || !board_in_band(hz, true) || !db25d_cal_tx_plan(hz, power >= 1, DB25D_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	tx_stop();
	tx_uhf = hz >= UHF_FROM_HZ;
	tx_pa = plan.pa;
	tx_off_us = plan.off_us;
	// Deviation and sub-audio words go in while the chip is still receiving, as the stock firmware does
	db25d_main_tx_prepare(tone.type, tone.value, hz, bw);

	// Receive audio muted at the chip before any audio pin moves, then the stock "audio off" pin state (3.3): with
	// PTB14 raised here instead, the speaker popped at every key-up. That the microphone still reaches the chip is unchecked
	db25d_main_af(DB25D_AF_MUTE);
	hrc_pin_write(PIN_SPK_SOURCE, true);
	hrc_pin_write(PIN_SPK_BODY, true);
	hrc_pin_write(PIN_MIC_PATH, false);
	// RX paths off (PTA23 stays high until the PA is selected)
	hrc_pin_write(PIN_RX_FRONT_END, false);
	hrc_pin_write(PIN_UHF_RX, false);
	hrc_pin_write(PIN_VHF_RX, false);
	hal_delay_us(TX_RX_OFF_US);
	db25d_main_tx_tune(synth_hz(hz));
	db25d_lo_off();
	hal_delay_ms(TX_SETTLE_MS);
	hal_delay_us(plan.on_us);

	// PA select (UHF: PTA26 high, PB3 low; VHF the other way round) with the RX supply off, then the one TX supply of the
	// band (UHF: PTA27 high, PTB2 low; VHF the other way round)
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	hrc_pin_write(tx_uhf ? PIN_VHF_TX : PIN_UHF_TX, false);
	hrc_pin_write(tx_uhf ? PIN_UHF_TX : PIN_VHF_TX, true);
	hrc_pin_write(PIN_RX_SUPPLY, false);
	hal_delay_us(TX_SUPPLY_US);
	hrc_pin_write(tx_uhf ? PIN_TX_SUPPLY_B : PIN_TX_SUPPLY_A, false);
	hrc_pin_write(tx_uhf ? PIN_TX_SUPPLY_A : PIN_TX_SUPPLY_B, true);
	transmitting = true;
	apc_ramp(tx_pa, true);
	return true;
}

// End of a transmission without going back to receive (the stock order, 3.2): the main chip to its RX state first
// (still on the TX frequency), the page's delay, the ramp down, supply and PA select off, PTA23 high. True when there
// was one; rx() then retunes and waits the stock's 300 and 200 us
static bool tx_stop(void)
{
	if (!transmitting) {
		return false;
	}
	db25d_main_tx_end();	// ends a DCS tail still on
	hal_delay_us(tx_off_us);
	apc_ramp(tx_pa, false);
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_TX_SUPPLY_A, false);
	hrc_pin_write(PIN_TX_SUPPLY_B, false);
	hrc_pin_write(PIN_UHF_TX, false);
	hrc_pin_write(PIN_VHF_TX, false);
	hrc_pin_write(PIN_RX_SUPPLY, true);
	hrc_dac_b_power(false);
	transmitting = false;
	audio(audio_open);
	return true;
}

static void tx_off(void)
{
#if CONFIG_DMR && DB25D_DMR_TX
	dmr_disarm();	// the RF task finishes the burst it is in; rx() then sets the receiver up again
#endif
	if (have_rx) {
		rx(rx_hz, rx_bw, &rx_tone);	// stops the transmission first
	} else {
		tx_stop();
	}
}

// FM TX tone (1750 Hz burst, roger beep, CW ID): the chip's tone generator instead of the microphone (0xC3FA). The chip
// drops it at the end of the transmission (db25d_main_tx_end)
static bool tx_tone(uint16_t hz)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	db25d_main_tone(hz, DB25D_TX_TONE_GAIN);
	return true;
}

#if CONFIG_DTMF
// DTMF (radio-features.md 3.7): the chip's two tone generators, as the stock DTMF. Dropped at the end of the
// transmission like a tone
static bool tx_dtmf(char digit)
{
	uint16_t low = 0, high = 0;
	if (!transmitting || mode != RADIO_MODE_FM || (digit != 0 && !dtmf_pair(digit, &low, &high))) {
		return false;
	}
	db25d_main_dtmf(low, high);
	return true;
}
#endif

// The DCS turn-off tail (4.7); the core holds the carrier for it
static bool tx_tail(bool on)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	return db25d_main_tail(on);
}

static bool tx_tone_key(bool down)
{
	if (!transmitting || mode != RADIO_MODE_FM || !db25d_main_tone_on()) {
		return false;
	}
	db25d_main_tone_key(down);
	return true;
}

static void status(radio_status_t *st)
{
	if (!supported) {
		st->rssi_dbm = -160;
		st->noise = UINT8_MAX;		// all noise: 0 reads as a quiet channel, the squelch opens and the screen says receiving
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
	// 0x0C on every poll, tone or not: the diagnostics page shows it (which sub-audio the chip hears)
	db25d_main_status(&last_rssi_raw, &last_noise_raw, &last_css_raw);
	// At the main chip's input: the front end's gain isn't taken out yet (needs the calibration page). Both chip
	// kinds read RSSI as half dB with a 160 dB offset
	st->rssi_dbm = bk4819_rssi_dbm(last_rssi_raw);
	st->noise = (uint8_t)(last_noise_raw & 0x7Fu);
	st->carrier = false;
	st->tone_ok = db25d_main_tone_heard(last_css_raw, rx_tone.type);
}

// The stock audio-on and audio-off steps (hardware-db25d.md 3.3) with the body speaker only. The mic speaker stays off:
// its pin low also darkens the backlight
static void audio(bool on)
{
	audio_open = on;
	if (transmitting) {
		return;		// the speaker stays muted until tx_stop() restores this
	}
	if (supported) {
		// In DMR the modem samples the chip's audio whether or not the speaker is on, so the output stays on, with the
		// stock digital word (4.5)
		if (mode == RADIO_MODE_DMR) {
			db25d_main_af(DB25D_AF_DMR);
		} else {
			db25d_main_af(on && !vol_muted ? DB25D_AF_FM : DB25D_AF_MUTE);
		}
	}
	speaker_pins();
}

// The speaker volume (the radio core passes the step as a percent, no knob here): the chip's AF DAC gain, register 0x48 bits
// 3:0, stepping with DB25D_VOL_GAIN_TOP. Step 0 closes the chip output as well, as the gain alone isn't known to be
// silent. DMR: the chip's output is the modem's input (4.5), so it keeps the stock gain; the volume comes back with FM.
// The PCM path's gain (vol_dac) is set in volume() in every mode
static void volume_apply(void)
{
	if (!supported || mode == RADIO_MODE_DMR || transmitting) {
		return;		// during a transmission nothing here moves: tx_stop() runs audio(), and the gain is in place
	}
	db25d_main_af_gain(vol_gain);
	db25d_main_af(audio_open && !vol_muted ? DB25D_AF_FM : DB25D_AF_MUTE);
}

static void volume(uint8_t percent)
{
	unsigned step = (percent * RADIO_VOLUME_MAX + 50u) / 100u;	// the step back from its percent
	if (step > RADIO_VOLUME_MAX) {
		step = RADIO_VOLUME_MAX;
	}
	vol_muted = step == 0;
	vol_gain = step == 0 ? 0 : (uint8_t)(((step - 1u) * DB25D_VOL_GAIN_TOP + (RADIO_VOLUME_MAX - 1u) / 2u) / (RADIO_VOLUME_MAX - 1u));
	vol_dac = hrc7000_dac_code(hrc7000_volume_db(step, RADIO_VOLUME_DEFAULT, RADIO_VOLUME_MAX, DB25D_PCM_VOL_TOP_DB));
	if (modem.buf != NULL) {	// attached by init(), which applies vol_dac itself
		hrc7000_set_dac_gain(&modem, vol_dac);
		speaker_pins();		// step 0 and back: the amplifier
	}
	volume_apply();
}

void db25d_radio_diag(db25d_radio_diag_t *d)
{
	d->main_id = db25d_rf_main_id();
	d->lo_id = db25d_rf_lo_id();
	db25d_rf_variant(d->variant);
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->css_raw = last_css_raw;
	d->rf_writes = db25d_rf_writes();
	d->hz = rx_hz;
	d->transmitting = transmitting || dmr_armed;
	db25d_tx_plan_t plan;
	d->pa_low = have_rx && db25d_cal_tx_plan(rx_hz, false, DB25D_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
	d->pa_high = have_rx && db25d_cal_tx_plan(rx_hz, true, DB25D_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
}

const hal_radio_t db25d_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.volume = volume,
	.volume_pcm = true,	// the modem's line out gain scales DMR voice and beeps (vol_dac)
	.tx_tone = tx_tone,
	.tx_tail = tx_tail,
	.tx_tone_key = tx_tone_key,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
};
