// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 HR_C6000: power-up, the DMR receive path, the PCM audio stream and the speaker routing
// (docs/spec/hardware-md9600.md 4, 6). The chip and its glue are the shared STM32 code: the configuration and voice
// ports (platform/stm32f4/hrc/hrc_bus.c; MOSI is PE5 and MISO PE4 here, the voice port SPI1), the slot engine and its
// three interrupt lines (dmr_glue.c, PC0-PC2), the I2S3 PCM stream (i2s_pcm.c) and the codec host (vocoder.c).
//
// Unlike on radios with an HR_C6000 in the FM path, the chip is not in the FM receive path: FM audio comes from the IF chip, and 4:0x36 (the FM
// feed-through) stays 0. The chip still runs in FM, because its codec's line out carries the beeps and prompts there.
// In DMR the band's IF reaches the chip at 450 kHz (the IF set-up is radio_md9600.c's); entering DMR and every retune
// rewrite the reference offset and the DMR modulation pair from the calibration.
//
// Transmit: the slot engine asks the board to key the RF side for each burst (rf_switch). Here that is refused unless
// the build has CONFIG_MD9600_DMR_TX and the transmit arm is on (md9600_dmr_tx_allow); the RF chain's per-burst keying
// is radio_md9600.c's (md9600_rf_dmr_key). FM transmit puts the chip in its FM TX mode (the microphone on its line
// input, two-point modulation with the band's FM pair) for the over, and back to the receive set-up after it.
//
// FM receive audio to a host (MD9600_FM_RX_TAP, hal_audio_tap.h AUDIO_TAP_FM_RX): the core expects it on the stream's
// input, which in FM is otherwise the chip's codec with the microphone off. While the stream runs in FM, the block
// callback swaps that input for the discriminator capture (disc_md9600.c) taken from 9146 Hz to 8 kHz, gated by the
// core's squelch and silent in transmit.
//
// FM transmit audio from a host (MD9600_FM_TX_INJECT, AUDIO_TAP_FM_TX): PWM on PC8 (pwm_md9600.c) reaches the chip's
// MIC_P / MIC_N inputs; inject switches those on and the microphone's LINEIN1 off for the rest of the over.
#include <stddef.h>
#include "md9600.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"
#include "modem/hrc6000/hrc6000.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "hrc/hrc_bus.h"
#include "hrc/dmr_glue.h"
#include "hrc/i2s_pcm.h"
#include "speaker_md9600.h"
#if MD9600_FM_RX_TAP
#include <string.h>
#include "audio/resample/resample.h"
#endif
#if CONFIG_DMR && CONFIG_AUDIO_PROBE
#include <math.h>
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define C6000_PWD_WAIT_MS	12	// at least 10 ms after PWD low before the first access (as the other HR_C6000 radios)
#define DMR_MIC_GAIN		7	// the other HR_C6000 radios' value (unverified here)
#define RX_GAIN_DB		2	// 4:0x37 = 0xC2 (unverified)
#define SAFE_GAIN		0x80u	// modulation gain without a calibration value
#define VOICE_SPI_PINS		(GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5)	// PB3 SCK, PB4 MISO, PB5 MOSI (AF5)

// DMR modulation pair in the calibration (hardware-md9600.md 11.2, unverified): I at 0x130 (UHF, 9 points) and 0x139
// (VHF, 5 points), Q at 0x140 and 0x149
#define CAL_DMR_I_UHF		0x130u
#define CAL_DMR_I_VHF		0x139u
#define CAL_DMR_Q_UHF		0x140u
#define CAL_DMR_Q_VHF		0x149u
// FM modulation pair: I at 0x170 / 0x179, Q at 0x180 / 0x189 (hardware-md9600.md 11.2, unverified)
#define CAL_FM_I_UHF		0x170u
#define CAL_FM_I_VHF		0x179u
#define CAL_FM_Q_UHF		0x180u
#define CAL_FM_Q_VHF		0x189u
#define FM_MIC_GAIN		7	// the other HR_C6000 radios' value (unverified here)

// The configuration port on PE2-PE5 and the voice port on SPI1 (PB3-PB5, chip select PD7): mode 2 at PCLK2 / 16 =
// 4.5 MHz, the other HR_C6000 radios' rate (unverified on this radio)
static const stm32_hrc_bus_cfg_t modem_cfg = {
	.port = GPIOE,
	.cs = PIN_MASK(PIN_C6000_CS), .clk = PIN_MASK(PIN_C6000_CLK), .mosi = PIN_MASK(PIN_C6000_MOSI),
	.miso = PIN_MASK(PIN_C6000_MISO),
	.voice_spi = SPI1, .voice_br = 3,
	.voice_cs_port = PIN_PORT(PIN_C6000_VCS), .voice_cs = PIN_MASK(PIN_C6000_VCS),
};
static hrc_bus_t modem_bus;
static hrc_t modem;
static md9600_c6000_diag_t diag;
static uint32_t tuned_hz;

static void count(bool ok)
{
	if (!ok) {
		diag.errors++;
	}
}

// --- calibration ---

// The calibration point nearest hz of a UHF / VHF row pair (hardware-md9600.md 11.3 point spacing); SAFE_GAIN when
// the byte is blank
static uint8_t cal_gain(uint16_t uhf_row, uint16_t vhf_row, uint32_t hz)
{
	uint8_t band;
	if (!md9600_band_of(hz, &band)) {
		return SAFE_GAIN;
	}
	uint32_t lo = band == MD9600_UHF ? 400000000u : 136000000u;
	uint32_t step = band == MD9600_UHF ? 10000000u : 9500000u;
	uint32_t last = band == MD9600_UHF ? 8u : 4u;
	uint32_t idx = hz <= lo ? 0u : (hz - lo + step / 2u) / step;
	if (idx > last) {
		idx = last;
	}
	uint8_t v = md9600_cal_byte((uint16_t)((band == MD9600_UHF ? uhf_row : vhf_row) + idx));
	return (v == 0x00u || v == 0xFFu) ? SAFE_GAIN : v;
}

static void ref_offset(uint32_t hz)
{
	uint8_t band = MD9600_UHF;
	int8_t off = 0;
	(void)md9600_band_of(hz, &band);
	if (!md9600_cal_ref_tune(band == MD9600_UHF, &off)) {
		off = 0;
	}
	count(hrc6000_set_ref_offset(&modem, off, false));
}

// --- speaker ---

// Two callers: the core's FM squelch (md9600_speaker_fm, radio task) and PCM (speaker_pcm, the audio task or the radio
// task). The mutex keeps their register and pin writes from interleaving
static bool fm_wanted, pcm_wanted, band_vhf, warm_wanted;
static SemaphoreHandle_t speaker_lock;

// The codec DAC to the line out (4:0xE2 bit 1); in DMR also the codec under MCU control (4:0xE0 = 0xC9) with the I2S
// mode bits of 4:0x22 kept set (the HR_C6000's PCM playback set-up on the other radios)
static void codec_out(void)
{
	uint8_t v;
	if (hrc_read(&modem, 4, 0xE2, &v) && (v & 0x02u) == 0) {
		count(hrc_write(&modem, 4, 0xE2, (uint8_t)(v | 0x02u)));
	}
	if (diag.dmr) {
		count(hrc_write(&modem, 4, 0xE0, 0xC9));
		if (hrc_read(&modem, 4, 0x22, &v) && (v & 0x06u) != 0x06u) {
			count(hrc_write(&modem, 4, 0x22, (uint8_t)(v | 0x06u)));
		}
	}
}

static void speaker_apply(void)
{
	bool locked = speaker_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (locked) {
		xSemaphoreTake(speaker_lock, portMAX_DELAY);
	}
	md9600_speaker_t s = md9600_speaker_lines(fm_wanted, pcm_wanted, diag.dmr, band_vhf, warm_wanted);
	if (s.af_mute) {
		PIN_WRITE(PIN_AF_MUTE, true);	// mute first, then switch
	}
	if (s.codec_out && diag.up) {
		codec_out();
	}
	PIN_WRITE(PIN_AF_BAND, s.af_vhf);
	PIN_WRITE(PIN_FM_MUTE, s.fm_pass);
	if (!s.af_mute) {
		PIN_WRITE(PIN_AF_MUTE, false);	// switch first, then unmute
	}
	diag.speaker = (uint8_t)((s.af_mute ? 0u : 1u) | (s.fm_pass ? 2u : 0u) | (s.codec_out ? 4u : 0u) |
				 (warm_wanted ? 8u : 0u));
	if (locked) {
		xSemaphoreGive(speaker_lock);
	}
}

void md9600_speaker_fm(bool on, bool vhf)
{
	fm_wanted = on;
	band_vhf = vhf;
	speaker_apply();
}

// --- FM transmit ---

static bool fm_tx;

// The tone generator for the 1750 Hz burst, the roger beep, DTMF and APRS (hrc6000_tx_pair)
#if MD9600_TX_SIG
static hrc6000_tx_sig_t tx_sig;
static uint8_t tx_sig_dev;	// 4:0xA0 for the band and bandwidth of the transmission
#endif

// The FM receive set-up (power-up and after an FM over): FM audio never passes through this chip, so it stays in DMR
// mode, where its codec plays the beeps
static void fm_rx_setup(void)
{
	count(hrc6000_set_mode_fm_rx(&modem));
	count(hrc6000_fm_audio(&modem, false));
	count(hrc6000_fm_rx_pcm(&modem));
}

void md9600_c6000_fm_tx(uint32_t hz, bool wide)
{
	if (!diag.up || diag.dmr) {
		return;
	}
	uint8_t band = MD9600_UHF;
	int8_t off = 0;
	(void)md9600_band_of(hz, &band);
	if (!md9600_cal_ref_tune(band == MD9600_UHF, &off)) {
		off = 0;
	}
	hrc6000_fm_tx_t cfg = {
		.uhf = band == MD9600_UHF,
		.wide = wide,
		.tone = HRC6000_TONE_NONE,
		.mod2 = cal_gain(CAL_FM_I_UHF, CAL_FM_I_VHF, hz),
		.mod1 = cal_gain(CAL_FM_Q_UHF, CAL_FM_Q_VHF, hz),
		.ref_offset = off,
		.mic_gain = FM_MIC_GAIN,
	};
	fm_tx = true;
#if MD9600_TX_SIG
	tx_sig_dev = hrc6000_fm_deviation(cfg.uhf, wide).dtmf;
#endif
	count(hrc6000_set_mode_fm_tx(&modem, &cfg));
}

#if MD9600_TX_SIG

bool md9600_c6000_tx_pair(uint16_t low_hz, uint16_t high_hz)
{
	if (!fm_tx) {
		return false;
	}
	bool ok = hrc6000_tx_pair(&modem, &tx_sig, low_hz, high_hz, tx_sig_dev);
	count(ok);
	return ok;
}

bool md9600_c6000_tx_key(bool down)
{
	return fm_tx && hrc6000_tx_sig_key(&modem, &tx_sig, down);
}

void md9600_c6000_tx_service(uint32_t now_ms)
{
	if (fm_tx) {
		(void)hrc6000_tx_sig_service(&modem, &tx_sig, now_ms);
	}
}

#if CONFIG_APRS_AFSK
// APRS: the first symbol's tone at level percent of the band's tone deviation x 2 (50 = the nominal); then
// md9600_c6000_afsk_tone from the symbol timer's interrupt
bool md9600_c6000_afsk_begin(uint16_t hz, uint8_t level)
{
	if (!fm_tx) {
		return false;
	}
	uint32_t d = ((uint32_t)tx_sig_dev * level + 25u) / 50u;
	bool ok = hrc6000_tx_pair(&modem, &tx_sig, hz, hz, (uint8_t)(d > 0xFFu ? 0xFFu : d));
	count(ok);
	return ok;
}

bool md9600_c6000_afsk_tone(uint16_t hz)
{
	return hrc6000_tx_sig_retune(&modem, &tx_sig, hz, hz);
}
#endif
#endif

#if MD9600_FM_TX_INJECT
// Host audio on PC8 in place of the microphone (hal_audio_tap_t.fm_tx_inject): full scale to the whole PWM range. At
// the FM mic gain, linear to about 950 Hz peak deviation for half scale at 1 kHz, the limiter near 1.4 kHz peak (UHF,
// 12.5 kHz; bench 2026-11-05, hardware-md9600.md 19.9). The mic gain register isn't monotonic, so it stays
#define INJECT_AMP		127
#define INJECT_NOMINAL_PEAK	26000	// about 1.5 kHz peak deviation at 1 kHz
static bool injecting;

static void inject_stop(void)
{
	if (!injecting) {
		return;
	}
	injecting = false;
	md9600_pwm_audio(NULL, 0);
	uint8_t e0;
	if (fm_tx && hrc_read(&modem, 4, 0xE0, &e0)) {
		count(hrc_write(&modem, 4, 0xE0, (uint8_t)((e0 | 0x40u) & ~0x06u)));	// the microphone again
	}
}

static bool fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
	if (pull == NULL) {
		inject_stop();
		return true;
	}
	if (!fm_tx || mode != AUDIO_TAP_VOICE) {
		return false;	// no flat path: the PWM goes through the codec's voice filter
	}
	uint8_t e0;
	if (!hrc_read(&modem, 4, 0xE0, &e0)) {
		count(false);
		return false;
	}
	md9600_pwm_audio(pull, INJECT_AMP);
	count(hrc_write(&modem, 4, 0xE0, (uint8_t)((e0 & ~0x40u) | 0x06u)));	// MIC_P / MIC_N in, LINEIN1 out
	injecting = true;
	return true;
}
#else
static void inject_stop(void)
{
}
#endif

void md9600_c6000_fm_tx_end(void)
{
	if (!fm_tx) {
		return;
	}
	inject_stop();
#if MD9600_TX_SIG
	count(hrc6000_tx_pair(&modem, &tx_sig, 0, 0, 0));
#endif
	fm_tx = false;
	fm_rx_setup();
	count(hrc6000_tones_off(&modem));
	ref_offset(tuned_hz);
	speaker_apply();
}

// --- FM receive audio on the stream ---

#if MD9600_FM_RX_TAP
// The fill the converter keeps: one block (183 inputs) plus two capture bursts of 32, about 28 ms of latency
#define FM_TAP_TARGET		256u
// The ring's 15-bit swing times this, in 1/256: the codec level of the other HR_C6000 radios (usb-audio.md 6.1). A
// 1 kHz tone at 1.5 kHz peak deviation (UHF, the SDR bench, 2026-11-06) gave 8735 rms at a gain of 4 here and 3520
// rms on the FM tap of an HR_C6000 radio with the codec path: 4 * 3520 / 8735 = 1.61
#ifndef MD9600_FM_RX_GAIN_Q8
#define MD9600_FM_RX_GAIN_Q8	413
#endif

static audio_block_cb_t stream_cb;
static void *stream_ctx;
static bool stream_on;			// radio task
static volatile bool fm_tap;		// the block callback swaps the input; set after the converter is ready
static resample_t fm_rs;
static int16_t fm_block[AUDIO_BLOCK_SAMPLES];

// Radio task: the capture runs while the stream does in FM
static void fm_tap_update(void)
{
	bool want = stream_on && !diag.dmr;
	if (want == fm_tap) {
		return;
	}
	if (want) {
		md9600_disc_start(MD9600_DISC_STREAM);
		resample_init(&fm_rs, MD9600_DISC_RATE_HZ, 8000u, md9600_disc_head());
		fm_tap = true;
	} else {
		fm_tap = false;
		md9600_disc_stop(MD9600_DISC_STREAM);
	}
}

// The audio task, every 20 ms
static void stream_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	if (fm_tap) {
		// The converter runs with the squelch shut too, so its place on the ring stays current
		resample_block(&fm_rs, md9600_disc_ring(), MD9600_DISC_RING, md9600_disc_head(), fm_block,
			       AUDIO_BLOCK_SAMPLES, FM_TAP_TARGET);
		if (!fm_wanted || fm_tx) {
			memset(fm_block, 0, sizeof(fm_block));
		} else {
			for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				int32_t v = ((int32_t)fm_block[i] * MD9600_FM_RX_GAIN_Q8) / 256;
				fm_block[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
		}
		mic = fm_block;
	}
	stream_cb(mic, spk, stream_ctx);
}
#else
static void fm_tap_update(void)
{
}
#endif

// --- DMR glue ---

#if CONFIG_MD9600_DMR_TX
static volatile bool tx_allowed;

void md9600_dmr_tx_allow(bool on)
{
	tx_allowed = on;
}
#endif

// RF_TX_INTER's advance before the slot (0x12 bits 5-0, about 100 us steps; hrc6000-registers.md): an SFR retunes the
// synthesizer from receive to transmit at that interrupt, so it needs more lead than a plain transmission (0)
void md9600_c6000_tx_lead(uint8_t steps)
{
	count(hrc_write(&modem, 4, 0x12, (uint8_t)(steps & 0x3Fu)));
}

// From the modem interrupts: never blocks
static bool rf_switch(bool tx)
{
#if CONFIG_MD9600_DMR_TX
	if (tx && !tx_allowed) {
		diag.tx_refused++;
		return false;
	}
	return md9600_rf_dmr_key(tx);
#else
	if (tx) {
		diag.tx_refused++;
		return false;
	}
	return true;	// receive is where the RF side already is
#endif
}

static const stm32_hrc_dmr_board_t dmr_board = {
	.int_port = GPIOC,
	.rf_switch = rf_switch,
#if CONFIG_MD9600_DMR_TX
	.rf_counters = md9600_rf_dmr_counters,
	.tx_prepare = md9600_rf_dmr_prepare,
	.tx_wait_lock = md9600_rf_dmr_wait,
	.tx_done = md9600_rf_dmr_done,
	.tx_end = md9600_dmr_tx_end,
#endif
};

// --- radio hooks (radio_md9600.c) ---

void md9600_c6000_init(void)
{
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
	}
	stm32_pin_af(GPIOB, VOICE_SPI_PINS, GPIO_AF5_SPI1, false, GPIO_NOPULL);
	stm32_hrc_bus_init(&modem_cfg, &modem_bus);
	PIN_WRITE(PIN_C6000_PWD, false);
	hal_delay_ms(C6000_PWD_WAIT_MS);
	hrc6000_attach(&modem, &modem_bus);
	count(hrc6000_power_on_init(&modem, DMR_MIC_GAIN));
	fm_rx_setup();
	count(hrc6000_set_rx_gain(&modem, RX_GAIN_DB));
	count(hrc6000_tones_off(&modem));	// with the init's 0x80 the chip never syncs on DMR (hardware-md9600.md 4.2)
	diag.up = hrc_read(&modem, 4, 0x10, &diag.r10);
	stm32_hrc_dmr_init(&modem, &dmr_board);
	speaker_apply();
}

#if CONFIG_DMR
// The receive I/Q order follows the first LO's side (hardware-md9600.md 4.2, 5.2): above the signal on VHF, which
// inverts the IF, and below it on UHF. Swapped on VHF, straight on UHF
static bool iq_swap(uint32_t hz)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(hz, &band);
	return band == MD9600_VHF;
}
#endif

void md9600_c6000_mode(bool dmr, uint32_t hz)
{
#if !CONFIG_DMR
	dmr = false;
#endif
	if (dmr == diag.dmr) {
		return;
	}
	diag.dmr = dmr;
	if (dmr) {
#if CONFIG_DMR
		count(hrc6000_rx_iq_swap(&modem, iq_swap(hz), false));
		count(hrc6000_dmr_enter(&modem, cal_gain(CAL_DMR_I_UHF, CAL_DMR_I_VHF, hz),
					cal_gain(CAL_DMR_Q_UHF, CAL_DMR_Q_VHF, hz), DMR_MIC_GAIN));
#endif
		ref_offset(hz);
		tuned_hz = hz;
		stm32_hrc_dmr_enter();
	} else {
		stm32_hrc_dmr_leave();
		count(hrc6000_set_mode_fm_rx(&modem));
		count(hrc6000_fm_audio(&modem, false));
		count(hrc6000_fm_rx_pcm(&modem));
	}
	speaker_apply();
	fm_tap_update();
}

void md9600_c6000_retune(uint32_t hz)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(hz, &band);
	band_vhf = band == MD9600_VHF;	// rx() has switched U_V_AF_SW to it
	if (!diag.up) {
		return;
	}
	ref_offset(hz);
#if CONFIG_DMR
	if (diag.dmr && iq_swap(hz) != iq_swap(tuned_hz)) {
		count(hrc6000_rx_iq_swap(&modem, iq_swap(hz), true));	// a band change
	}
#endif
	if (diag.dmr && hz != tuned_hz) {
		// New channel: its modulation pair, and the receiver searches again
		count(hrc_set_mod_gain(&modem, cal_gain(CAL_DMR_I_UHF, CAL_DMR_I_VHF, hz),
				       cal_gain(CAL_DMR_Q_UHF, CAL_DMR_Q_VHF, hz)));
		stm32_hrc_dmr_rx_restart();
	}
	tuned_hz = hz;
}

void md9600_c6000_diag(md9600_c6000_diag_t *d)
{
	*d = diag;
}

// --- hal_audio_t ---

#if CONFIG_DMR
// Clock idle high, as on the other HR_C6000 radios (unverified here)
static const stm32_i2s_pcm_cfg_t i2s_cfg = { .ckpol = true };

#if MD9600_FM_RX_TAP
static bool audio_start(audio_block_cb_t cb, void *ctx)
{
	stream_cb = cb;
	stream_ctx = ctx;
	if (!stm32_i2s_pcm_start(&i2s_cfg, stream_block, NULL)) {
		return false;
	}
	stream_on = true;
	fm_tap_update();
	return true;
}

static void audio_stop(void)
{
	stm32_i2s_pcm_stop();
	stream_on = false;
	fm_tap_update();
}
#else
static bool audio_start(audio_block_cb_t cb, void *ctx)
{
	return stm32_i2s_pcm_start(&i2s_cfg, cb, ctx);
}

static void audio_stop(void)
{
	stm32_i2s_pcm_stop();
}
#endif

static void speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	speaker_apply();
}

// The radio core's amplifier hang (hal_audio_t.speaker_warm): the amplifier on with no source through it
static void speaker_warm(bool on)
{
	if (on == warm_wanted) {
		return;
	}
	warm_wanted = on;
	speaker_apply();
}

// The amplifier takes about 60 ms from unmute to sound (by a microphone at the speaker: with the default 20 ms lead-in
// a lone 25 or 40 ms beep was lost and longer ones lost their start; with 80 ms every beep plays whole). A mobile on
// a supply: the amplifier stays up for 30 s after the last key, PTT, reception or beep, so those play at once
const hal_audio_t md9600_audio = { .start = audio_start, .stop = audio_stop, .speaker_pcm = speaker_pcm,
				   .speaker_lead_ms = 80, .speaker_warm = speaker_warm, .speaker_hang_s = 30,
				   .stream_stats = stm32_i2s_pcm_stream_stats };
#endif

#if CONFIG_DMR && (CONFIG_AUDIO_PROBE || MD9600_FM_RX_TAP)
#if CONFIG_AUDIO_PROBE
// The audio probe's register access (hal_audio_tap_t.modem_reg): one HR_C6000 register, any task. It can change
// state under the driver, which is what a bench probe is for
static bool probe_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write)
{
	return write ? hrc_write(&modem, page, reg, *val) : hrc_read(&modem, page, reg, val);
}

static bool probe_burst(uint8_t page, uint8_t reg, const uint8_t *buf, uint16_t len)
{
	return hrc_write_burst(&modem, page, reg, buf, len);
}

// PC8 (the beep PWM net, hardware-md9600.md 2.3): where it goes is unknown. A square wave on it while keyed shows
// whether it reaches the transmit audio (test_audio_probe.py). TIM7's interrupt toggles the pin as a GPIO, so TIM8
// keeps its timebase for the backlight and the sub-audio tone
void TIM7_IRQHandler(void)
{
	TIM7->SR = 0;
	GPIO_TypeDef *port = PIN_PORT(PIN_BEEP_PWM);
	uint16_t mask = PIN_MASK(PIN_BEEP_PWM);
	port->BSRR = (port->ODR & mask) ? (uint32_t)mask << 16 : mask;
}

static void probe_pin_tone(uint16_t hz)
{
	TIM7->CR1 = 0;
	TIM7->DIER = 0;
	HAL_NVIC_DisableIRQ(TIM7_IRQn);
	md9600_pwm_out(MD9600_PWM_BEEP, 0);	// the pin a low output
	if (hz == 0) {
		return;
	}
	__HAL_RCC_TIM7_CLK_ENABLE();
	uint32_t pclk = HAL_RCC_GetPCLK1Freq();
	uint32_t clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk : 2u * pclk;	// APB1 timer clock
	TIM7->PSC = clk / 1000000u - 1u;	// 1 MHz
	TIM7->ARR = 500000u / hz - 1u;		// a toggle every half period
	TIM7->CNT = 0;
	TIM7->EGR = TIM_EGR_UG;
	TIM7->SR = 0;
	TIM7->DIER = TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM7_IRQn, 6, 0);	// no RTOS calls in the handler
	HAL_NVIC_EnableIRQ(TIM7_IRQn);
	TIM7->CR1 = TIM_CR1_CEN;
}

// PWM audio on PC8 (hal_audio_tap_t.pin_pwm) through the same path as fm_tx_inject: a sine of hz at amp_permille of
// half the PWM range. top is ignored: TIM8's period is the backlight's (256 counts at 7.2 MHz)
static volatile float probe_step, probe_phase, probe_amp;

static void probe_sine(int16_t *pcm, uint16_t n)
{
	float ph = probe_phase;
	for (uint16_t i = 0; i < n; i++) {
		pcm[i] = (int16_t)(probe_amp * sinf(6.2831853f * ph));
		ph += probe_step;
		if (ph >= 1.0f) {
			ph -= 1.0f;
		}
	}
	probe_phase = ph;
}

static uint32_t probe_pin_pwm(uint16_t top, uint16_t hz, uint16_t amp_permille)
{
	(void)top;
	md9600_pwm_audio(NULL, 0);
	if (hz == 0 || hz >= 4000u) {
		return 0;
	}
	probe_step = (float)hz / 8000.0f;
	probe_phase = 0.0f;
	probe_amp = 32767.0f * (float)(amp_permille > 1000u ? 1000u : amp_permille) / 1000.0f;
	md9600_pwm_audio(probe_sine, 127);
	return 72000000u / 10u / 256u;
}

// Every MCU input sampled for ms: the pins that changed, per port A-E
static uint8_t probe_pin_scan(uint16_t ms, uint16_t *changed, uint8_t max)
{
	GPIO_TypeDef *const ports[] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };
	uint8_t n = max < 5u ? max : 5u;
	uint16_t first[5];
	for (uint8_t i = 0; i < n; i++) {
		first[i] = (uint16_t)ports[i]->IDR;
		changed[i] = 0;
	}
	uint32_t t0 = hal_millis();
	while (hal_millis() - t0 < ms) {
		for (uint8_t i = 0; i < n; i++) {
			changed[i] |= (uint16_t)(ports[i]->IDR ^ first[i]);
		}
	}
	return n;
}
#endif

const hal_audio_tap_t md9600_audio_tap = {
	.caps = AUDIO_TAP_DMR
#if MD9600_FM_RX_TAP
		| AUDIO_TAP_FM_RX
#endif
#if MD9600_FM_TX_INJECT
		| AUDIO_TAP_FM_TX
#endif
		,
#if MD9600_FM_TX_INJECT
	.fm_tx_inject = fm_tx_inject,
	.fm_tx_nominal_peak = INJECT_NOMINAL_PEAK,
#endif
#if CONFIG_AUDIO_PROBE
	.modem_reg = probe_reg,
	.modem_burst = probe_burst,
	.pin_tone = probe_pin_tone,
	.pin_scan = probe_pin_scan,
	.pin_pwm = probe_pin_pwm,
#endif
};
#endif
