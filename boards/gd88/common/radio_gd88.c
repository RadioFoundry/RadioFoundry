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
// gd88_burst_off, on the shared RF-burst task), with the drive stepped at the key-up rule's points instead of ramped
// (radio/pa_keyup.h): A plans them and sends B their times after the TX line's edge.
#include "gd88_rf.h"
#include "config.h"
#include "hal_dtmf.h"
#include "radio/radio.h"
#include "radio/ref_trim.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/rfbus/rfbus.h"
#include "rf/bk4819/bk4819.h"
#include "rf/at1141/at1141.h"
#include "rf/superhet/superhet.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/at1141/at1141_superhet.h"
#include "modem/hrc7000/hrc7000.h"
#include "audio_pcm.h"
#include "audio/afsk_tonegen/afsk_tonegen.h"
#if CONFIG_DMR && GD88_TX && CONFIG_SFR
#include "rx2/rx2_dmr.h"
#endif
#if CONFIG_DMR && GD88_DMR_TX && GD88_TX
#include "radio/pa_keyup.h"
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
// Speaker volume steps 1..RADIO_VOLUME_MAX as one level in dB for both paths: 3 dB a step, GD88_VOL_TOP_DB at the
// top, so step 1 is 45 dB below it. Step 0 is mute. FM: the main chip's AF level (bk4819_set_af_level_db). Beeps and
// DMR voice leave the modem's line out: its DAC gain down to the DAC's floor, then a gain on the samples. The speaker
// amplifier after them is loud: 24 dB down was still loud in a room. Decoded DMR voice is close to full scale:
// measured at the speaker, its level stops following the gain above about +6 dB and the sound distorts from 0 dB up
#ifndef GD88_VOL_TOP_DB
#define GD88_VOL_TOP_DB		0
#endif
// FM's level on the same steps: the main chip's AF DAC held at GD88_FM_DAC, its AF gains at vol_db + GD88_FM_TOP_DB.
// Set so that one speech clip plays about as loud on both at every step, measured at the speaker by microphone: DMR
// decoding to -6 dBFS peaks, FM peaking at 2.5 kHz deviation on a 12.5 kHz channel. At DAC 0 and the gains' top FM was
// 19-20 dB quieter; DAC 2 adds about 15 dB. Step 1 is then the gains' floor (BK4819_AF_MIN_DB). A test signal of tones
// changing every 20 ms is no reference here: the vocoder loses about 13 dB on it, about 6 on steady tones
#ifndef GD88_FM_DAC
#define GD88_FM_DAC		2u
#endif
#ifndef GD88_FM_TOP_DB
#define GD88_FM_TOP_DB		(-3)
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
static int8_t vol_db;		// the level of the current step (vol_level_db)
static uint8_t vol_pct = 50;

static unsigned vol_step(uint8_t percent)
{
	unsigned step = (percent * RADIO_VOLUME_MAX + 50u) / 100u;
	return step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step;
}

static int vol_level_db(unsigned step)
{
	return hrc7000_volume_db(step, RADIO_VOLUME_MAX, RADIO_VOLUME_MAX, 0) + GD88_VOL_TOP_DB;
}
static bool dmr_armed;		// DMR TX armed by tx(): the bursts key the RF (dmr_gd88.c)
#if GD88_TX || GD88_DMR_TX
static uint16_t tx_off_us;
#endif
#if CONFIG_DMR && GD88_DMR_TX
#if !GD88_TX
static uint16_t tx_on_us;
#endif
static uint32_t tx_hz;
static void dmr_disarm(void);
#endif
#if GD88_TX
// The PA drive set over the debug link for the next transmission (power calibration; hal_radio_t.tx_drive), 0 = the
// level's. Any task sets it; tx_off clears it
static volatile uint16_t drive_override;
#endif
#if CONFIG_DMR && GD88_DMR_TX && GD88_TX
// The DMR key-up (radio/pa_keyup.h, docs/spec/pa-keyup.md). B selects the PA and its supply ahead of the rule's
// points, so they are drive writes only, on B: the level's drive at its lead, or a boost and then the drive. B times
// them from the TX line's edge, so the edge has its own fixed time after the RF TX interrupt: later than a late
// interrupt (a few hundred us, dmr_rfburst.c) and the task's wake, which then don't move the points. The earliest
// point sets the RF TX interrupt's lead with the page's TX-on delay (gd88_dmr_key_rf_tx_lead).
// Measured (tools/bench/pa_cal.py), [VHF, UHF]. UHF, both levels: the output is up 0.1 to 0.2 ms after its drive,
// the higher level sooner. With every drive on where the ramp began (GD88_DMR_DRIVE_US) the levels were up 0.65 and
// 0.88 ms before the first symbol; with these, 0.32 and 0.27 ms. VHF comes up 0.76 ms (the top drive) to 0.98 ms
// (the low table's) after its drive, and a boost doesn't shorten that: its window holds the slowest drive, so no
// level is boosted. With the drive at GD88_DMR_DRIVE_US the low level was up 0.18 ms after the first symbol; with
// these, 0.25 ms before it (the low level measured, the others by the fit over six drives)
#define KEY_EDGE_AT_US		300u
// The earliest point: the modem's longest lead (6.3 ms) has to hold the sequence, the page's longest TX-on delay
// and this
#define KEYUP_MAX_AHEAD_US	3000u
_Static_assert(GD88_DMR_SEQ_US + GD88_CAL_TX_DELAY_MAX_US + KEYUP_MAX_AHEAD_US <=
	       63u * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US, "the earliest key-up point fits the modem's longest lead");
_Static_assert(KEY_EDGE_AT_US + 50u + 200u < GD88_DMR_SEQ_US, "B's sequence after the edge ends before the first point");
static const pa_keyup_cfg_t keyup_cfg[2] = {
	{ .dead_pm = 383, .knee_us = 105, .slope_us = 892, .window_us = 1250, .settle_us = 160, .pa_us = GD88_DMR_DRIVE_US },
	{ .dead_pm = 0, .knee_us = 189, .slope_us = 22, .window_us = GD88_DMR_DRIVE_US, .settle_us = 160,
	  .pa_us = GD88_DMR_DRIVE_US },
};
static pa_keyup_t ku;		// the transmission's, set before the RF task is armed
static uint8_t tx_lead;		// the RF TX interrupt's lead, 100 us steps

// A point us before the first symbol as B's time after the TX line's edge (never 0: that asks for the ramp)
static uint16_t key_at(uint16_t us)
{
	uint32_t at = hrc_rfburst_pa_at_us(tx_lead, us);
	return (uint16_t)(at > KEY_EDGE_AT_US ? at - KEY_EDGE_AT_US : 1u);
}
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

// The reference trim (hal_radio_t.ref_trim): both BK4819s run from the TCXO, so the trim retunes them instead of
// moving its control voltage. Receive: the LO at f x (1 + trim) +- IF; the main chip stays on the IF. Transmit: the
// main chip at f x (1 + trim)
static volatile int16_t trim_ppb;
static int16_t tuned_ppb;	// the trim the LO was tuned with: DMR bursts use it too, so TX and RX move together

static uint32_t synth_hz(uint32_t hz)
{
	return ref_trim_tune_hz(hz, tuned_ppb);
}

#if CONFIG_REF_TRIM
// Taken at the next tune, so the RF chips' bus stays the radio task's: true asks the core for one
static bool ref_trim_set(int16_t ppb)
{
	trim_ppb = ppb;
	return true;
}
#endif

static void lo_tune(uint32_t hz)
{
	sh_lo.ops->tune(sh_lo.chip, synth_hz(hz) + lo_hz_for(hz) - hz);
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
static void write_bias(uint32_t hz)
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
#if CONFIG_DMR && GD88_DMR_TX && GD88_TX
	pa_keyup_init(&keyup_cfg[1], KEYUP_MAX_AHEAD_US, 4095u);	// DAC B's range (B's)
#endif

	// The modem block powered as the DMR glue does, so the reference trim reaches the TCXO in FM builds too
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	vol_db = (int8_t)vol_level_db(vol_step(vol_pct));
	modem.dac_gain = hrc7000_dac_code(vol_db);	// the power-on init writes it
	hrc_audio_pcm_spk_gain(hrc7000_sample_gain_q15(vol_db));
	hrc7000_power_on_init(&modem, gd88_cal_tx_center(), GD88_MIC_GAIN_DB);
	write_bias(BIAS_DEFAULT_HZ);

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
// The LO chip's GPIO, through the superhet LO ops (both chips drive it)
static void lo_gpio(uint8_t pin, bool high)
{
	superhet_lo_gpio(&sh_lo, pin, high);
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

// keep (the end of a DMR transmission, gd88_dmr_tx_end): the last burst's TX off already put the main chip on the IF
// in RX, the LO on and the RX path on (dmr_disarm() has waited for the RF task). On the same channel it stays as it is:
// set up again now, the receiver would miss a reply that comes in the next frame, 30 ms after our last burst
static bool rx_set(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone_in, bool keep)
{
	if (!supported || !board_in_band(hz, false)) {
		return false;
	}
#if CONFIG_DMR && GD88_DMR_TX
	dmr_disarm();	// a retune ends DMR TX
#endif
	bool from_tx = tx_stop();
	bool back = keep && mode == RADIO_MODE_DMR && have_rx && hz == rx_hz && !asleep && !from_tx && tuned_ppb == trim_ppb;
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = rx_tone_in != NULL ? *rx_tone_in : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	asleep = false;

	write_bias(hz);
	if (back) {
		return true;
	}
	tuned_ppb = trim_ppb;
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

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone_in)
{
	return rx_set(hz, bw, rx_tone_in, false);
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

// TX-off wait in a burst: A's TX line falls ahead of the last symbol by B's wake, so B waits none; B's own PA has no
// link to wait for, so it sits out that same lead
#if GD88_TX
#define DMR_OFF_DELAY_US	0u
#else
#define DMR_OFF_DELAY_US	GD88_DMR_OFF_AHEAD_US
#endif

static bool dmr_tx(uint32_t hz, uint8_t power)
{
	gd88_tx_plan_t plan;
	if (!supported || !board_in_band(hz, true) || !gd88_cal_tx_plan(hz, power >= 1, GD88_TX_DRIVE_PCT, &plan)) {
		return false;
	}
	if (dmr_armed) {
		hrc_rfburst_disarm();
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
#if GD88_TX
	uint16_t pa = drive_override != 0u ? drive_override : plan.pa;
	// The key-up against the high table's drive here; the low table's is the dead zone's anchor
	gd88_tx_plan_t other;
	uint16_t top = gd88_cal_tx_plan(hz, true, GD88_TX_DRIVE_PCT, &other) && other.pa > pa ? other.pa : pa;
	uint16_t ref = gd88_cal_tx_plan(hz, false, GD88_TX_DRIVE_PCT, &other) ? other.pa : 0u;
	pa_keyup_select(&keyup_cfg[hz >= UHF_FROM_HZ ? 1 : 0]);
	ku = pa_keyup_plan(pa, top, ref);
	tx_lead = gd88_dmr_key_rf_tx_lead(ku.boost != 0u ? ku.boost_us : ku.pa_us, plan.on_us);
	const rx2_tx_key_t key = {
		.boost = ku.boost,
		.boost_at_us = ku.boost != 0u ? key_at(ku.boost_us) : 0u,
		.drive_at_us = key_at(ku.boost != 0u ? ku.drive_us : ku.pa_us),
	};
	if (!gd88_pa_arm(hz, pa, plan.on_us, DMR_OFF_DELAY_US, &key)) {
		return false;
	}
#else
	if (!gd88_pa_arm(hz, plan.pa, plan.on_us, DMR_OFF_DELAY_US, NULL)) {
		return false;
	}
	tx_on_us = plan.on_us;
	uint8_t tx_lead = gd88_dmr_own_rf_tx_lead(plan.on_us);
#endif
	tx_off_us = DMR_OFF_DELAY_US;
	tx_hz = hz;
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias_dc(gd88_cal_tx_center(), gd88_cal_mod(hz)));
	hrc7000_set_rf_tx_lead(&modem, tx_lead);
	bk4819_dmr_tx_prepare(&rf_main);
	dmr_armed = true;
	gd88_dmr_tx_arm();
	return true;
}

static void dmr_disarm(void)
{
	if (dmr_armed) {
		hrc_rfburst_disarm();
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
// One burst: the TX line high at its time after the RF TX interrupt (B starts its PA sequence: the page's TX-on delay,
// PA select, 200 us, supply, then the drive at the key-up's points, counted from this edge), 50 us, main chip to f and
// the LO off, then until B's last point
void gd88_burst_on(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait)
{
	at(KEY_EDGE_AT_US);
	gd88_path_rx_front_off();
	wait(TX_RX_OFF_US);
	bk4819_burst_tune(&rf_main, synth_hz(tx_hz), BK4819_STATE_TX_MAIN);
	lo_off();
	at(hrc_rfburst_pa_at_us(tx_lead, ku.boost != 0u ? ku.drive_us : ku.pa_us));
}

// After a burst, from the task's hold (GD88_DMR_OFF_AHEAD_US before the last symbol): the TX line low (B waits the
// page's TX-off delay and ramps down), that long, 300 us, the main chip back to the IF and the LO on again, 300 us,
// the band's RX path, 200 us
void gd88_burst_off(hrc_rfburst_wait_fn wait)
{
	tx_paths_off();
	wait(tx_off_us + PA_RAMP_US + PA_SLACK_US);
	wait(TX_END_PATH_US);
	bk4819_burst_tune(&rf_main, IF_HZ, BK4819_STATE_RX);
	lo_resume();
	wait(TX_END_PATH_US);
	gd88_path_rx(rx_hz, true, true);
	wait(TX_END_RX_US);
}
#else
// B's own burst (the original firmware's path 1): its path pins to its chip in the TX band and its RX path off, 50 us,
// the chip to f and the LO off, the page's TX-on delay, then its PA: select, 200 us, supply, the ramp
void gd88_burst_on(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait)
{
	(void)at;
	gd88_path_own_tx(true, tx_hz);
	own_gpio(true, tx_hz);
	wait(TX_RX_OFF_US);
	bk4819_burst_tune(&rf_main, synth_hz(tx_hz), BK4819_STATE_TX_MAIN);
	lo_off();
	wait(tx_on_us);
	gd88_pa_own(true);
}

// After it, from the task's hold: the page's TX-off delay, the PA down, 300 us, the chip back to the IF and the LO on,
// 300 us, the path pins and the RX path back, 200 us
void gd88_burst_off(hrc_rfburst_wait_fn wait)
{
	wait(tx_off_us);
	gd88_pa_own(false);
	wait(TX_END_PATH_US);
	bk4819_burst_tune(&rf_main, IF_HZ, BK4819_STATE_RX);
	lo_resume();
	wait(TX_END_PATH_US);
	own_gpio(false, tx_hz);
	gd88_path_own_tx(false, tx_hz);
	gd88_path_rx(rx_hz, true, true);
	wait(TX_END_RX_US);
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
	if (drive_override != 0u) {
		plan.pa = drive_override;
	}
	if (!gd88_pa_arm(hz, plan.pa, plan.on_us, plan.off_us, NULL)) {
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
	bk4819_tx_tune(&rf_main, synth_hz(hz));
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

static void tx_end(bool keep)
{
#if CONFIG_DMR && GD88_DMR_TX
	dmr_disarm();	// the RF task finishes the burst it is in; rx_set() then sets the receiver up again or keeps it
#endif
	if (have_rx) {
		rx_set(rx_hz, rx_bw, &rx_tone, keep);	// stops the transmission first
	} else {
		tx_stop();
	}
}

static void tx_off(void)
{
	tx_end(false);
#if GD88_TX
	drive_override = 0u;
#endif
}

#if GD88_TX
static uint16_t tx_drive_level(uint32_t hz, uint8_t level)
{
	gd88_tx_plan_t plan;
	return supported && gd88_cal_tx_plan(hz, level >= 1, GD88_TX_DRIVE_PCT, &plan) ? plan.pa : 0u;
}

static bool tx_drive(uint16_t drive)
{
	if (drive > 4095u) {	// DAC B is 12 bits
		return false;
	}
	drive_override = drive;
	return true;
}
#endif

#if CONFIG_DMR && GD88_DMR_TX
// hrc_dmr_board_t.tx_end (dmr_gd88.c): the last burst's TX off left the receiver on
void gd88_dmr_tx_end(void)
{
	tx_end(true);
}
#endif

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
	bk4819_set_af_level_db(&rf_main, GD88_FM_DAC, vol_db + GD88_FM_TOP_DB);
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
	vol_db = (int8_t)vol_level_db(step);
	if (modem.regs != NULL) {
		hrc7000_set_dac_gain(&modem, hrc7000_dac_code(vol_db));	// the init sets it from vol_pct before then
	}
	hrc_audio_pcm_spk_gain(hrc7000_sample_gain_q15(vol_db));
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

// The shared PTT line's gate trusts no new press while this is set (drivers/input/sharedptt). DMR TX keys the RF per
// burst while armed, without setting transmitting; on B's transmitter (remote) the RF is B's, on the same cable
bool gd88_radio_transmitting(void)
{
#if CONFIG_DMR && GD88_TX && CONFIG_SFR
	if (dmr_remote) {
		return true;
	}
#endif
	return transmitting || dmr_armed;
}

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
	d->audio_open = audio_open;
	d->pcm_on = pcm_on;
	gd88_tx_plan_t plan;
	d->pa_low = GD88_TX && have_rx && gd88_cal_tx_plan(rx_hz, false, GD88_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
	d->pa_high = GD88_TX && have_rx && gd88_cal_tx_plan(rx_hz, true, GD88_TX_DRIVE_PCT, &plan) ? plan.pa : 0;
}

#if !GD88_TX
// A transmits through B (pa_gd88b.c key_on): B's main chip's GPIO 1 (0x33 bit 5) passes A's VHF signal to the PA, low
// on UHF; GPIO 0 (bit 6) stays low, A's chip and not B's (hardware-gd88.md 10). Written only when it changes, and left
// after the transmission as the original firmware leaves it. Without it a VHF key had only a leak, some 70 dB down
#define A_TX_GPIO_VHF		1u
void gd88_a_tx_band(bool uhf)
{
	bk4819_gpio(&rf_main, A_TX_GPIO_VHF, !uhf);
}
#endif

// The debug link's CHIP_REG (board.chip_reg, hardware-gd88.md 13): chip 0 is the main BK4819, chip 1 the LO when it
// is a BK4819 (an AT1141 LO isn't offered), page 0 only, registers 0x00-0x7F. Through the driver, so its copies of the
// enables, AF gain and GPIO words follow a write; its next set-up overwrites the register. A frame is atomic (rfbus.h),
// so the radio task's frames and this one don't mix
bool gd88_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	bk4819_t *dev = chip == 0 ? &rf_main : chip == 1 && !lo_is_at1141 ? &rf_lo : NULL;
	if (dev == NULL || page != 0 || reg > 0x7Fu || (write && *val > 0xFFFFu) || !supported) {
		return false;	// supported: both chips answered at the radio init
	}
	if (write) {
		bk4819_write(dev, reg, (uint16_t)*val);
	}
	*val = bk4819_read(dev, reg);
	return true;
}

const hal_radio_t gd88_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
#if GD88_TX
	.tx_drive_level = tx_drive_level,
	.tx_drive = tx_drive,
#endif
	.dmr_tx_end = hrc_dmr_tx_end,
	.status = status,
	.audio = audio,
	.volume = volume,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim_set,
#endif
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
