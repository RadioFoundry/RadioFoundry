// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro radio: FD6818 RF transceiver (bit-banged, its own register set), the HR_C7000 baseband for analog FM,
// separate RX and TX band paths and the PA drive on DAC channel B (docs/spec/hardware-dm1801pro.md 5; modem-hrc7000.md
// 4, 5.1, 8) for analog FM and DMR. The DMR slot engine, PA one-shot and PCM audio are the shared HR_C7000 glue
// (platform/hrc7000/dmr_glue.c, audio_pcm.c); this file supplies the per-burst RF switch and the PA key.
//
// Transmit needs the factory calibration (the PA drive comes from it): without it tx() refuses.
#include "dm1801pro.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "cal/baofeng/baofeng_cal.h"
#include "rf/fd6818/fd6818.h"
#include "modem/hrc7000/hrc7000.h"
#include "dmr_glue.h"
#include "audio_pcm.h"

#define DAC_C_INIT		0x06E2u		// the DM-32UV's value (modem-hrc7000.md 4); the DM-1801 Pro's is 0x067E: unverified

#define ADC_CTRL		REG32(ADC_BASE + 0x00)
#define ADC_CTRL_RESET		(1u << 3)

#define UHF_FROM_HZ		200000000u	// VHF / UHF split of the path selects (5.2)
#define MIC_GAIN_DB		11u		// GCR_ADCL at init (the DM-32UV's, unverified)
#define RF_BIT_SCALE_US		5u		// the radio clocks the FD6818 at about 5 us per half period (5.1)
#define PA_RAMP_STEPS		40u		// 5.4
#define PA_RAMP_STEP_US		50u
#define FD_REG_TX_AUX		0x50u		// 3B18 after the TX state, before the TX path comes on (5.4)
#define DMR_PA_DELAY_US		28000u		// the DM-32UV's "DMR PA" default; the radio's own isn't known (5.4)
#define FD_REG_NOISE		0x65u
#define FD_REG_RSSI		0x67u

static fd6818_t fd;
static hrc7000_t modem;

static radio_mode_t mode = RADIO_MODE_FM;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static bool transmitting;
static uint32_t tx_hz;		// FM TX frequency and bandwidth, for the TX tone deviation
static bool tx_wide;
static bool pcm_on;		// a beep or DMR voice plays through the codec
static bool rx_audio;		// the squelch is open (the radio's audio hook)

static void speaker_source(void);
static volatile bool dmr_keyed;		// DMR transmission on: the PA timer may key the PA
static volatile uint16_t tx_drive;	// DAC B value at the top of the ramp (FM) or while keyed (DMR)
static uint16_t last_rssi_raw, last_noise_raw;

// ---- FD6818 bus ----

static void rf_cs(bool level) { hrc_pin_write(PIN_RF_CS, level); }
static void rf_clk(bool level) { hrc_pin_write(PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(PIN_RF_DATA); }
static void rf_delay_us(uint32_t us) { hal_delay_us(us * RF_BIT_SCALE_US); }

static const fd6818_bus_t rf_bus = {
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

// A path select drives its VHF-side and UHF-side pins as a complementary pair; both low = off (5.5). Which side is
// VHF is inferred and unverified
static void path_pins(uint8_t vhf_pin, uint8_t uhf_pin, uint32_t hz, bool on)
{
	bool uhf = hz > UHF_FROM_HZ;
	hrc_pin_write(vhf_pin, on && !uhf);
	hrc_pin_write(uhf_pin, on && uhf);
}

static void rx_path(uint32_t hz, bool on) { path_pins(PIN_RX_VHF, PIN_RX_UHF, hz, on); }
static void tx_path(uint32_t hz, bool on) { path_pins(PIN_TX_VHF, PIN_TX_UHF, hz, on); }

static void dac_b_power(bool on)
{
	uint32_t s = hrc_irq_save();
	if (on) {
		DAC_PD_CTRL &= ~DAC_B_OFF;
	} else {
		DAC_PD_CTRL |= DAC_B_OFF;
	}
	hrc_irq_restore(s);
}

// The PA drive as a ramp, so the carrier doesn't splatter (5.4: 40 steps)
static void pa_ramp(uint16_t from, uint16_t to)
{
	for (uint32_t i = 1; i <= PA_RAMP_STEPS; i++) {
		int32_t v = (int32_t)from + ((int32_t)to - (int32_t)from) * (int32_t)i / (int32_t)PA_RAMP_STEPS;
		DAC_DATA_B = (uint32_t)v;
		hal_delay_us(PA_RAMP_STEP_US);
	}
}

static bool in_band(uint32_t hz, bool tx)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (hz >= b->lo_hz && hz <= b->hi_hz) {
			return !tx || b->tx;
		}
	}
	return false;
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

static void write_mod_bias(uint32_t hz)
{
	hrc7000_wr(&modem, C7_RF_MOD_BIAS, hrc7000_mod_bias(bf_cal_tx_center(), bf_cal_modcal(hz)));
}

// ---- From the DMR interrupts ----

// RF TX interrupt (tx): FD6818 idle then TX for our burst, then the TX aux word. After the burst: PA drive off, FD6818
// back to receive. The band path pins stay as tx() / rx() set them for the whole transmission
static void dmr_rf_switch(bool tx)
{
	if (!tx) {
		DAC_DATA_B = 0;
	}
	fd6818_dmr_burst(&fd, tx, fd.variant->bw_dmr);
	if (tx) {
		fd6818_write(&fd, FD_REG_TX_AUX, 0x3B18);
	}
}

// The PA one-shot expired: key the drive if the transmission is still on (one write: a burst is 27.5 ms long, there is
// no time for the 2 ms ramp of the analog path)
static void dmr_pa_on(void)
{
	if (dmr_keyed) {
		DAC_DATA_B = tx_drive;
	}
}

static const hrc_dmr_board_t dmr_board = {
	.rf_switch = dmr_rf_switch, .pa_on = dmr_pa_on, .pa_delay_us = DMR_PA_DELAY_US,
};

// ---- hal_radio_t ----

static void init(void)
{
	hrc7000_attach(&modem, (volatile uint32_t *)HRC7000_REG_BASE, (volatile uint8_t *)HRC7000_BUF_BASE);
	// modem-hrc7000.md 4 steps 1-3: ADC reset and idle, DAC B to its pin, DACs B and C on with the PA drive at 0
	ADC_CTRL = ADC_CTRL_RESET;
	hal_delay_us(2);
	ADC_CTRL = 0;
	uint32_t s = hrc_irq_save();
	IO_DIPLEX2 &= ~IO_DIPLEX2_DAC_B;
	hrc_irq_restore(s);
	DAC_PD_CTRL = 0x01u;
	DAC_PD_MODE_EN = 0x01u;
	DAC_DATA_A = 0;
	DAC_DATA_B = 0;
	DAC_DATA_C = DAC_C_INIT;
	hrc7000_power_on_init(&modem, bf_cal_tx_center(), MIC_GAIN_DB);

	hrc_pin_out(PIN_RF_CS, true);
	hrc_pin_out(PIN_RF_CLK, false);
	fd6818_attach_variant(&fd, &rf_bus, &fd6818_variant_dm1801pro);
	fd6818_init(&fd);
	fd6818_set_mode(&fd, FD6818_MODE_FM);
	mode = RADIO_MODE_FM;

	hrc_dmr_board_init(&modem, &dmr_board);
	hrc_audio_pcm_attach(&modem, dm1801pro_speaker_pcm);
}

static void set_mode(radio_mode_t m)
{
	if (m == mode) {
		return;
	}
	if (m == RADIO_MODE_DMR && CONFIG_DMR) {
		fd6818_set_mode(&fd, FD6818_MODE_DMR);
		dac_b_power(true);	// FM receive powers DAC B down; DMR needs it (hardware-dm32.md 3.8)
		hrc7000_dmr_enter(&modem, hrc7000_mod_bias(bf_cal_tx_center(), bf_cal_modcal(rx_hz)), MIC_GAIN_DB);
		mode = RADIO_MODE_DMR;
		hrc_dmr_enter();
	} else {
		hrc_dmr_leave();
		dmr_keyed = false;
		DAC_DATA_B = 0;
		fd6818_set_mode(&fd, FD6818_MODE_FM);
		hrc7000_fm_idle(&modem);
		mode = RADIO_MODE_FM;
	}
	have_rx = false;	// the next rx() retunes everything
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	if (!in_band(hz, false)) {
		return false;
	}
	DAC_DATA_B = 0;
	tx_path(hz, false);
	fd6818_set_freq(&fd, hz);
	if (mode == RADIO_MODE_DMR) {
		fd6818_set_bandwidth(&fd, fd.variant->bw_dmr);
		fd6818_set_state(&fd, FD6818_STATE_RX);
		rx_path(hz, true);
	} else {
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
		fd6818_set_state(&fd, FD6818_STATE_RX);
		rx_path(hz, true);
		dac_b_power(false);
		hrc7000_fm_rx(&modem, bw == RADIO_BW_25K);
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);	// FM plays on the analog path (speaker_source)
		hrc7000_set_subtone(&modem, false, c7_tone(tone != NULL ? tone->type : TONE_NONE),
			tone != NULL ? tone->value : 0);
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = tone != NULL ? *tone : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	transmitting = false;
	speaker_source();
	return true;
}

// 5.4 "Enter TX": RX path off, frequency, the RF state sequence, 0x50, the TX path, then the PA ramp. The radio's
// waits are an uncalibrated busy-wait count; 1 ms each is at least as long
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	if (!in_band(hz, true) || !bf_cal_valid()) {
		return false;
	}
	// Levels 0..2 are the factory's low, mid and high points (hardware-dm1801pro.md 6.4); which menu level picks which
	// is unverified
	static const uint8_t level_of[3] = { 1, 4, 8 };
	tx_drive = bf_cal_power_level(hz, level_of[power > 2 ? 2 : power]);
	rx_path(hz, false);
	hal_delay_ms(1);
	write_mod_bias(hz);
	fd6818_set_freq(&fd, hz);
	dac_b_power(true);
	if (mode == RADIO_MODE_DMR) {
		// The TX path comes on for the whole transmission; the drive waits for the PA one-shot of each burst
		hrc_pin_write(PIN_MIC_PATH, false);
		fd6818_set_bandwidth(&fd, fd.variant->bw_dmr);
		fd6818_set_state(&fd, FD6818_STATE_TX);
		fd6818_write(&fd, FD_REG_TX_AUX, 0x3B18);
		hal_delay_ms(1);
		tx_path(hz, true);
		dmr_keyed = true;
	} else {
		fd6818_fm_tx_prepare(&fd);
		tx_hz = hz;
		tx_wide = bw == RADIO_BW_25K;
		hrc7000_fm_tx(&modem, tx_wide);
		hrc7000_set_subtone(&modem, true, c7_tone(tone != NULL ? tone->type : TONE_NONE),
			tone != NULL ? tone->value : 0);
		hrc_pin_write(PIN_MIC_PATH, true);
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
		fd6818_set_state(&fd, FD6818_STATE_TX);
		fd6818_write(&fd, FD_REG_TX_AUX, 0x3B18);
		hal_delay_ms(1);
		tx_path(hz, true);
		pa_ramp(0, tx_drive);
	}
	transmitting = true;
	return true;
}

// 5.4 "Leave TX": PA ramp down, TX path off, then the receive set-up again
static void tx_off(void)
{
	if (!transmitting) {
		return;
	}
	dmr_keyed = false;
	if (mode == RADIO_MODE_FM) {
		pa_ramp(tx_drive, 0);
	}
	DAC_DATA_B = 0;
	hal_delay_ms(1);
	if (have_rx) {
		hrc_pin_write(PIN_TX_VHF, false);
		hrc_pin_write(PIN_TX_UHF, false);
		if (mode == RADIO_MODE_FM) {
			hrc7000_fm_idle(&modem);
		}
		subtone_t t = rx_tone;
		rx(rx_hz, rx_bw, &t);
	}
	transmitting = false;
}

// FM TX tone (1750 Hz burst, roger beep, Morse): the modem's DTMF sender as a single tone (modem-hrc7000.md 8.3).
// hrc7000_fm_idle() at the end of the transmission gives the microphone back
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

static void status(radio_status_t *st)
{
	uint16_t r67 = fd6818_read(&fd, FD_REG_RSSI);
	uint16_t r65 = fd6818_read(&fd, FD_REG_NOISE);
	last_rssi_raw = fd6818_rssi_raw(r67);
	last_noise_raw = r65 & 0xFFu;
	st->rssi_dbm = fd6818_rssi_dbm(r67);
	st->noise = (uint8_t)last_noise_raw;
	st->carrier = false;
	st->tone_ok = mode != RADIO_MODE_FM || hrc7000_subtone_ok(&modem);
}

// The speaker hears one of two sources (PTB15, checked on the radio): 1 = the FD6818's audio output directly (FM
// receive), 0 = the HR_C7000's codec output (beeps, DMR voice). PTB14 is driven as its complement, as the radio's own
// firmware does; during a transmission the TX path owns PTB14 (the microphone route)
static void speaker_source(void)
{
	// The FD6818's output is squelched noise while the squelch is closed: switching the speaker to it then let a burst
	// of static through at the end of every beep, even with the amplifier turned off first
	bool analog = mode == RADIO_MODE_FM && !transmitting && !pcm_on && rx_audio;
	hrc_pin_write(PIN_SPK_SOURCE, analog);
	if (!transmitting) {
		hrc_pin_write(PIN_MIC_PATH, !analog);
	}
}

void dm1801pro_amp(uint8_t who, bool on)
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
		dm1801pro_amp(DM1801PRO_AMP_RADIO, false);	// before the source switch, as for the speaker PCM
	}
	speaker_source();
	if (on) {
		dm1801pro_amp(DM1801PRO_AMP_RADIO, true);
	}
}

// Speaker PCM (a beep, DMR voice): the codec plays the CPU's samples from the prompt FIFO, the speaker listens to the
// codec, and the
// amplifier stays on for it whatever the squelch does. The modem's own FM playback is never used on this radio
void dm1801pro_speaker_pcm(bool on)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	if (on != pcm_on) {
		pcm_on = on;
		hrc7000_audio_bits(&modem, on ? C7_AUDIO_RING_PLAY : 0, on ? C7_AUDIO_FM_PLAY : C7_AUDIO_RING_PLAY);
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

void dm1801pro_radio_diag(dm1801pro_radio_diag_t *d)
{
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->fm_noise = hrc7000_fm_noise(&modem);
	d->rf_writes = fd.writes;
}

const hal_radio_t dm1801pro_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.tx_tone = tx_tone,
	.tx_tone_key = tx_tone_key,
	.status = status,
	.audio = audio,
	.volume = NULL,
};
