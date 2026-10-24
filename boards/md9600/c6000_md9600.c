// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
// the build has CONFIG_MD9600_DMR_TX and md9600_dmr_tx_allow(true) was called, and even then it needs the RF chain's
// per-burst keying (md9600_rf_dmr_key), which the radio code doesn't have yet. The radio's tx() refuses as well, so no
// DMR transmission starts.
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
static bool fm_wanted, pcm_wanted, band_vhf;
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
	md9600_speaker_t s = md9600_speaker_lines(fm_wanted, pcm_wanted, diag.dmr, band_vhf);
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
	diag.speaker = (uint8_t)((s.af_mute ? 0u : 1u) | (s.fm_pass ? 2u : 0u) | (s.codec_out ? 4u : 0u));
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

// --- DMR glue ---

#if CONFIG_MD9600_DMR_TX
static volatile bool tx_allowed;

void md9600_dmr_tx_allow(bool on)
{
	tx_allowed = on;
}

// The RF chain's per-burst keying (hardware-md9600.md 5.5: T5 and RF_APC_SW per burst). Not written: refuse to key,
// accept the return to receive (nothing was keyed)
__attribute__((weak)) bool md9600_rf_dmr_key(bool tx)
{
	return !tx;
}
#endif

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
	count(hrc6000_set_mode_fm_rx(&modem));
	count(hrc6000_fm_audio(&modem, false));		// FM audio never passes through this chip
	count(hrc6000_set_rx_gain(&modem, RX_GAIN_DB));
	diag.up = hrc_read(&modem, 4, 0x10, &diag.r10);
	stm32_hrc_dmr_init(&modem, &dmr_board);
	speaker_apply();
}

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
	}
	speaker_apply();
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

static bool audio_start(audio_block_cb_t cb, void *ctx)
{
	return stm32_i2s_pcm_start(&i2s_cfg, cb, ctx);
}

static void speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	speaker_apply();
}

const hal_audio_t md9600_audio = { .start = audio_start, .stop = stm32_i2s_pcm_stop, .speaker_pcm = speaker_pcm };
#endif
