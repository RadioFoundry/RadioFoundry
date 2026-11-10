// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV radio: FD6818 RF transceiver (bit-banged), the HR_C7000 baseband for FM and DMR, LNA / PA selects and the
// PA drive on DAC channel B (docs/spec/hardware-dm32.md 3.8, 8, 9, 11.3; modem-hrc7000.md 4, 5.1, 8).
//
// The FD6818 is also driven from the DMR interrupts (the per-burst TX / RX switch), so every FD6818 frame runs with
// interrupts off (the bus's crit_enter); a frame takes about 50 us.
#include "dm32.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/fd6818/fd6818.h"
#include "modem/hrc7000/hrc7000.h"
#include "fm_rx_audio.h"
#include "hrc_afsk.h"
#include "radio/ref_trim.h"
#include "radio/pa_keyup.h"

#define DAC_C_INIT		0x06E2u

#define UHF_FROM_HZ		280000000u	// PA / LNA band split (8.5)
#define MIC_GAIN_DB		11u		// GCR_ADCL at init (9.1)
// DMR bursts on the shared RF-burst task (dmr_rfburst.h), timed from the RF TX interrupt (hardware-dm32.md 8.5). The PA
// keys up from the power cal table (radio/pa_keyup.h): a drive boost whose length follows the step, a hold, then the
// step's drive. A low step's drive alone took 3-4 ms to full power; with the boost every step is up by the first
// symbol. The PA is selected with the boost, so the FD6818 goes to TX 0.3 ms before that: 1.8 ms before the first
// symbol on a simplex channel (two state writes, 0.1 ms), after the inbound burst before ours (it ends about 2.3 ms
// before our first symbol). A split channel's retune holds the task 1.1-1.4 ms, so it runs before the boost. The lead
// leaves the task's wake (up to 0.42 ms) room before that
#define RF_TX_LEAD		26u		// 100 us steps (RF_CONTROL b13:8)
#define SWITCH_AHEAD_US		300u		// the FD6818 to TX this long before the PA
#define SWITCH_AHEAD_SPLIT_US	1450u		// the same with the synthesizer's move to the TX frequency, before the boost
#define TASK_WAKE_US		420u		// the RF task's latest wake after the interrupt
// The earliest key-up point: the split's switch still after the task's wake
#define KEYUP_MAX_AHEAD_US	(RF_TX_LEAD * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US - TASK_WAKE_US - SWITCH_AHEAD_SPLIT_US)
// The key-up constants (docs/spec/pa-keyup.md): measured on the SDR bench at UHF and VHF, every step up 0.1-0.8 ms
// before the first symbol and no more than 3 dB over (hardware-dm32.md 8.5). The knee goes with pa_us: the control
// charges less with the PA selected, so a later PA wants a smaller knee
#define KEYUP_BOOST_US		1500u
#define KEYUP_PA_US		1500u
#define KEYUP_DRIVE_US		500u
_Static_assert(KEYUP_BOOST_US <= KEYUP_MAX_AHEAD_US && KEYUP_PA_US <= KEYUP_BOOST_US && KEYUP_DRIVE_US <= KEYUP_BOOST_US,
	       "the key-up's points must come after the split's switch, in order (pa_keyup_valid)");
#if CONFIG_DMR
static const pa_keyup_cfg_t keyup_cfg = {
	.dead = 680, .knee = 1450, .slope_us = 600, .boost_pct = 120, .hold_pct = 80,
	.boost_us = KEYUP_BOOST_US, .pa_us = KEYUP_PA_US, .drive_us = KEYUP_DRIVE_US,
};
#endif
// TX data advance (hrc7000_t.tx_pre_on): the chip's 0.4 ms until the inbound burst's offset against the outbound is
// measured here. The other FD6818 board needed 1.1 ms
#define TX_PRE_ON		4u
// FM TX limiter (FM_DEV_COEF b23:16, hrc7000_t.fm_tx_limit) at 12.5 / 25 kHz: the driver's default 4 holds mic audio
// to about 1.65 kHz peak at 25 kHz (modem-hrc7000.md 8.4). The values measured on the other FD6818 board; at 12.5 kHz a loud whistle and
// voice peaked at 1.3 kHz (99.9 %) on the SDR bench, so the 12.5 kHz value may still be low. 25 kHz not measured yet
#define FM_TX_LIMIT_12K5	5u
#define FM_TX_LIMIT_25K		0x0Fu
#define FD_REG_NOISE		0x65u
#define FD_REG_RSSI		0x67u

static fd6818_t fd;
static rfbus_3w_t fd_bus;
static hrc7000_t modem;

static radio_mode_t mode = RADIO_MODE_FM;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static bool transmitting;
static uint32_t tx_hz;		// TX frequency: the FM tone deviation, the LNA band between DMR bursts
static bool tx_wide;
static volatile bool dmr_keyed;		// DMR transmission on: the burst sequence may key the PA
static volatile bool tx_uhf;
static volatile uint16_t tx_drive;	// DAC B value for the transmission
static pa_keyup_cfg_t ku_cfg;		// the transmission's DMR key-up, set before the RF task is armed
static pa_keyup_t ku;
static uint16_t last_rssi_raw, last_noise_raw;
static bool pcm_on;		// a beep or DMR voice plays through the codec
static bool rx_audio;		// the squelch is open (the radio's audio hook)
static bool asleep;		// the receiver is powered down between listen windows (rx_sleep)
static bool aprs_listen;	// APRS takes the HR_C7000's FM audio (aprs_listen_route)
static bool afsk_tx;		// APRS tones play through the prompt FIFO into the modulator (afsk_tx_route)

// The speaker amplifier is on while any user wants it
#define AMP_RADIO	0x01u	// receive audio
#define AMP_PCM		0x02u	// speaker PCM
#define AMP_BCAST	0x04u	// the broadcast receiver (dm32_bcast_audio)

static void speaker_source(void);
static void ring_play(void);
static void amp(uint8_t who, bool on);

// ---- FD6818 bus ----

static void fd_cs(bool level) { hrc_pin_write(PIN_FD_CS, level); }
static void fd_clk(bool level) { hrc_pin_write(PIN_FD_CLK, level); }
static void fd_data_out(bool level) { hrc_pin_out(PIN_FD_DATA, level); }
static void fd_data_release(void) { hrc_pin_in(PIN_FD_DATA); }
static bool fd_data_in(void) { return hrc_pin_read(PIN_FD_DATA); }

static const rfbus_3w_pins_t fd_bus_pins = {
	.cs = fd_cs,
	.clk = fd_clk,
	.data_out = fd_data_out,
	.data_release = fd_data_release,
	.data_in = fd_data_in,
	.delay_us = hal_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

// ---- PA, LNA, DAC ----

static void pa_off(void)
{
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_UHF_PA, false);
	hrc_pin_write(PIN_VHF_PA, false);
}

static void lna(uint32_t hz, bool on)
{
	bool uhf = hz >= UHF_FROM_HZ;
	hrc_pin_write(PIN_UHF_LNA, on && uhf);
	hrc_pin_write(PIN_VHF_LNA, on && !uhf);
}

static void pa_select(void)
{
	hrc_pin_write(tx_uhf ? PIN_UHF_PA : PIN_VHF_PA, true);
}

static void pa_key(void)
{
	pa_select();
	DAC_DATA_B = tx_drive;
}

static uint16_t fm_bw_reg(radio_bw_t bw)
{
	return bw == RADIO_BW_25K ? FD6818_BW_FM_25K : FD6818_BW_FM_12K5;
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

static void write_mod_bias(uint32_t hz)
{
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias(bf_cal_tx_center(), bf_cal_modcal(hz)));
}

// ---- From the DMR interrupts ----

// The reference trim (hal_radio_t.ref_trim): the FD6818 is programmed f x (1 + trim), at every tune and burst. The
// modem's DC bias stays the factory's tune
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

// The LNA off and the FD6818 idle then TX, on the TX frequency (a split channel's one synthesizer moves for the burst
// and back)
static void burst_switch(void)
{
	lna(tx_hz, false);
	fd6818_dmr_burst_at(&fd, true, FD6818_BW_DMR, synth_hz(tx_hz));
}

// One burst's TX on, from the RF task (dmr_rfburst.h): the key-up's points in time order (the boost, the PA on, the
// hold, the drive to the step's), with the switch to TX before the PA, or before them all on a split channel; every
// point from the RF TX interrupt. Without a boost the PA is keyed at its point at the step's drive
static void burst_on(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait)
{
	(void)wait;
	bool boost = dmr_keyed && ku.boost != 0u;
	bool split = tx_hz != rx_hz;
	enum { P_SWITCH, P_BOOST, P_PA, P_HOLD, P_DRIVE, P_COUNT };	// on a tie in this order
	// The switch to TX: ahead of the PA, or ahead of every point on a split channel (its retune holds the task)
	uint32_t sw = split ? SWITCH_AHEAD_SPLIT_US + (boost ? ku_cfg.boost_us : ku_cfg.pa_us) : SWITCH_AHEAD_US + ku_cfg.pa_us;
	const uint32_t ahead[P_COUNT] = { sw, ku_cfg.boost_us, ku_cfg.pa_us, ku.hold_us, ku_cfg.drive_us };
	bool done[P_COUNT] = { false, !boost, false, !boost, !boost };
	for (;;) {
		int8_t next = -1;
		for (int8_t i = 0; i < P_COUNT; i++) {
			if (!done[i] && (next < 0 || ahead[i] > ahead[next])) {
				next = i;
			}
		}
		if (next < 0) {
			break;
		}
		done[next] = true;
		at(hrc_rfburst_pa_at_us(RF_TX_LEAD, ahead[next]));
		if (next == P_SWITCH) {
			burst_switch();
			continue;
		}
		if (!dmr_keyed) {
			// Disarmed in the sequence: the drive may be at the boost, so the PA goes off now, not at burst_off
			if (boost) {
				pa_off();
			}
			continue;
		}
		switch (next) {
		case P_BOOST:
			DAC_DATA_B = ku.boost;
			break;
		case P_HOLD:
			DAC_DATA_B = ku.hold;
			break;
		case P_DRIVE:
			DAC_DATA_B = tx_drive;
			break;
		default:
			if (boost) {
				pa_select();	// the drive has its own points
			} else {
				pa_key();
			}
			break;
		}
	}
}

// The burst's TX off, after the task's hold to its last symbol: PA off, the FD6818 and the LNA back to RX, so the
// radio hears the other slot (the hotspot listens while keyed)
static void burst_off(void)
{
	pa_off();
	fd6818_dmr_burst_at(&fd, false, FD6818_BW_DMR, synth_hz(rx_hz));
	lna(rx_hz, true);
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

// PCM amplitude per AFSK level step: 50 (nominal) = 2750, about 3 kHz peak. Bench, 25 kHz: 2925 gave 3.3 kHz peak;
// 5850 and up are limited at 5.1 kHz peak, 4.5 kHz on the tones
static const hrc_afsk_board_t afsk_board = {
	.tx_route = afsk_tx_route, .listen = aprs_listen_route, .amp_per_level = 55,
};
#endif

// ---- hal_radio_t ----

static void init(void)
{
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	modem.tx_pre_on = TX_PRE_ON;
	hrc_modem_analog_init(DAC_C_INIT);
	pa_off();
	lna(0, false);
	hrc7000_power_on_init(&modem, bf_cal_tx_center(), MIC_GAIN_DB);
	modem.fm_tx_limit[0] = FM_TX_LIMIT_12K5;
	modem.fm_tx_limit[1] = FM_TX_LIMIT_25K;
	hrc7000_set_rf_tx_lead(&modem, RF_TX_LEAD);
#if CONFIG_DMR
	pa_keyup_init(&keyup_cfg, KEYUP_MAX_AHEAD_US, DAC_MAX);
#endif

	hrc_pin_out(PIN_FD_CS, true);
	hrc_pin_out(PIN_FD_CLK, false);
	rfbus_3w_attach(&fd_bus, &fd_bus_pins);
	fd6818_attach(&fd, &fd_bus.bus);
	fd6818_init(&fd);
	fd6818_set_mode(&fd, FD6818_MODE_FM);
	mode = RADIO_MODE_FM;

	hrc_dmr_board_init(&modem, &dmr_board);
	hrc_audio_pcm_attach(&modem, dm32_speaker_pcm);
	hrc_fm_rx_attach(&modem);
#if CONFIG_APRS
	hrc_afsk_attach(&modem, &afsk_board);
#endif
}

// Out of the receiver's sleep before anything else touches the FD6818: its 0x37 low-power word would stay otherwise
static void wake(void)
{
	if (asleep) {
		fd6818_rx_sleep(&fd, false, FD6818_MODE_FM);
		asleep = false;
	}
}

static void set_mode(radio_mode_t m)
{
	wake();
	if (m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR && CONFIG_DMR) {
		fd6818_set_mode(&fd, FD6818_MODE_DMR);
		hrc_dac_b_power(true);	// FM receive powers DAC B down; DMR needs it (hardware-dm32.md 3.8)
		hrc7000_dmr_enter(&modem, hrc7000_mod_bias(bf_cal_tx_center(), bf_cal_modcal(rx_hz)), MIC_GAIN_DB);
		mode = RADIO_MODE_DMR;
		hrc_dmr_enter();
	} else {
		dmr_disarm();
		hrc_dmr_leave();
		pa_off();
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
	wake();
	pa_off();
	lna(hz, true);
	if (back) {
		// receiving already
	} else if (mode == RADIO_MODE_DMR) {
		fd6818_set_freq(&fd, synth_hz(hz));
		fd6818_set_bandwidth(&fd, FD6818_BW_DMR);
		fd6818_set_state(&fd, FD6818_STATE_RX);
	} else {
		fd6818_set_freq(&fd, synth_hz(hz));
#if CONFIG_AM_RX
		fd6818_set_bandwidth(&fd, am ? FD6818_BW_AM : fm_bw_reg(bw));
#else
		(void)am;
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
#endif
		fd6818_set_state(&fd, FD6818_STATE_RX);
		hrc_dac_b_power(false);
		hrc7000_fm_rx(&modem, bw == RADIO_BW_25K);
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);	// FM plays on the analog path (speaker_source)
		// The FD6818 detects the tone on its own demodulated audio: the C7000's detector gets no audio while the
		// FD6818 plays FM to the speaker (hardware-dm32.md 8.4)
		fd6818_set_rx_tone(&fd, fd_tone(tone != NULL ? tone->type : TONE_NONE), tone != NULL ? tone->value : 0);
#if CONFIG_AM_RX
		fd6818_set_am(&fd, am);	// every AM tune: full gain again for the new channel; FM: AM off
#endif
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = tone != NULL ? *tone : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	transmitting = false;
	speaker_source();
	return true;
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	return rx_tune(hz, bw, tone, false, false);
}

#if CONFIG_AM_RX
// Air band AM (docs/spec/airband-and-broadcast.md 3.3, hardware-dm32.md 8.7): FM receive with the FD6818's AM set-up
// (its narrowest filter, the AF output on the AM demodulator, the gain loop). The speaker plays that output straight,
// as in FM receive (speaker_source); no sub-audio. The 108-136 MHz signal comes in through the VHF LNA
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
	rx_tune(rx_hz, rx_bw, &t, fd6818_am_on(&fd), keep);
}

#if CONFIG_DMR
// The key-up drives from the cal table at the TX frequency (radio/pa_keyup.h), before the RF task is armed
static void keyup_plan(uint32_t hz)
{
	uint16_t top = 0u;
	for (uint8_t l = 0u; l < board.power_count; l++) {
		uint16_t v = bf_cal_power_level(hz, l);
		top = v > top ? v : top;
	}
	ku_cfg = keyup_cfg;			// pa_keyup_plan leaves it when no key-up is set up (no boost then)
	ku = pa_keyup_plan(tx_drive, top, 0u, &ku_cfg);	// the plan and its points from one set of constants
}
#endif

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	if (!board_in_band(hz, true)) {
		return false;
	}
	wake();
	tx_uhf = hz >= UHF_FROM_HZ;
	tx_drive = bf_cal_power_level(hz, power);
#if CONFIG_DMR
	if (mode == RADIO_MODE_DMR) {
		keyup_plan(hz);
	}
#endif
	tx_hz = hz;
	write_mod_bias(hz);
	// DMR keeps listening on the RX frequency until a burst (burst_on retunes per burst); on a split channel the
	// synthesizer on the TX frequency here left the radio deaf to the repeater's downlink
	fd6818_set_freq(&fd, synth_hz(mode == RADIO_MODE_DMR && have_rx ? rx_hz : hz));
	hrc_dac_b_power(true);
	if (mode == RADIO_MODE_DMR) {
		// 8.5 "Enter DMR TX": the PA select and drive wait for each burst's sequence. Between bursts the radio
		// receives: the FD6818 in RX and the LNA on until the RF task switches to the first burst (burst_on). In TX
		// here, the hotspot, which keys to listen, heard nothing until its first burst
		hrc_pin_write(PIN_MIC_PATH, false);
		lna(have_rx ? rx_hz : hz, true);
		fd6818_set_bandwidth(&fd, FD6818_BW_DMR);
		fd6818_set_state(&fd, FD6818_STATE_RX);
		dmr_keyed = true;
#if CONFIG_DMR
		hrc_rfburst_arm();
#endif
	} else {
		lna(hz, false);
		fd6818_set_rx_tone(&fd, FD6818_TONE_NONE, 0);	// 0x51's enable would add the FD6818's tone to the C7000's
		fd6818_fm_tx_prepare(&fd);
		tx_wide = bw == RADIO_BW_25K;
		hrc7000_set_subtone(&modem, true, c7_tone(tone != NULL ? tone->type : TONE_NONE),
			tone != NULL ? tone->value : 0);	// before FM_PTT: the modem takes the DCS code then
		hrc7000_fm_tx(&modem, tx_wide);
		hrc_pin_write(PIN_MIC_PATH, true);
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
		fd6818_set_state(&fd, FD6818_STATE_TX);
		pa_key();
	}
	transmitting = true;
	ring_play();
	return true;
}

static void tx_end(bool keep)
{
	dmr_disarm();
	pa_off();
#if CONFIG_APRS
	if (afsk_tx) {
		hrc_afsk.tx_stop();	// the core stops the tones first; this is the safety net
	}
#endif
	if (mode == RADIO_MODE_FM) {
		hrc7000_fm_idle(&modem);
	}
	transmitting = false;
	ring_play();
	if (have_rx) {
		retune(keep);
	}
}

static void tx_off(void)
{
	tx_end(false);
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
	st->rssi_dbm = fd6818_rssi_dbm(r67);
	st->noise = fd6818_noise(r65);	// the chip's floor lifted above the core's squelch levels (fd6818.h)
#if CONFIG_AM_RX
	(void)fd6818_am_status(&fd, r65, r67, &st->rssi_dbm, &st->noise);	// AM: its own level and squelch value
#endif
	st->carrier = false;
	st->tone_ok = mode != RADIO_MODE_FM || fd6818_rx_tone_ok(&fd, fd_tone(rx_tone.type));
}

// The speaker hears one of two sources (PTB15, checked on the radio): 1 = the FD6818's audio output directly (FM
// receive), 0 = the HR_C7000's codec output (beeps, DMR voice). PTB15 = 1 also takes the FD6818's audio off the
// HR_C7000's ADC, so DMR receive finds no sync while it is high. PTB14 is driven as its complement, as on the other
// FD6818 board; during a transmission the TX path owns PTB14 (the microphone route).
//
// While APRS listens the HR_C7000 must hear the FD6818 all the time (its FM receive audio is what the demodulator
// gets, squelch open or not), so PTB15 stays 0 and an open squelch plays FM through the HR_C7000's own FM playback
// (AUDIO_CONTROL b6) and the codec instead
static void speaker_source(void)
{
	// The FD6818's output is squelched noise while the squelch is closed: the speaker listens to it only while it is open
	bool fm_open = mode == RADIO_MODE_FM && !transmitting && !pcm_on && rx_audio;
	bool analog = fm_open && !aprs_listen;
	hrc_pin_write(PIN_SPK_SOURCE, analog);
	if (!transmitting) {
		hrc_pin_write(PIN_MIC_PATH, !analog);
	}
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
	uint32_t s = hrc_irq_save();	// dm32_speaker_pcm runs speaker_source from the audio task
	aprs_listen = on;
	if (rx_audio) {
		amp(AMP_RADIO, false);	// the source changes under an open squelch: no click
	}
	speaker_source();
	if (rx_audio) {
		amp(AMP_RADIO, true);
	}
	hrc_irq_restore(s);
}
#endif

static void amp(uint8_t who, bool on)
{
	static uint8_t users;
	uint32_t s = hrc_irq_save();
	users = on ? (uint8_t)(users | who) : (uint8_t)(users & ~who);
	hrc_pin_write(PIN_AMP_EN, users != 0);
	hrc_irq_restore(s);
}

static void audio(bool on)
{
	rx_audio = on;
	if (!on) {
		amp(AMP_RADIO, false);	// before the source switch, so the switch doesn't click
	}
	speaker_source();
	if (on) {
		amp(AMP_RADIO, true);
	}
}

// The codec plays the speaker PCM only outside a transmission: the HR_C7000 mixes its ring playback into the TX
// modulator, so a beep during DMR TX (the talk permit tone) put audio on the 4FSK and the bursts it overlapped lost
// their sync (checked on the SDR bench: the voice LC headers sent during the tone didn't decode). In DMR the gate in
// audio_pcm.c holds it off while the engine transmits; armed between bursts, the radio receives and beeps play (the
// talk permit before the first burst). In FM TX the microphone has the modulator
static void ring_play(void)
{
	bool on = (pcm_on && !(transmitting && mode != RADIO_MODE_DMR)) || afsk_tx;
	hrc_audio_pcm_ring(&modem, on);
}

// Speaker PCM (a beep, DMR voice): the codec plays the CPU's samples from the prompt FIFO, the speaker listens to the
// codec, and the amplifier stays on for it whatever the squelch does. The modem's own FM playback is not used
void dm32_speaker_pcm(bool on)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	if (on != pcm_on) {
		pcm_on = on;
		hrc7000_audio_bits(&modem, 0, on ? C7_AUDIO_FM_PLAY : 0);
		ring_play();
		// Off: the amplifier first, so the FD6818's squelched noise doesn't come through as the source switches back
		if (!on) {
			amp(AMP_PCM, false);
		}
		speaker_source();
		if (on) {
			amp(AMP_PCM, true);
		}
	}
	hrc_irq_restore(s);
}

#if CONFIG_APRS
// APRS AFSK (hrc_afsk.c): during an FM transmission the prompt FIFO's audio goes to the modulator (the path that put
// beeps on DMR bursts, modem-hrc7000.md 8.4), so the tones go there with the microphone muted. False when the radio
// isn't transmitting FM
static bool afsk_tx_route(bool on)
{
	if (on && (!transmitting || mode != RADIO_MODE_FM)) {
		return false;
	}
	afsk_tx = on;
	hrc7000_codec_mute(&modem, on, false);
	ring_play();
	return true;
}
#endif

#if CONFIG_BCAST_FM
// The broadcast receiver's audio (hrc_bcast.h audio_path) needs the amplifier. The core mutes the channel's audio
// while it plays, so the speaker source stays on the codec, which is silent then
void dm32_bcast_audio(bool on)
{
	amp(AMP_BCAST, on);
}
#endif

void dm32_radio_diag(dm32_radio_diag_t *d)
{
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->fm_noise = hrc7000_fm_noise(&modem);
	d->fd_writes = fd_bus.bus.writes;
}

// Receiver battery saving (FM receive only, the radio's own sequence: hardware-dm32.md 8.8): FD6818 idle with its
// low-power 0x37 word and the LNA supply off; waking retunes with rx(). The HR_C7000 keeps its clocks
static void rx_sleep(bool sleep)
{
	if (mode != RADIO_MODE_FM || transmitting || !have_rx || sleep == asleep) {
		return;
	}
	if (sleep) {
		fd6818_rx_sleep(&fd, true, FD6818_MODE_FM);
		lna(rx_hz, false);
		asleep = true;
	} else {
		retune(false);
	}
}

// The debug link's CHIP_REG (board.chip_reg, hardware-dm32.md 8.1): chip 0 is the FD6818, page 0 only, registers
// 0x00-0x7F (bit 7 of the address is the read flag). Straight on the bus: the driver's copies of the state and bandwidth
// words don't follow a write, and its next set-up overwrites it. A frame is atomic (rfbus.h), so the radio task's
// frames and this one don't mix
bool dm32_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	if (chip != 0 || page != 0 || reg > 0x7Fu || (write && *val > 0xFFFFu) || fd.bus == NULL) {
		return false;	// fd.bus: not attached yet (radio init)
	}
	if (write) {
		rfbus_write(fd.bus, reg, (uint16_t)*val);
	}
	*val = rfbus_read(fd.bus, reg);
	return true;
}

const hal_radio_t dm32_radio = {
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
	.rx_sleep = rx_sleep,
#if CONFIG_AM_RX
	.rx_am = rx_am,
#endif
	.status = status,
	.audio = audio,
	.volume = NULL,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
};

// The shared PTT line's gate trusts no new press while this is set (drivers/input/sharedptt)
bool dm32_radio_transmitting(void)
{
	return transmitting;
}
