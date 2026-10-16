// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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

#define DAC_C_INIT		0x06E2u

#define ADC_CTRL		REG32(ADC_BASE + 0x00)
#define ADC_CTRL_RESET		(1u << 3)

#define UHF_FROM_HZ		280000000u	// PA / LNA band split (8.5)
#define MIC_GAIN_DB		11u		// GCR_ADCL at init (9.1)
#define DMR_PA_DELAY_US		28000u		// "DMR PA" default (8.6)
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
static volatile bool dmr_keyed;		// DMR transmission on: the PA timer may key the PA
static volatile bool tx_uhf;
static volatile uint16_t tx_drive;	// DAC B value for the transmission
static uint16_t last_rssi_raw, last_noise_raw;
static bool pcm_on;		// a beep or DMR voice plays through the codec
static bool rx_audio;		// the squelch is open (the radio's audio hook)

// The speaker amplifier is on while any user wants it
#define AMP_RADIO	0x01u	// receive audio
#define AMP_PCM		0x02u	// speaker PCM

static void speaker_source(void);

// ---- FD6818 bus ----

static void fd_cs(bool level) { hrc_pin_write(PIN_FD_CS, level); }
static void fd_clk(bool level) { hrc_pin_write(PIN_FD_CLK, level); }
static void fd_data_out(bool level) { hrc_pin_out(PIN_FD_DATA, level); }
static void fd_data_release(void) { hrc_pin_in(PIN_FD_DATA); }
static bool fd_data_in(void) { return hrc_pin_read(PIN_FD_DATA); }

static const fd6818_bus_t fd_bus = {
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

static void pa_key(void)
{
	hrc_pin_write(tx_uhf ? PIN_UHF_PA : PIN_VHF_PA, true);
	DAC_DATA_B = tx_drive;
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

// RF TX interrupt (tx): FD6818 idle then TX for our burst. After the burst: PA off, FD6818 back to RX
static void dmr_rf_switch(bool tx)
{
	if (!tx) {
		pa_off();
	}
	fd6818_dmr_burst(&fd, tx, FD6818_BW_DMR);
}

// The PA one-shot expired: key the PA if the transmission is still on
static void dmr_pa_on(void)
{
	if (dmr_keyed) {
		pa_key();
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
	pa_off();
	lna(0, false);
	hrc7000_power_on_init(&modem, bf_cal_tx_center(), MIC_GAIN_DB);

	hrc_pin_out(PIN_FD_CS, true);
	hrc_pin_out(PIN_FD_CLK, false);
	fd6818_attach(&fd, &fd_bus);
	fd6818_init(&fd);
	fd6818_set_mode(&fd, FD6818_MODE_FM);
	mode = RADIO_MODE_FM;

	hrc_dmr_board_init(&modem, &dmr_board);
	hrc_audio_pcm_attach(&modem, dm32_speaker_pcm);
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
		pa_off();
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
	pa_off();
	lna(hz, true);
	fd6818_set_freq(&fd, hz);
	if (mode == RADIO_MODE_DMR) {
		fd6818_set_bandwidth(&fd, FD6818_BW_DMR);
		fd6818_set_state(&fd, FD6818_STATE_RX);
	} else {
		fd6818_set_bandwidth(&fd, fm_bw_reg(bw));
		fd6818_set_state(&fd, FD6818_STATE_RX);
		dac_b_power(false);
		hrc7000_fm_rx(&modem, bw == RADIO_BW_25K);
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_FM_PLAY);	// FM plays on the analog path (speaker_source)
		// The FD6818 detects the tone on its own demodulated audio: the C7000's detector gets no audio while the
		// FD6818 plays FM to the speaker (hardware-dm32.md 8.4)
		fd6818_set_rx_tone(&fd, fd_tone(tone != NULL ? tone->type : TONE_NONE), tone != NULL ? tone->value : 0);
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = tone != NULL ? *tone : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;
	transmitting = false;
	speaker_source();
	return true;
}

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	if (!in_band(hz, true)) {
		return false;
	}
	tx_uhf = hz >= UHF_FROM_HZ;
	tx_drive = bf_cal_power_level(hz, power);
	lna(hz, false);
	write_mod_bias(hz);
	fd6818_set_freq(&fd, hz);
	dac_b_power(true);
	if (mode == RADIO_MODE_DMR) {
		// 8.5 "Enter DMR TX": the PA select and drive wait for the PA one-shot of each burst
		hrc_pin_write(PIN_MIC_PATH, false);
		fd6818_set_bandwidth(&fd, FD6818_BW_DMR);
		fd6818_set_state(&fd, FD6818_STATE_TX);
		dmr_keyed = true;
	} else {
		fd6818_set_rx_tone(&fd, FD6818_TONE_NONE, 0);	// 0x51's enable would add the FD6818's tone to the C7000's
		fd6818_fm_tx_prepare(&fd);
		tx_hz = hz;
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
	return true;
}

static void tx_off(void)
{
	dmr_keyed = false;
	pa_off();
	if (mode == RADIO_MODE_FM) {
		hrc7000_fm_idle(&modem);
	}
	transmitting = false;
	if (have_rx) {
		subtone_t t = rx_tone;
		rx(rx_hz, rx_bw, &t);
	}
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
	st->noise = fd6818_noise(r65);	// the chip's floor lifted above the core's squelch levels (fd6818.h)
	st->carrier = false;
	st->tone_ok = mode != RADIO_MODE_FM || fd6818_rx_tone_ok(&fd, fd_tone(rx_tone.type));
}

// The speaker hears one of two sources (PTB15, checked on the radio): 1 = the FD6818's audio output directly (FM
// receive), 0 = the HR_C7000's codec output (beeps, DMR voice). PTB15 = 1 also takes the FD6818's audio off the
// HR_C7000's ADC, so DMR receive finds no sync while it is high. PTB14 is driven as its complement, as on the DM-1801
// Pro; during a transmission the TX path owns PTB14 (the microphone route)
static void speaker_source(void)
{
	// The FD6818's output is squelched noise while the squelch is closed: the speaker listens to it only while it is open
	bool analog = mode == RADIO_MODE_FM && !transmitting && !pcm_on && rx_audio;
	hrc_pin_write(PIN_SPK_SOURCE, analog);
	if (!transmitting) {
		hrc_pin_write(PIN_MIC_PATH, !analog);
	}
}

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

// Speaker PCM (a beep, DMR voice): the codec plays the CPU's samples from the prompt FIFO, the speaker listens to the
// codec, and the amplifier stays on for it whatever the squelch does. The modem's own FM playback is not used
void dm32_speaker_pcm(bool on)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	if (on != pcm_on) {
		pcm_on = on;
		hrc7000_audio_bits(&modem, on ? C7_AUDIO_RING_PLAY : 0, on ? C7_AUDIO_FM_PLAY : C7_AUDIO_RING_PLAY);
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

void dm32_radio_diag(dm32_radio_diag_t *d)
{
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->fm_noise = hrc7000_fm_noise(&modem);
	d->fd_writes = fd.writes;
}

const hal_radio_t dm32_radio = {
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
