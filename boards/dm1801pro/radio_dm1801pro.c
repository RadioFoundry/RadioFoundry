// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-1801 Pro radio: FD6818 RF transceiver (bit-banged, its own register set), the HR_C7000 baseband for analog FM,
// separate RX and TX band paths and the PA drive on DAC channel B (docs/spec/hardware-dm1801pro.md 5; modem-hrc7000.md
// 4, 5.1, 8) for analog FM and DMR. The DMR slot engine, RF-burst task and PCM audio are the shared HR_C7000 glue
// (platform/hrc7000/dmr_glue.c, audio_pcm.c); this file supplies the per-burst RF switch and the PA key.
//
// Transmit needs the factory calibration (the PA drive comes from it): without it tx() refuses.
#include "dm1801pro.h"
#include "config.h"
#include "pins.h"
#include "audio_route.h"
#include "platform.h"
#include "hal_time.h"
#include "cal/baofeng/baofeng_cal.h"
#include "rf/fd6818/fd6818.h"
#include "modem/hrc7000/hrc7000.h"
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "hrc_afsk.h"
#include "radio/ref_trim.h"
#include "radio/apc_ramp.h"
#include "radio/pa_keyup.h"

// DAC C biases the modem's AF input. The radio's own value: with the other HR_C7000 boards' 0x06E2 the modem missed the sync of
// about half of the DMR senders (bench, strong and weak signal); with 0x067E it took every burst
#define DAC_C_INIT		0x067Eu

#define UHF_FROM_HZ		200000000u	// VHF / UHF split of the path selects (5.2)
// Mic gains in GCR_ADCL (dB), before the user's offset: DMR at power-on, DMR entry and DMR TX start, FM at each FM TX
// start. The other HR_C7000 boards' 11 dB for all three until the SDR bench sets the FM ones (hardware-dm1801pro.md 12: speech on FM
// gave 1.4 kHz peak, low)
#define MIC_GAIN_DMR_DB		11u
#define MIC_GAIN_FM_12K5_DB	11u
#define MIC_GAIN_FM_25K_DB	11u
#define RF_BIT_SCALE_US		1u		// 1 us per half clock, still slower than the radio (5.1)
#define PA_RAMP_STEPS		40u		// 5.4
#define PA_RAMP_STEP_US		50u
#define FD_REG_TX_AUX		0x50u		// 3B18 after the TX state, before the TX path comes on (5.4)
#define FD_REG40_BASE		0x3480u		// FM transmit: 0x40 = this + the calibration byte (5.2, 6.4)
// DMR bursts on the shared RF-burst task (dmr_rfburst.h), timed from the RF TX interrupt. The RF leaves receive 0.6 ms
// ahead of the lead point, not earlier: an earlier switch cut off the tail of the inbound burst before ours (a clipped
// SFR header lost the next superframe; at 2.0 ms the frame before ours still carried most of the voice bit errors).
// Swept 0.4-3.1 ms in hardware-dm1801pro.md 5.6. The interrupt comes 0.5 ms before that, so a late wake of the task
// (up to 0.42 ms) doesn't make the switch late. The drive is keyed 0.5 ms before the first symbol (one step unless the key-up below has a boost; it was
// 0.45 ms before to 0.65 ms after with the glue's PA one-shot, 5.6)
#define RF_TX_LEAD		11u		// 100 us steps (RF_CONTROL b13:8)
#define RF_SWITCH_LEAD_US	600u		// the RF to TX this long before the lead point
#define PA_AHEAD_US		1000u		// the drive keyed this long before the first symbol
// The DMR key-up (radio/pa_keyup.h, docs/spec/pa-keyup.md). Its points come after the switch to TX, which takes about
// 0.2 ms from its point: the earliest is that long after it
#define SWITCH_US		200u
#define KEYUP_MAX_AHEAD_US	(RF_SWITCH_LEAD_US + HRC_RFBURST_SYMBOL_AFTER_LEAD_US - SWITCH_US)
// No boost: the output follows the drive about 0.65 ms later at every drive, on both bands, so the drive only has to
// come earlier. At 0.5 ms the first symbol was 5-18 dB low; from 0.8 ms it is within 1 dB and the -3 dB point 0.15 ms
// before it, at 1.0 ms 0.3 ms before (hardware-dm1801pro.md 5.6)
static const pa_keyup_cfg_t keyup_cfg = {
	.boost_pct = 100, .hold_pct = 100, .boost_us = 0, .pa_us = PA_AHEAD_US, .drive_us = 0,
};
// TX data advance (hrc7000_t.tx_pre_on) 1.1 ms, not the chip's 0.4 ms: at 0.4 ms the inbound burst came 0.71 ms late
// against the outbound, about 0.11 ms less per step (hardware-dm1801pro.md 5.6)
#define TX_PRE_ON		11u
#define FM_TX_LIMIT_12K5	5u	// FM_DEV_COEF b23:16 (hrc7000_t.fm_tx_limit)
#define FM_TX_LIMIT_25K		0x0Fu
#define FD_REG_NOISE		0x65u
#define FD_REG_RSSI		0x67u
// The RSSI correction (hardware-dm1801pro.md 5.4): another radio as the reference. UHF from the SDR bench's carrier with
// both radios side by side on identical dummy loads (2026-10-20, second run): +16 dB at 451.5 MHz, +11 at two test
// frequencies near 461 MHz, +10 near 467 MHz, the same at every level from -125 to -99 dBm; 400-450 not measured,
// 451's value carried down. VHF from the floor across the band and a NOAA station (+41 dB), first run
static const fd6818_rssi_cal_t rssi_cal[] = {
	{ 136, -26 }, { 140, -36 }, { 150, -36 }, { 162, -41 }, { 174, -40 },
	{ 400, -16 }, { 451, -16 }, { 461, -11 }, { 467, -10 }, { 470, -10 },
};

static fd6818_t fd;
static rfbus_3w_t rf_bus;
static hrc7000_t modem;

static radio_mode_t mode = RADIO_MODE_FM;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static bool asleep;		// rx_sleep(true): the receiver is off until rx_sleep(false) or the next rx()
static bool transmitting;
static uint32_t tx_hz;		// TX frequency (FM: also the TX tone deviation, with tx_wide; DMR: the burst TX path)
static bool tx_wide;
static bool pcm_on;		// a beep or DMR voice plays through the codec
static void ring_play_update(void);
static bool rx_audio;		// the squelch is open (the radio's audio hook)
static bool aprs_listen;	// APRS takes the HR_C7000's FM audio (aprs_listen_route)
static bool afsk_tx;		// APRS tones play through the prompt FIFO into the modulator (afsk_tx_route)

static void speaker_source(void);
static volatile bool dmr_keyed;		// DMR transmission on: the burst sequence may key the drive
static pa_keyup_t ku;			// the transmission's DMR key-up, set before the RF task is armed
static pa_keyup_point_t key_pt[PA_KEYUP_POINTS];
static uint8_t key_n;
static volatile uint16_t tx_drive;	// DAC B value at the top of the ramp (FM) or while keyed (DMR)
static uint16_t last_rssi_raw, last_noise_raw;
static volatile int8_t mic_offset_db;	// hal_radio_t.mic_gain: taken at the next transmission start

// ---- FD6818 bus ----

static void rf_cs(bool level) { hrc_pin_write(PIN_RF_CS, level); }
static void rf_clk(bool level) { hrc_pin_write(PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(PIN_RF_DATA); }
static void rf_delay_us(uint32_t us) { hal_delay_us(us * RF_BIT_SCALE_US); }

static const rfbus_3w_pins_t rf_bus_pins = {
	.cs = rf_cs,
	.clk = rf_clk,
	.data_out = rf_data_out,
	.data_release = rf_data_release,
	.data_in = rf_data_in,
	.delay_us = rf_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

// ---- Band paths and PA ----

// A path select drives its VHF-side and UHF-side pins as a complementary pair; both low = off (5.5). The band follows
// the frequency of that direction, UHF above 200 MHz, as the radio's own band bits do. Receive is checked on both bands
// and transmit on UHF; VHF transmit follows the radio's code but is unchecked on air
static void path_pins(uint8_t vhf_pin, uint8_t uhf_pin, uint32_t hz, bool on)
{
	bool uhf = hz > UHF_FROM_HZ;
	hrc_pin_write(vhf_pin, on && !uhf);
	hrc_pin_write(uhf_pin, on && uhf);
}

static void rx_path(uint32_t hz, bool on) { path_pins(PIN_RX_VHF, PIN_RX_UHF, hz, on); }
static void tx_path(uint32_t hz, bool on) { path_pins(PIN_TX_VHF, PIN_TX_UHF, hz, on); }

// The PA drive as a ramp, so the carrier doesn't splatter (5.4: 40 steps), on the shared APC ramp's curve
static void pa_ramp(uint16_t target, bool up)
{
	for (uint16_t i = 1; i <= PA_RAMP_STEPS; i++) {
		DAC_DATA_B = apc_ramp_level(target, up ? i : (uint16_t)(PA_RAMP_STEPS - i), PA_RAMP_STEPS);
		hal_delay_us(PA_RAMP_STEP_US);
	}
}

static uint16_t fm_bw_reg(radio_bw_t bw)
{
	return bw == RADIO_BW_25K ? fd.variant->bw_fm_25k : fd.variant->bw_fm_12k5;
}

static hrc7000_tone_t c7_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return HRC7000_TONE_CTCSS;
	case TONE_DCS: return HRC7000_TONE_DCS;
	case TONE_DCS_INV: return HRC7000_TONE_DCS_INV;
	default: return HRC7000_TONE_NONE;
	}
}

static fd6818_tone_t fd_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return FD6818_TONE_CTCSS;
	case TONE_DCS: return FD6818_TONE_DCS;
	case TONE_DCS_INV: return FD6818_TONE_DCS_INV;
	default: return FD6818_TONE_NONE;
	}
}

// The PA drive set over the debug link for the next transmission (power calibration; hal_radio_t.tx_drive), 0 = the
// level's. Any task sets it; tx_off clears it
static volatile uint16_t drive_override;

// The calibration as the radio's own firmware reads it (hardware-dm1801pro.md 6.4): the band's rows at its TX points,
// the VHF rows up to 200 MHz and the UHF rows above, as for the TX path select
uint16_t dm1801pro_pa_drive(uint32_t hz, uint8_t power)
{
	// Five levels: 2, 3 and 4 are the low, middle and high rows; 0 and 1 are fractions of the low row's drive
	// (hardware-dm1801pro.md 5.6: draft values, to be set with a wattmeter)
	static const uint8_t low_pct[2] = { 37, 65 };
	if (power < 2u) {
		return (uint16_t)(((uint32_t)bf_cal_tx_byte(hz, BF_ROW_PA_LOW) << 4) * low_pct[power] / 100u);
	}
	return (uint16_t)(bf_cal_tx_byte(hz, power == 2u ? BF_ROW_PA_LOW : power == 3u ? BF_ROW_PA_MID : BF_ROW_PA_HIGH) << 4);
}

// RF_MOD_BIAS_CTRL: TX center + 384 in the high half, the band's modulation byte x 0x0101 in the low half. The radio
// multiplies the byte by 1.8 (Wide) or 0.9 (Narrow) on an FM channel; RadioFoundry doesn't, since its FM deviations were set on
// the bench without the factor (hardware-dm1801pro.md 10)
static uint32_t mod_bias(uint32_t hz)
{
	uint8_t b = bf_cal_tx_byte(hz, BF_ROW_MOD);
	return hrc7000_mod_bias(bf_cal_tx_center(), b != 0 ? (uint16_t)(b * 0x0101u) : HRC7000_MODCAL_DEFAULT);
}

static void write_mod_bias(uint32_t hz)
{
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, mod_bias(hz));
}

// The reference trim (hal_radio_t.ref_trim): the FD6818 is programmed f x (1 + trim), at every tune and burst
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

// ---- From the DMR interrupts ----

// One burst's TX on, from the RF task (dmr_rfburst.h), at the switch point: RX path off, FD6818 straight to TX (no
// idle) and the TX aux word, TX path on; then the drive ahead of the first symbol (one write: a burst is 27.5 ms long,
// there is no time for the 2 ms ramp of the analog path). Each path is on only while the FD6818 is in its state, as in
// the radio's own handlers (5.4) (both points from the RF TX interrupt)
static void burst_on(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait)
{
	(void)wait;
	at(hrc_rfburst_switch_at_us(RF_TX_LEAD, RF_SWITCH_LEAD_US));
	rx_path(rx_hz, false);
	fd6818_dmr_burst_at(&fd, true, fd.variant->bw_dmr, synth_hz(tx_hz));
	fd6818_write(&fd, FD_REG_TX_AUX, 0x3B18);
	tx_path(tx_hz, true);
	// The key-up's points in time order. The TX path is on from the switch, so the PA's own point only sets the
	// step's drive when there is no boost
	for (uint8_t i = 0; i < key_n; i++) {
		at(hrc_rfburst_pa_at_us(RF_TX_LEAD, key_pt[i].us));
		if (!dmr_keyed) {
			continue;
		}
		switch (key_pt[i].kind) {
		case PA_KEYUP_PT_BOOST:
			DAC_DATA_B = ku.boost;
			break;
		case PA_KEYUP_PT_HOLD:
			DAC_DATA_B = ku.hold;
			break;
		case PA_KEYUP_PT_DRIVE:
			DAC_DATA_B = tx_drive;
			break;
		default:
			if (ku.boost == 0u) {
				DAC_DATA_B = tx_drive;
			}
			break;
		}
	}
}

// The burst's TX off, after the task's hold to its last symbol: drive off, TX path off, FD6818 straight back to
// receive, RX path on. The TX path switches per burst: a hotspot stays keyed while it listens, and with the TX path on
// it heard nothing
static void burst_off(void)
{
	DAC_DATA_B = 0;
	tx_path(tx_hz, false);
	fd6818_dmr_burst_at(&fd, false, fd.variant->bw_dmr, synth_hz(rx_hz));
	rx_path(rx_hz, true);
}

static const hrc_rfburst_board_t rf_board = { .on = burst_on, .off = burst_off };

static void dmr_tx_end(void);

static const hrc_dmr_board_t dmr_board = { .burst = &rf_board, .tx_end = dmr_tx_end };

// Per-burst keying off (task level): waits for the RF task to finish the burst it is in and leave the RF receiving
static void dmr_disarm(void)
{
	dmr_keyed = false;
#if CONFIG_DMR
	if (hrc_rfburst_armed()) {
		hrc_rfburst_disarm();
	}
#endif
}

#if CONFIG_APRS
static bool afsk_tx_route(bool on);
static void aprs_listen_route(bool on);

// PCM amplitude per AFSK level step through the mixer route: 68 gave 4.62 kHz peak at level 50 and 25 kHz, linear in
// the level, the 2200 Hz tone 5.1 dB above the 1200 Hz one (pre-emphasis) (BENCH 2026-10-16). 46 aims at 3.1 kHz
static const hrc_afsk_board_t afsk_board = {
	.tx_route = afsk_tx_route, .listen = aprs_listen_route, .amp_per_level = 46,
};
#endif

// ---- hal_radio_t ----

static void init(void)
{
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	// FM TX limiter (modem-hrc7000.md 8.4): the KP 4 held speech and APRS to 1.65 kHz at 25 kHz here. 0x0F, the chip's
	// reset value, peaked at 4.8 kHz; 5 at 12.5 kHz scales to about 2.4 kHz (limiter x coefficient, not measured)
	modem.fm_tx_limit[0] = FM_TX_LIMIT_12K5;
	modem.fm_tx_limit[1] = FM_TX_LIMIT_25K;
	modem.rf_tx_lead = RF_TX_LEAD;
	modem.tx_pre_on = TX_PRE_ON;
	hrc_modem_analog_init(DAC_C_INIT);
	hrc7000_power_on_init(&modem, bf_cal_tx_center(), MIC_GAIN_DMR_DB);
	hrc7000_set_rf_tx_lead(&modem, RF_TX_LEAD);
#if CONFIG_DMR
	pa_keyup_init(&keyup_cfg, KEYUP_MAX_AHEAD_US, 4095u);
#endif

	hrc_pin_out(PIN_RF_CS, true);
	hrc_pin_out(PIN_RF_CLK, false);
	rfbus_3w_attach(&rf_bus, &rf_bus_pins);
	fd6818_attach_variant(&fd, &rf_bus.bus, &fd6818_variant_dm1801pro);
	fd6818_init(&fd);
	fd6818_set_mode(&fd, FD6818_MODE_FM);
	mode = RADIO_MODE_FM;

	hrc_dmr_board_init(&modem, &dmr_board);
	hrc_audio_pcm_attach(&modem, dm1801pro_speaker_pcm);
#if CONFIG_APRS
	hrc_afsk_attach(&modem, &afsk_board);
#endif
}

static void set_mode(radio_mode_t m)
{
	if (m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR && CONFIG_DMR) {
		fd6818_set_mode(&fd, FD6818_MODE_DMR);
		hrc_dac_b_power(true);	// FM receive powers DAC B down; DMR needs it (hardware-dm32.md 3.8)
		hrc7000_dmr_enter(&modem, mod_bias(rx_hz), hrc7000_mic_db(MIC_GAIN_DMR_DB, mic_offset_db));
		mode = RADIO_MODE_DMR;
		hrc_dmr_enter();
	} else {
		dmr_disarm();
		hrc_dmr_leave();
		DAC_DATA_B = 0;
		fd6818_set_mode(&fd, FD6818_MODE_FM);
		hrc7000_fm_idle(&modem);
		mode = RADIO_MODE_FM;
	}
	have_rx = false;	// the next rx() retunes everything
}

// FM (or DMR) receive, or AM in FM mode: the same set-up, only the FD6818's AF output differs. keep (the end of a DMR
// transmission, dmr_tx_end): the burst switch already put the FD6818 in RX; on the same channel it stays as it is
static bool rx_tune(uint32_t hz, radio_bw_t bw, const subtone_t *tone, bool am, bool keep)
{
	if (!board_in_band(hz, false) || (am && mode != RADIO_MODE_FM)) {
		return false;
	}
	dmr_disarm();	// a retune ends DMR TX (the hotspot's rx() disarms too)
	bool back = keep && mode == RADIO_MODE_DMR && have_rx && hz == rx_hz && !asleep;
	DAC_DATA_B = 0;
	tx_path(hz, false);
	if (back) {
		// receiving already
	} else if (mode == RADIO_MODE_DMR) {
		fd6818_set_freq(&fd, synth_hz(hz));
		fd6818_set_rx_tone(&fd, FD6818_TONE_NONE, 0);
		fd6818_set_bandwidth(&fd, fd.variant->bw_dmr);
		fd6818_set_state(&fd, FD6818_STATE_RX);
		rx_path(hz, true);
	} else {
		fd6818_set_freq(&fd, synth_hz(hz));
#if CONFIG_AM_RX
		fd6818_set_bandwidth(&fd, am ? FD6818_BW_AM : fm_bw_reg(bw));
#else
		(void)am;
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
#endif
		fd6818_set_state(&fd, FD6818_STATE_RX);
		rx_path(hz, true);
		hrc_dac_b_power(false);
		hrc7000_fm_rx(&modem, bw == RADIO_BW_25K);
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);	// FM plays on the analog path (speaker_source)
		// The FD6818 detects the tone on its own demodulated audio: while it plays FM to the speaker (PTB15 high) the
		// HR_C7000's detector has no input (hardware-dm32.md 8.4)
		fd6818_set_rx_tone(&fd, fd_tone(tone != NULL ? tone->type : TONE_NONE), tone != NULL ? tone->value : 0);
		hrc7000_set_subtone(&modem, false, HRC7000_TONE_NONE, 0);	// the modem's tone bits off (left on by a TX)
#if CONFIG_AM_RX
		fd6818_set_am(&fd, am);	// every AM tune: full gain again for the new channel; FM: AM off
#endif
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = tone != NULL ? *tone : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	asleep = false;
	transmitting = false;
	speaker_source();
	return true;
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	return rx_tune(hz, bw, tone, false, false);
}

#if CONFIG_AM_RX
// Air band AM (docs/spec/airband-and-broadcast.md 3.3): FM receive with the FD6818's AM set-up (fd6818_set_am), played
// straight to the speaker as in FM receive; no sub-audio. 108-136 MHz comes in on the VHF receive path
static bool rx_am(uint32_t hz, radio_bw_t bw)
{
	return rx_tune(hz, bw, NULL, true, false);
}
#endif

// Back on the frequency, mode and demodulator the receiver had (after a transmission or the receiver's sleep); keep as
// in rx_tune
static void retune(bool keep)
{
	subtone_t t = rx_tone;
	(void)rx_tune(rx_hz, rx_bw, &t, fd6818_am_on(&fd), keep);
}

// 5.4 "Enter TX": RX path off, frequency, the RF state sequence, 0x50, the TX path, then the PA ramp. The radio's
// waits are an uncalibrated busy-wait count; 1 ms each is at least as long
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	if (!board_in_band(hz, true) || !bf_cal_valid()) {
		return false;
	}
	// Levels 0..2 are the factory's low, mid and high rows, as the radio's own power setting picks them
	// (hardware-dm1801pro.md 6.4)
	tx_drive = dm1801pro_pa_drive(hz, power);
	if (drive_override != 0u) {
		tx_drive = drive_override;
	}
	rx_path(hz, false);
	hal_delay_ms(1);
	write_mod_bias(hz);
	// DMR keeps listening on the RX frequency until a burst (burst_on retunes per burst); on a split channel the
	// synthesizer on the TX frequency here left the radio deaf to the repeater's downlink
	fd6818_set_freq(&fd, synth_hz(mode == RADIO_MODE_DMR && have_rx ? rx_hz : hz));
	fd6818_set_rx_tone(&fd, FD6818_TONE_NONE, 0);	// 0x51's enable would add the FD6818's tone to the HR_C7000's
	hrc_dac_b_power(true);
	if (mode == RADIO_MODE_DMR) {
		// The FD6818 and the RX path receive until our first burst and between bursts; burst_on keys each burst
		// with the TX path and the drive. A hotspot stays keyed like this while it listens,
		// so receive must work while keyed (set_freq left the FD6818 idle)
		// The microphone reaches the codec ADC with PTB14 high, the same as for FM (audio_route.h); the other HR_C7000 boards' DMR
		// level (low) cut it
		hrc_pin_write(PIN_MIC_PATH, dm1801pro_audio_route(false).mic_path);
		hrc7000_set_mic_gain(&modem, hrc7000_mic_db(MIC_GAIN_DMR_DB, mic_offset_db));
		fd6818_set_bandwidth(&fd, fd.variant->bw_dmr);
		fd6818_set_state(&fd, FD6818_STATE_RX);
		hal_delay_ms(1);
		tx_hz = hz;	// for burst_on's TX path
		rx_path(rx_hz, true);
		// The burst's key-up points from the cal table at the TX frequency
		pa_keyup_cfg_t c = keyup_cfg;	// the plan and its points from one set of constants
		uint16_t top = dm1801pro_pa_drive(hz, 0xFFu);
#if CONFIG_DMR
		ku = pa_keyup_plan(tx_drive, top, 0u, &c);
#else
		ku = pa_keyup_drives(&c, tx_drive, top, 0u, 4095u);
#endif
		key_n = pa_keyup_points(&c, &ku, key_pt);
		dmr_keyed = true;
#if CONFIG_DMR
		hrc_rfburst_arm();
#endif
	} else {
		fd6818_fm_tx_prepare(&fd);
		// The radio's FM key-up writes 0x40 from the band's three calibration bytes; the next rx() puts the power-on
		// word back (fd6818_set_freq)
		fd6818_write(&fd, FD6818_REG_BAND_B, (uint16_t)(FD_REG40_BASE + bf_cal_tx_byte(hz, BF_ROW_REG40)));
		tx_hz = hz;
		tx_wide = bw == RADIO_BW_25K;
		hrc7000_set_subtone(&modem, true, c7_tone(tone != NULL ? tone->type : TONE_NONE),
			tone != NULL ? tone->value : 0);	// before FM_PTT: the modem takes the DCS code then
		hrc7000_set_mic_gain(&modem, hrc7000_mic_db(tx_wide ? MIC_GAIN_FM_25K_DB : MIC_GAIN_FM_12K5_DB,
			mic_offset_db));
		hrc7000_fm_tx(&modem, tx_wide);
		hrc_pin_write(PIN_MIC_PATH, dm1801pro_audio_route(false).mic_path);
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
		fd6818_set_state(&fd, FD6818_STATE_TX);
		fd6818_write(&fd, FD_REG_TX_AUX, 0x3B18);
		hal_delay_ms(1);
		tx_path(hz, true);
		pa_ramp(tx_drive, true);
	}
	transmitting = true;
	uint32_t irq = hrc_irq_save();
	ring_play_update();
	hrc_irq_restore(irq);
	return true;
}

// 5.4 "Leave TX": PA ramp down, TX path off, then the receive set-up again (keep: as in rx_tune)
static void tx_end(bool keep)
{
	if (!transmitting) {
		return;
	}
	dmr_disarm();
#if CONFIG_APRS
	if (afsk_tx) {
		hrc_afsk.tx_stop();	// the core stops the tones first; this is the safety net
	}
#endif
	if (mode == RADIO_MODE_FM) {
		pa_ramp(tx_drive, false);
	}
	DAC_DATA_B = 0;
	hal_delay_ms(1);
	if (have_rx) {
		hrc_pin_write(PIN_TX_VHF, false);
		hrc_pin_write(PIN_TX_UHF, false);
		if (mode == RADIO_MODE_FM) {
			hrc7000_fm_idle(&modem);
		}
		retune(keep);
	}
	transmitting = false;
	uint32_t irq = hrc_irq_save();
	ring_play_update();
	hrc_irq_restore(irq);
}

static void tx_off(void)
{
	tx_end(false);
	drive_override = 0u;
}

static uint16_t drive_level(uint32_t hz, uint8_t level)
{
	return board_in_band(hz, true) ? (bf_cal_valid() ? dm1801pro_pa_drive(hz, level) : 0u) : 0u;
}

static bool drive_set(uint16_t drive)
{
	if (drive > 4095u) {	// DAC B is 12 bits
		return false;
	}
	drive_override = drive;
	return true;
}

// hrc_dmr_board_t.tx_end: the burst switch left the FD6818 receiving
static void dmr_tx_end(void)
{
	tx_end(true);
}

// FM TX tone (1750 Hz burst, roger beep, Morse): the modem's DTMF sender as a single tone (modem-hrc7000.md 8.3).
// hrc7000_fm_idle() at the end of the transmission gives the microphone back
// The DCS turn-off tail, from the modem (modem-hrc7000.md 8.2); the core holds the carrier for it
static bool tx_tail(bool on)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	return hrc7000_tx_tail(&modem, on);
}

static bool tx_tone(uint16_t hz)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	hrc7000_tx_tone(&modem, hz, hrc7000_dtmf_dev(tx_hz, tx_wide));
	return true;
}

static bool tx_tone_key(bool down)
{
	if (!transmitting || mode != RADIO_MODE_FM || !modem.tone_on) {
		return false;
	}
	hrc7000_tx_tone_key(&modem, down);
	return true;
}

#if CONFIG_DTMF
// DTMF (radio-features.md 3.7): the same sender with the digit's two tones at the tone deviation
static bool tx_dtmf(char digit)
{
	if (!transmitting || mode != RADIO_MODE_FM) {
		return false;
	}
	return hrc7000_tx_dtmf(&modem, digit, hrc7000_dtmf_dev(tx_hz, tx_wide));
}
#endif

static void status(radio_status_t *st)
{
	uint16_t r67 = fd6818_read(&fd, FD_REG_RSSI);
	uint16_t r65 = fd6818_read(&fd, FD_REG_NOISE);
	last_rssi_raw = fd6818_rssi_raw(r67);
	last_noise_raw = r65 & 0xFFu;
	st->rssi_dbm = (int16_t)(fd6818_rssi_dbm(r67) + fd6818_rssi_cal_db(rssi_cal, sizeof(rssi_cal) / sizeof(rssi_cal[0]),
									    rx_hz));
	st->noise = fd6818_noise(r65);	// the chip's floor lifted above the core's squelch levels (fd6818.h)
#if CONFIG_AM_RX
	(void)fd6818_am_status(&fd, r65, r67, &st->rssi_dbm, &st->noise);	// AM: its own level and squelch value
#endif
	st->carrier = false;
	st->tone_ok = mode != RADIO_MODE_FM || fd6818_rx_tone_ok(&fd, fd_tone(rx_tone.type));
}

// Receiver battery saving (FM receive only): the FD6818 idle and the RX path off; waking tunes again with rx()
static void rx_sleep(bool sleep)
{
	if (mode != RADIO_MODE_FM || transmitting || !have_rx || sleep == asleep) {
		return;
	}
	if (sleep) {
		fd6818_set_state(&fd, FD6818_STATE_IDLE);
		rx_path(rx_hz, false);
		asleep = true;
	} else {
		retune(false);
	}
}

// The speaker hears one of two sources (PTB15, checked on the radio): 1 = the FD6818's audio output directly (FM
// receive), 0 = the HR_C7000's codec output (beeps, DMR voice). PTB14 is always its complement, as the radio's own
// firmware drives it (audio_route.h), during a transmission too.
//
// While APRS listens the HR_C7000 must hear the FD6818 all the time (PTB15 = 1 takes the FD6818's audio off its
// input), so PTB15 stays 0 and an open squelch plays FM through the HR_C7000's own FM playback and the codec instead
static void speaker_source(void)
{
	// The FD6818's output is squelched noise while the squelch is closed: switching the speaker to it then let a burst
	// of static through at the end of every beep, even with the amplifier turned off first
	bool fm_open = mode == RADIO_MODE_FM && !transmitting && !pcm_on && rx_audio;
	bool analog = fm_open && !aprs_listen;
	dm1801pro_audio_route_t r = dm1801pro_audio_route(analog);
	hrc_pin_write(PIN_SPK_SOURCE, r.spk_source);
	hrc_pin_write(PIN_MIC_PATH, r.mic_path);
	if (mode == RADIO_MODE_FM && !transmitting) {
		bool play = fm_open && aprs_listen;
		uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
		hrc7000_audio_bits(&modem, play ? C7_AUDIO_FM_PLAY : 0, play ? 0 : C7_AUDIO_FM_PLAY);
		hrc_irq_restore(s);
	}
}

#if CONFIG_APRS
// APRS listens (hrc_afsk.c): the FD6818's audio stays on the HR_C7000's input, which also plays it to the speaker
static void aprs_listen_route(bool on)
{
	uint32_t s = hrc_irq_save();	// dm1801pro_speaker_pcm runs speaker_source from the audio task
	aprs_listen = on;
	if (rx_audio) {
		dm1801pro_amp(DM1801PRO_AMP_RADIO, false);	// the source changes under an open squelch: no click
	}
	speaker_source();
	if (rx_audio) {
		dm1801pro_amp(DM1801PRO_AMP_RADIO, true);
	}
	hrc_irq_restore(s);
}
#endif

void dm1801pro_amp(uint8_t who, bool on)
{
	static uint8_t users;
	uint32_t s = hrc_irq_save();
	users = on ? (uint8_t)(users | who) : (uint8_t)(users & ~who);
	hrc_pin_write(PIN_AMP_EN, users != 0);
	hrc_irq_restore(s);
}

static void mic_gain(int8_t db)
{
	mic_offset_db = db;
}

static void audio(bool on)
{
	rx_audio = on;
	if (!on) {
		dm1801pro_amp(DM1801PRO_AMP_RADIO, false);	// before the source switch, as for the speaker PCM
	}
	speaker_source();
	if (on) {
		dm1801pro_amp(DM1801PRO_AMP_RADIO, true);
	}
}

// The prompt FIFO's playback (ring play) also feeds the TX modulator: a talk permit tone played at key-up rode on the
// voice LC headers, and the bench modem then joined by late entry (bench 2026-10-15, modem-hrc7000.md 8.4).
// In DMR the gate in audio_pcm.c holds it off while the engine transmits (armed between bursts the radio receives);
// in FM TX the microphone has the modulator. The caller holds the AUDIO_CONTROL lock (interrupts off)
static void ring_play_update(void)
{
	bool play = (pcm_on && !(transmitting && mode != RADIO_MODE_DMR)) || afsk_tx;	// APRS tones: on purpose
	hrc_audio_pcm_ring(&modem, play);
}

// Speaker PCM (a beep, DMR voice): the codec plays the CPU's samples from the prompt FIFO, the speaker listens to the
// codec, and the
// amplifier stays on for it whatever the squelch does. The modem's own FM playback is never used on this radio
void dm1801pro_speaker_pcm(bool on)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	if (on != pcm_on) {
		pcm_on = on;
		if (on) {
			hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);
		}
		ring_play_update();
		// Off: the amplifier first. Switching the source back while it still plays let a burst of the FD6818's
		// squelched noise through at the end of every beep
		if (!on) {
			dm1801pro_amp(DM1801PRO_AMP_PCM, false);
		}
		speaker_source();
		if (on) {
			dm1801pro_amp(DM1801PRO_AMP_PCM, true);
		}
	}
	hrc_irq_restore(s);
}

#if CONFIG_APRS
// APRS AFSK (hrc_afsk.c): during an FM transmission the tones play through the prompt FIFO (ring play), and the codec
// mixer puts the DAC's audio on the ADC's output to the modulator with the microphone's input zeroed. Ring play alone
// (with the mic muted) sent no tones here, unlike the other HR_C7000 boards (hardware-dm1801pro.md 10). False when the radio isn't
// transmitting FM
static bool afsk_tx_route(bool on)
{
	if (on && (!transmitting || mode != RADIO_MODE_FM)) {
		return false;
	}
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	afsk_tx = on;
	hrc7000_codec_dac_to_tx(&modem, on);
	ring_play_update();
	hrc_irq_restore(s);
	return true;
}
#endif

void dm1801pro_radio_diag(dm1801pro_radio_diag_t *d)
{
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->fm_noise = hrc7000_fm_noise(&modem);
	d->rf_writes = rf_bus.bus.writes;
	d->c7_audio = hrc7000_rd(&modem, C7_AUDIO_CONTROL);
	d->c7_rf_mode = hrc7000_rd(&modem, C7_RF_MODE);
	d->c7_work_mode = (uint8_t)hrc7000_rd(&modem, C7_WORK_MODE);
	d->c7_fm_ptt = (uint8_t)(hrc7000_rd(&modem, C7_FM_PTT) & 1u);
	d->pa_hz = tx_hz != 0 ? tx_hz : rx_hz;
	d->pa_drive = tx_drive;
}

uint8_t dm1801pro_codec_byte(uint8_t reg)
{
	return hrc7000_codec_rd(&modem, reg);
}

const hal_radio_t dm1801pro_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.dmr_tx_end = hrc_dmr_tx_end,
	.tx_tone = tx_tone,
	.tx_tail = tx_tail,
	.tx_tone_key = tx_tone_key,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
	.status = status,
	.audio = audio,
	.volume = NULL,
	.rx_sleep = rx_sleep,
#if CONFIG_AM_RX
	.rx_am = rx_am,
#endif
	.mic_gain = mic_gain,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
	.tx_drive_level = drive_level,
	.tx_drive = drive_set,
};

// The shared PTT line's gate trusts no new press while this is set (drivers/input/sharedptt)
bool dm1801pro_radio_transmitting(void)
{
	return transmitting;
}
