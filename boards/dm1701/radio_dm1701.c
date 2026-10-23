// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 radio: AT1846S (I2C3) + HR_C6000 (bit-banged config SPI, voice SPI2) + RF path GPIOs + APC DAC +
// calibration. FM receive and transmit (docs/spec/hardware-dm1701.md 4-8, 12; docs/spec/modem-hrc6000.md 3, 8), and the
// RF side of DMR (modem-hrc6000.md 3.1, 4.3; hardware-dm1701.md 5.3, 6): set_mode(DMR) puts both chips in DMR and starts
// the slot engine (platform/stm32f4/hrc/dmr_glue.c); tx() in DMR only arms the per-burst keying (TX frequency and PA drive) and the PA
// then follows the bursts from the modem interrupts through dm1701_rf_dmr_switch(), which writes AT1846S reg 0x30
// directly over I2C3 (never through the task-level driver and its cache, which is resynced when DMR TX ends).
// tx_off() in DMR belongs after DMR_EV_TX_DONE; called while a transmission runs it drops it.
//
// TX switching sequence (gotchas 3.5): the TX frequency is written with the receiver off (reg 0x30 idle, then the
// synthesizer words) and the chip goes straight from idle to FM TX, never through RX on. With this sequence the old
// on-air measurement found no offset, so DM1701_TX_OFFSET_HZ_* default to 0. Measure the carrier with this exact
// sequence and put the measured correction there; do not copy the +3 kHz of the other sequence.
#include <stddef.h>
#include "config.h"
#include "dm1701.h"
#include "pins.h"
#include "hal_time.h"
#include "hal_dtmf.h"
#include "rf/at1846s/at1846s.h"
#include "modem/hrc6000/hrc6000.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "hrc/hrc_bus.h"
#include "dmr_dm1701.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// Measured TX carrier correction for the sequence above, per bandwidth (Hz, added to the TX frequency)
#define DM1701_TX_OFFSET_HZ_12K5	0
#define DM1701_TX_OFFSET_HZ_25K		0

// APC DAC value when no calibration is loaded: deliberately very low, never an uncalibrated high drive
#define DM1701_DAC_UNCALIBRATED		300u
#define DAC_MAX				4095u

#define UHF_MIN_HZ		300000000u
#define FM_MIC_GAIN		7	// 0..31, as the init value 0xE4 = 0x27
#define DMR_MIC_GAIN		7
#define I2C_TRIES		3
#define I2C_TIMEOUT_MS		5
#define C6000_PWD_WAIT_MS	12	// at least 10 ms after PWD low before the first SPI access
#define RF_ISR_I2C_TIMEOUT_MS	2	// one reg 0x30 write from a modem ISR takes about 0.1 ms at 400 kHz

// Voice SPI2 (hardware-dm1701.md 4.3): PB13 SCK, PB14 MISO, PB15 MOSI (AF5), chip select PIN_C6000_VCS
#define VOICE_SPI_PINS		(GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15)

// DM-1701 power interpolation fractions (hardware-dm1701.md 7), per mille: f0 f1 f2 f3
static const uint16_t frac_vhf[4] = { 450, 750, 250, 530 };
static const uint16_t frac_uhf[4] = { 460, 730, 140, 360 };

static struct {
	at1846s_t rf;
	hrc_t modem;
	radio_mode_t mode;
	bool tx_on;
	bool rx_valid;
	bool rx_uhf;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	uint32_t bus_errors;	// I2C failures and failed chip operations since start (debug)
	dm1701_radio_diag_t diag;
	radio_status_t last_status;	// returned without bus traffic while DMR transmits
	uint16_t tone_hz;		// FM TX tone in place of the microphone (tx_tone), 0 = none
	uint16_t saved_59, saved_79;	// AT1846S tone deviation and tone select before the tone
	uint16_t saved_3a, saved_7a;	// AT1846S voice source (0x3A bits 14:12) and single tone enable before the tone
	bool asleep;			// rx_sleep(true): receiver off
	volatile bool afsk;		// APRS AFSK running (afsk_dm1701.c): the TIM6 interrupt owns I2C3
	bool inject;			// FM TX audio from the MCU (fm_tx_inject) instead of the microphone

	// DMR per-burst keying (shared with the modem ISRs)
	volatile bool dmr_armed;	// tx() in DMR mode: TX frequency and drive known
	volatile uint32_t dmr_tx_hz;
	volatile uint32_t dmr_drive;
	volatile bool dmr_uhf;
	volatile uint32_t rf_tuned_hz;	// what the AT1846S synthesizer holds, 0 unknown
	volatile bool rf_isr_dirty;	// the ISR wrote the AT1846S behind the task-level driver
	volatile bool rf_rx_pending;	// a back-to-RX write was skipped: retry at the next timeslot interrupt
	volatile uint8_t rf_page;	// AT1846S page last written to reg 0x7F, 0xFF unknown
	uint32_t rf_skipped, rf_unarmed, rf_rx_retries;
} r = { .rf_page = 0xFF };

static I2C_HandleTypeDef i2c3;
static DAC_HandleTypeDef dac;

static bool in_isr(void)
{
	return __get_IPSR() != 0;
}

static void count(bool ok)
{
	if (!ok) {
		r.bus_errors++;
	}
}

// --- AT1846S bus: I2C3 (hardware-dm1701.md 5.1) ---

static void i2c_setup(void)
{
	__HAL_RCC_I2C3_CLK_ENABLE();
	stm32_pin_af(PIN_PORT(PIN_I2C3_SCL), PIN_MASK(PIN_I2C3_SCL), GPIO_AF4_I2C3, true, GPIO_PULLUP);
	stm32_pin_af(PIN_PORT(PIN_I2C3_SDA), PIN_MASK(PIN_I2C3_SDA), GPIO_AF4_I2C3, true, GPIO_PULLUP);
	i2c3.Instance = I2C3;
	i2c3.Init.ClockSpeed = 400000;
	i2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
	i2c3.Init.OwnAddress1 = 0;
	i2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	i2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	i2c3.Init.OwnAddress2 = 0;
	i2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	i2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
	HAL_I2C_Init(&i2c3);
}

// One register transfer. Task context: up to 3 tries 1 ms apart, then the peripheral is reset. ISR context: one
// try, and none at all while a task transfer owns the bus (skip, never wait)
static bool i2c_xfer(uint8_t reg, uint8_t *buf, bool write)
{
	bool isr = in_isr();
	int tries = isr ? 1 : I2C_TRIES;
	for (int i = 0; i < tries; i++) {
		if (isr && HAL_I2C_GetState(&i2c3) != HAL_I2C_STATE_READY) {
			return false;
		}
		HAL_StatusTypeDef st = write
			? HAL_I2C_Mem_Write(&i2c3, AT1846S_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, 2, I2C_TIMEOUT_MS)
			: HAL_I2C_Mem_Read(&i2c3, AT1846S_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, 2, I2C_TIMEOUT_MS);
		if (st == HAL_OK) {
			return true;
		}
		if (i + 1 < tries) {
			hal_delay_ms(1);
		}
	}
	r.bus_errors++;
	if (!isr) {
		HAL_I2C_DeInit(&i2c3);
		i2c_setup();
	}
	return false;
}

static bool rf_write(void *ctx, uint8_t reg, uint16_t val)
{
	uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)val };
	if (reg == 0x7F) {
		r.rf_page = 0xFF;	// unknown until the write is through (the modem ISRs only write on page 0)
	}
	bool ok = i2c_xfer(reg, buf, true);
	if (ok && reg == 0x7F) {
		r.rf_page = (uint8_t)val;
	}
	return ok;
}

// One AT1846S page 0 register write from a modem ISR (or with them masked): a single try, skipped when a task-level
// transfer owns I2C3 or the chip is on another page. Never touches the task-level driver's cache
static bool rf_isr_write(uint8_t reg, uint16_t val)
{
	if (r.rf_page != 0 || HAL_I2C_GetState(&i2c3) != HAL_I2C_STATE_READY) {
		return false;
	}
	uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)val };
	return HAL_I2C_Mem_Write(&i2c3, AT1846S_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, 2, RF_ISR_I2C_TIMEOUT_MS)
		== HAL_OK;
}

static bool rf_read(void *ctx, uint8_t reg, uint16_t *val)
{
	uint8_t buf[2];
	if (!i2c_xfer(reg, buf, false)) {
		return false;
	}
	*val = (uint16_t)((buf[0] << 8) | buf[1]);
	return true;
}

static const at1846s_bus_t rf_bus = { .write = rf_write, .read = rf_read, .delay_ms = hal_delay_ms, .ctx = NULL };

// --- HR_C6000 bus: bit-banged config SPI on PE2-PE5 (hardware-dm1701.md 4.2) ---

// The configuration port on PE2-PE5 and the voice port on SPI2 (PB13-PB15, AF5, chip select PIN_C6000_VCS): mode 2,
// PCLK1 / 8 = 4.5 MHz. The shared transfers (platform/stm32f4/hrc/hrc_bus.c) mask the modem interrupts around each
static const stm32_hrc_bus_cfg_t modem_cfg = {
	.port = GPIOE,
	.cs = PIN_MASK(PIN_C6000_CS), .clk = PIN_MASK(PIN_C6000_CLK), .mosi = PIN_MASK(PIN_C6000_MOSI),
	.miso = PIN_MASK(PIN_C6000_MISO),
	.voice_spi = SPI2, .voice_br = 2,
	.voice_cs_port = PIN_PORT(PIN_C6000_VCS), .voice_cs = PIN_MASK(PIN_C6000_VCS),
};
static hrc_bus_t modem_bus;

static void voice_spi_setup(void)
{
	stm32_pin_af(GPIOB, VOICE_SPI_PINS, GPIO_AF5_SPI2, false, GPIO_NOPULL);
	stm32_hrc_bus_init(&modem_cfg, &modem_bus);
}

// --- APC DAC and RF path GPIOs (hardware-dm1701.md 6, 7) ---

static void dac_setup(void)
{
	__HAL_RCC_DAC_CLK_ENABLE();
	stm32_pin_analog(PIN_PORT(PIN_APC_DAC), PIN_MASK(PIN_APC_DAC));
	dac.Instance = DAC1;
	HAL_DAC_Init(&dac);
	DAC_ChannelConfTypeDef ch = { .DAC_Trigger = DAC_TRIGGER_NONE, .DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE };
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_1);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0);
	HAL_DAC_Start(&dac, DAC_CHANNEL_1);
}

static void apc_set(uint32_t value)
{
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, value > DAC_MAX ? DAC_MAX : value);
}

static void pa_off(void)
{
	PIN_WRITE(PIN_PA_EN2, false);
	PIN_WRITE(PIN_PA_EN1, false);
	PIN_WRITE(PIN_PA_BAND, false);
	apc_set(0);
}

static void rx_supply(bool on, bool uhf)
{
	PIN_WRITE(PIN_RX_UHF_5V, on && uhf);
	PIN_WRITE(PIN_RX_VHF_5V, on && !uhf);
}

static int32_t lerp(int32_t a, int32_t b, uint16_t per_mille)
{
	return a + (b - a) * (int32_t)per_mille / 1000;
}

// Board power level -> DAC value from the four calibrated curves (hardware-dm1701.md 7, DM-1701 fractions)
static uint32_t power_to_dac(const dm1701_cal_point_t *cal, uint8_t level, bool uhf)
{
	const uint16_t *f = uhf ? frac_uhf : frac_vhf;
	int32_t vl = cal->power_dac[0], lo = cal->power_dac[1], mid = cal->power_dac[2], hi = cal->power_dac[3];
	int32_t v;
	switch (level) {
	case 0: v = vl > 160 ? vl - 160 : 0; break;
	case 1: v = vl; break;
	case 2: v = lerp(vl, lo, f[0]); break;
	case 3: v = lerp(vl, lo, f[1]); break;
	case 4: v = lo; break;
	case 5: v = mid; break;
	case 6: v = lerp(mid, hi, f[2]); break;
	case 7: v = lerp(mid, hi, f[3]); break;
	default: v = hi; break;
	}
	if (v < 0) {
		v = 0;
	}
	return (uint32_t)v > DAC_MAX ? DAC_MAX : (uint32_t)v;
}

// --- Helpers ---

static const band_t *find_band(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return &board.bands[i];
		}
	}
	return NULL;
}

static at1846s_tone_t rf_tone(const subtone_t *t, uint16_t *value)
{
	*value = 0;
	if (t == NULL || t->value == 0) {
		return AT1846S_TONE_NONE;
	}
	*value = t->value;
	switch (t->type) {
	case TONE_CTCSS: return AT1846S_TONE_CTCSS;
	case TONE_DCS: return AT1846S_TONE_DCS;
	case TONE_DCS_INV: return AT1846S_TONE_DCS_INV;
	default: return AT1846S_TONE_NONE;
	}
}

static at1846s_bw_t rf_bw(radio_bw_t bw)
{
	return bw == RADIO_BW_25K ? AT1846S_BW_25K : AT1846S_BW_12K5;
}

static void apply_rx_tone(void)
{
	uint16_t value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	count(at1846s_set_rx_tone(&r.rf, type, value));
}

// --- DMR per-burst RF switching (modem-hrc6000.md 4.3, hardware-dm1701.md 6): modem ISR context ---

bool dm1701_rf_dmr_switch(bool tx)
{
	if (!tx) {
		pa_off();
		r.rf_isr_dirty = true;
		// As the TX side: transmitter off first, the RX frequency, then the RX supply and the receiver on. Straight from
		// DMR TX to RX on (one 0x30 write) left the receiver marginal between an SFR's bursts (inbound bursts lost at
		// random, HIL 2026-10-12), and the receiver stayed on the TX frequency where RX != TX
		uint16_t idle = at1846s_state_value(rf_bw(r.rx_bw), AT1846S_STATE_IDLE);
		bool ok = rf_isr_write(0x30, idle);
		if (ok && r.rf_tuned_hz != r.rx_hz) {
			uint32_t word = at1846s_freq_word(r.rx_hz);
			r.rf_tuned_hz = 0;
			ok = rf_isr_write(0x29, (uint16_t)((word >> 16) & 0x3FFFu)) && rf_isr_write(0x2A, (uint16_t)(word & 0xFFFFu));
			if (ok) {
				r.rf_tuned_hz = r.rx_hz;
			}
		}
		rx_supply(true, r.rx_uhf);
		if (ok && rf_isr_write(0x30, at1846s_state_value(rf_bw(r.rx_bw), AT1846S_STATE_RX))) {
			r.rf_rx_pending = false;
			return true;
		}
		r.rf_rx_pending = true;
		r.rf_skipped++;
		return false;
	}
	if (!r.dmr_armed) {
		r.rf_unarmed++;
		return false;
	}
	uint32_t hz = r.dmr_tx_hz;
	bool ok = true;
	r.rf_isr_dirty = true;
	// Always tune with the receiver off, then straight to TX (gotchas 3.5), simplex included: going from RX on
	// straight to TX on the already-tuned frequency put the DMR carrier about 3.07 kHz low (SDR bench, 2026-10-10),
	// too far for a 4FSK receiver to decode. The extra two writes cost about 0.25 ms per burst
	r.rf_tuned_hz = 0;
	if (r.rf_tuned_hz != hz) {
		uint32_t word = at1846s_freq_word(hz);
		r.rf_tuned_hz = 0;
		ok = rf_isr_write(0x30, at1846s_state_value(rf_bw(r.rx_bw), AT1846S_STATE_IDLE))
			&& rf_isr_write(0x29, (uint16_t)((word >> 16) & 0x3FFFu))
			&& rf_isr_write(0x2A, (uint16_t)(word & 0xFFFFu));
		if (ok) {
			r.rf_tuned_hz = hz;
		}
	}
	ok = ok && rf_isr_write(0x30, at1846s_state_value(rf_bw(r.rx_bw), AT1846S_STATE_DMR_TX));
	if (!ok) {
		r.rf_skipped++;
		return false;	// no RF without the AT1846S in TX: leave the PA off
	}
	r.rf_rx_pending = false;
	rx_supply(false, false);	// the RX pre-amps off while the PA is up (hardware-dm1701.md 6)
	apc_set(r.dmr_drive);
	PIN_WRITE(PIN_PA_EN1, true);
	PIN_WRITE(PIN_PA_BAND, r.dmr_uhf);
	PIN_WRITE(PIN_PA_EN2, true);
	return true;
}

void dm1701_rf_dmr_service(void)
{
	if (r.rf_rx_pending) {
		r.rf_rx_retries++;
		(void)dm1701_rf_dmr_switch(false);	// the whole sequence again (it clears rf_rx_pending)
	}
}

void dm1701_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx_retries)
{
	*skipped = r.rf_skipped;
	*unarmed = r.rf_unarmed;
	*rx_retries = r.rf_rx_retries;
}

// The shared DMR glue's view of this board: the modem's interrupt lines are PC0-PC2
static const stm32_hrc_dmr_board_t dmr_board = {
	.int_port = GPIOC,
	.rf_switch = dm1701_rf_dmr_switch,
	.ts_service = dm1701_rf_dmr_service,
	.rf_counters = dm1701_rf_dmr_counters,
};

// Stop per-burst keying (task level): disarm, drop a transmission still running, PA off
static void dmr_disarm(void)
{
	r.dmr_armed = false;
	if (stm32_hrc_dmr_tx_active()) {
		stm32_hrc_dmr_tx_drop();
	}
	pa_off();
}

// The AT1846S again as the task-level driver believes it: the modem ISRs may have retuned it and switched reg 0x30
static void rf_resync(void)
{
	if (!r.rf_isr_dirty) {
		return;
	}
	r.rf_isr_dirty = false;
	r.rf_rx_pending = false;
	if (r.rx_valid) {
		count(at1846s_set_freq(&r.rf, r.rx_hz));	// forced writes of 0x30 (idle), 0x29, 0x2A
		r.rf_tuned_hz = r.rx_hz;
	}
	count(at1846s_set_state(&r.rf, r.rx_valid ? AT1846S_STATE_RX : AT1846S_STATE_IDLE));	// forced
}

// HR_C6000 into DMR (modem-hrc6000.md 3.1) with the DMR modulation pair for the frequency, then the slot engine
static void dmr_enter(void)
{
	dm1701_cal_point_t cal;
	dm1701_cal_at(r.rx_valid ? r.rx_hz : board.bands[0].lo_hz, &cal);	// on false it fills safe defaults
#if CONFIG_DMR
	count(hrc6000_dmr_enter(&r.modem, cal.dmr_i, cal.dmr_q, DMR_MIC_GAIN));
#endif
	stm32_hrc_dmr_enter();
}

// --- hal_radio_t ---

// The speaker serves two requests: the core's FM squelch (audio(), radio task) and DMR voice PCM
// (dm1701_speaker_pcm(), from the audio task's block callback or the radio task). The amp is on for either; while
// PCM plays the FM feed-through (4:0x36) is off and the HR_C6000 codec DAC drives the line out. A mutex keeps the two
// callers' register and pin writes from interleaving.
static bool audio_wanted;	// the core wants the FM speaker on
static bool pcm_wanted;		// DMR voice PCM is playing
static bool volume_zero = false;	// knob at the bottom: silent (the lowest gain, -31 dB, is still audible)
static SemaphoreHandle_t speaker_lock;

// HR_C6000 codec set for PCM playback: DAC to the line out (4:0xE2 bit 1), and in DMR the codec under MCU control
// (4:0xE0 = 0xC9) with the I2S mode bits of 4:0x22 (0x06, as at init and in the 0x86 TX start) kept set
static void pcm_codec_on(void)
{
	uint8_t v;
	if (hrc_read(&r.modem, 4, 0xE2, &v) && (v & 0x02u) == 0) {
		count(hrc_write(&r.modem, 4, 0xE2, (uint8_t)(v | 0x02u)));
	}
	if (r.mode == RADIO_MODE_DMR) {
		count(hrc_write(&r.modem, 4, 0xE0, 0xC9));
		if (hrc_read(&r.modem, 4, 0x22, &v) && (v & 0x06u) != 0x06u) {
			count(hrc_write(&r.modem, 4, 0x22, (uint8_t)(v | 0x06u)));
		}
	}
}

static void speaker_apply(void)
{
	bool locked = speaker_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (locked) {
		xSemaphoreTake(speaker_lock, portMAX_DELAY);
	}
	bool pcm = pcm_wanted;
	bool fm = audio_wanted && !pcm;
	bool on = (fm || pcm) && !volume_zero;
	// The FM feed-through also gates the FM audio on the I2S stream that APRS receive reads (hardware-dm1701.md
	// 21.3.1), so it follows the core's request, not the knob: at volume zero only the amp is off
	count(hrc6000_fm_audio(&r.modem, fm));	// FM feed-through only for FM; PCM comes from the codec DAC
	if (on) {
		if (pcm) {
			pcm_codec_on();
		}
		PIN_WRITE(PIN_RX_AUDIO_MUX, true);	// HR_C6000 line out to the amp
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		PIN_WRITE(PIN_SPK_MUTE, true);
		PIN_WRITE(PIN_AMP_EN, false);
	}
	if (locked) {
		xSemaphoreGive(speaker_lock);
	}
}

static void audio(bool on)
{
	audio_wanted = on;
	speaker_apply();
}

#if CONFIG_DMR
// Overrides the fallback in audio_dm1701.c (hal_audio_t.speaker_pcm)
void dm1701_speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	speaker_apply();
}
#endif

static void init(void)
{
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
	}
	PIN_WRITE(PIN_MIC_POWER, true);		// kept on in RX and TX (4.5: some speaker-mics see false PTT without bias)
	rx_supply(false, false);
	dac_setup();
	pa_off();
	i2c_setup();
	voice_spi_setup();

	PIN_WRITE(PIN_C6000_PWD, false);	// the HR_C6000 wakes while the AT1846S initializes
	uint32_t pwd_ms = hal_millis();

	at1846s_attach(&r.rf, &rf_bus);
	count(at1846s_init(&r.rf));

	while ((uint32_t)(hal_millis() - pwd_ms) < C6000_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc6000_attach(&r.modem, &modem_bus);
	count(hrc6000_power_on_init(&r.modem, DMR_MIC_GAIN));
	count(at1846s_init_finish(&r.rf));

	r.mode = RADIO_MODE_FM;
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	audio(false);
	r.diag.c6000_ok = hrc_read(&r.modem, 4, 0x10, &r.diag.c6000_r10);
	stm32_hrc_dmr_init(&r.modem, &dmr_board);
}

static void set_mode(radio_mode_t mode)
{
	radio_mode_t prev = r.mode;
	r.mode = mode;
	if (mode == RADIO_MODE_DMR) {
		count(at1846s_set_mode(&r.rf, AT1846S_MODE_DMR));
		if (prev != RADIO_MODE_DMR) {
			dmr_enter();
		}
	} else {
		if (prev == RADIO_MODE_DMR) {
			dmr_disarm();
			stm32_hrc_dmr_leave();
			rf_resync();
			r.tx_on = false;
		}
		count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
		if (!r.tx_on) {
			count(hrc6000_set_mode_fm_rx(&r.modem));
		}
	}
	apply_rx_tone();	// the mode table rewrote reg 0x3A
}

// Back from an FM TX tone (tx_tone): the microphone as the voice source again, the tone registers as they were
static void tone_restore(void)
{
	if (r.tone_hz == 0) {
		return;
	}
	uint8_t e0;
	count(at1846s_modify(&r.rf, 0, 0x3A, 0x8FFF, (uint16_t)(r.saved_3a & 0x7000u)));	// voice source back
	count(at1846s_write(&r.rf, 0, 0x7A, r.saved_7a));
	count(at1846s_write(&r.rf, 0, 0x59, r.saved_59));
	count(at1846s_write(&r.rf, 0, 0x79, r.saved_79));
	if (hrc_read(&r.modem, 4, 0xE0, &e0)) {
		count(hrc_write(&r.modem, 4, 0xE0, (uint8_t)(e0 | 0x40u)));
	}
	r.tone_hz = 0;
}

#if CONFIG_NET_USRP
// FM TX audio from the MCU (hardware-dm1701.md 21.3.1, "PWM audio on PC8"): PWM on PC8 reaches the HR_C6000's
// differential microphone input, so MIC_P / MIC_N (4:0xE0 bits 1-2) go on and the microphone (LINEIN1, bit 6) off.
// The chip's FM chain does the rest as for the microphone: pre-emphasis, limiter, sub-audio tone. The mode tables
// rewrite 0xE0 at the next rx(), which ends the injection (inject_stop)
static void inject_stop(void)
{
	if (!r.inject) {
		return;
	}
	r.inject = false;
	dm1701_pwm_audio(NULL);
	uint8_t e0;
	if (hrc_read(&r.modem, 4, 0xE0, &e0)) {
		count(hrc_write(&r.modem, 4, 0xE0, (uint8_t)((e0 | 0x40u) & ~0x06u)));	// the microphone again
	}
}

bool dm1701_fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
	if (pull == NULL) {
		inject_stop();
		return true;
	}
	if (!r.tx_on || r.mode != RADIO_MODE_FM || mode != AUDIO_TAP_VOICE) {
		return false;	// flat (0x34's processing off) isn't offered: the PWM path stops at the codec's 3.5 kHz
	}
	uint8_t e0;
	if (!hrc_read(&r.modem, 4, 0xE0, &e0)) {
		count(false);
		return false;
	}
	dm1701_pwm_audio(pull);
	count(hrc_write(&r.modem, 4, 0xE0, (uint8_t)((e0 & ~0x40u) | 0x06u)));
	r.inject = true;
	return true;
}
#else
static void inject_stop(void)
{
}
#endif

// Enter RX (hardware-dm1701.md 6): PA off first, RX supply for the band, AT1846S to RX, then the HR_C6000
static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	if (find_band(hz) == NULL) {
		return false;
	}
	bool uhf = hz >= UHF_MIN_HZ;
	bool same = r.rx_valid && hz == r.rx_hz && bw == r.rx_bw;
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();
	}
	r.asleep = false;
	tone_restore();
	inject_stop();
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_tone = rx_tone != NULL ? *rx_tone : (subtone_t){ TONE_NONE, 0 };
	r.rx_uhf = uhf;
	r.rx_valid = true;

	pa_off();
	rx_supply(true, uhf);
	count(at1846s_set_bandwidth(&r.rf, rf_bw(bw)));
	count(at1846s_set_mode(&r.rf, r.mode == RADIO_MODE_DMR ? AT1846S_MODE_DMR : AT1846S_MODE_FM));
	count(at1846s_set_freq(&r.rf, hz));
	apply_rx_tone();
	count(at1846s_set_state(&r.rf, AT1846S_STATE_RX));
	r.rf_tuned_hz = hz;
	r.rf_isr_dirty = false;
	r.rf_rx_pending = false;

	dm1701_cal_point_t cal;
	dm1701_cal_at(hz, &cal);	// on false it fills safe defaults
	count(hrc6000_set_ref_offset(&r.modem, cal.ref_offset, false));
	if (r.mode == RADIO_MODE_FM) {
		count(hrc6000_set_mode_fm_rx(&r.modem));
	} else if (!same) {
		// New channel: the DMR modulation pair for it, and the receiver searches again (5.1). Back from our own
		// transmission on the same channel the engine has already restarted it, or kept a repeater's timing
		count(hrc_set_mod_gain(&r.modem, cal.dmr_i, cal.dmr_q));
		stm32_hrc_dmr_rx_restart();
	}
	r.tx_on = false;
	return true;
}

// Enter FM TX (hardware-dm1701.md 6): RX supplies off, HR_C6000 FM TX, AT1846S tuned with the receiver off and
// switched straight to TX, APC drive, then PA enable 1, band select, PA enable 2
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	const band_t *band = find_band(hz);
	if (band == NULL || !band->tx) {
		return false;
	}
	bool uhf = hz >= UHF_MIN_HZ;
	if (board.power_count > 0 && power >= board.power_count) {
		power = (uint8_t)(board.power_count - 1);
	}

	dm1701_cal_point_t cal;
	bool calibrated = dm1701_cal_at(hz, &cal) && cal.valid;
	uint32_t drive = calibrated ? power_to_dac(&cal, power, uhf) : DM1701_DAC_UNCALIBRATED;

	if (r.mode == RADIO_MODE_DMR) {
		// Arm only: the receiver stays on the RX frequency (a repeater's slot timing is followed until our first
		// burst); each burst is keyed from the modem's RF TX interrupt (dm1701_rf_dmr_switch)
		int32_t dmr_offset = bw == RADIO_BW_25K ? DM1701_TX_OFFSET_HZ_25K : DM1701_TX_OFFSET_HZ_12K5;
		count(hrc_set_mod_gain(&r.modem, cal.dmr_i, cal.dmr_q));
		r.dmr_tx_hz = (uint32_t)((int32_t)hz + dmr_offset);
		r.dmr_drive = drive;
		r.dmr_uhf = uhf;
		__DMB();
		r.dmr_armed = true;
		r.tx_on = true;
		return true;
	}

	uint16_t tone_value;
	at1846s_tone_t tone = rf_tone(tx_tone, &tone_value);
	int32_t offset = bw == RADIO_BW_25K ? DM1701_TX_OFFSET_HZ_25K : DM1701_TX_OFFSET_HZ_12K5;

	rx_supply(false, false);

	hrc6000_fm_tx_t fm = {
		.uhf = uhf,
		.wide = bw == RADIO_BW_25K,
		.tone = tone == AT1846S_TONE_NONE ? HRC6000_TONE_NONE
			: tone == AT1846S_TONE_CTCSS ? HRC6000_TONE_CTCSS : HRC6000_TONE_DCS,
		.mod2 = cal.fm_i,
		.mod1 = cal.fm_q,
		.ref_offset = cal.ref_offset,
		.mic_gain = FM_MIC_GAIN,
	};
	count(hrc6000_set_mode_fm_tx(&r.modem, &fm));

	count(at1846s_set_bandwidth(&r.rf, rf_bw(bw)));
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(at1846s_set_tx_tone(&r.rf, tone, tone_value));
	count(at1846s_set_freq(&r.rf, (uint32_t)((int32_t)hz + offset)));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_FM_TX));

	apc_set(drive);
	PIN_WRITE(PIN_PA_EN1, true);
	PIN_WRITE(PIN_PA_BAND, uhf);
	PIN_WRITE(PIN_PA_EN2, true);
	r.tx_on = true;
	return true;
}

static void tx_off(void)
{
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();	// after DMR_EV_TX_DONE this only disarms; before it, the transmission is cut
		r.rf_isr_dirty = !r.rx_valid;	// rx() below retunes with forced writes; without it, resync here
		rf_resync();
	}
	pa_off();
	inject_stop();
	if (r.rx_valid) {
		subtone_t tone = r.rx_tone;
		rx(r.rx_hz, r.rx_bw, &tone);
		return;
	}
	count(at1846s_set_tx_tone(&r.rf, AT1846S_TONE_NONE, 0));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	r.tx_on = false;
}

static void status(radio_status_t *st)
{
	if (stm32_hrc_dmr_tx_active() || r.afsk) {
		*st = r.last_status;	// keep I2C3 free for the per-burst switching / the AFSK tone switching
		return;
	}
	uint8_t rssi, noise;
	uint16_t flags, value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	if (!at1846s_read_signal(&r.rf, &rssi, &noise) || !at1846s_read_flags(&r.rf, &flags)) {
		st->rssi_dbm = -140;
		st->noise = 255;
		st->carrier = false;
		st->tone_ok = type == AT1846S_TONE_NONE;
		return;
	}
	r.diag.rssi = rssi;
	r.diag.noise = noise;
	r.diag.flags = flags;
	// Field calibration constants (hardware-dm1701.md 5.9)
	st->rssi_dbm = r.rx_uhf ? (int16_t)(rssi - 151) : (int16_t)(-164 + rssi * 32 / 27);
	st->noise = noise;
	st->carrier = (flags & AT1846S_FLAG_SQ) != 0;
	st->tone_ok = at1846s_tone_match(flags, type);
	r.last_status = *st;
}

// Soft volume (hardware-dm1701.md 11.2, 17): the knob's 0..100 % maps linearly to -31..+31 dB of HR_C6000 RX gain
static void volume(uint8_t percent)
{
	// At the bottom the amp is off (speaker_apply), so the gain stays at unity: the I2S copy of the FM audio that
	// APRS receive reads is scaled by it too
	int db = percent == 0 ? 0 : (int)percent * 62 / 100 - 31;
	count(hrc6000_set_rx_gain(&r.modem, (int8_t)db));
	if ((percent == 0) != volume_zero) {
		volume_zero = percent == 0;
		speaker_apply();
	}
}

// Tone deviation, reg 0x59 bits 15:6, for the 1750 Hz burst, CW ID and APRS AFSK (at its default level 50): about
// 3.1 kHz peak, about linear in the field. HIL 2026-10-10: 0x40 measured 7.2 kHz peak on air (SDR bench), too wide
#define TONE_DEV	0x1Cu

// DTMF deviation, reg 0x59 bits 15:6, for the two tones together (radio-features.md 3.7)
#define DTMF_DEV	0x1Cu

// FM TX tone (hardware-dm1701.md 5.8): the AT1846S tone generator as the voice source with the HR_C6000 microphone
// input muted (4:0xE0 bit 6 cleared). The AT1846S modulates it as it does its CTCSS (seen on the SDR bench) once
// The DCS turn-off tail (hardware-dm1701.md 5.7); the core holds the carrier for it. The sub-audio selection
// (reg 0x4E), apart from the tone generator tone_start() uses
static bool tx_tail(bool on)
{
	if (!r.tx_on || r.mode != RADIO_MODE_FM) {
		return false;
	}
	return at1846s_tx_tail(&r.rf, on);
}

// voice_sel selects tone1; with voice_sel left at "nothing" the carrier was unmodulated (gotchas 3.10). high != 0:
// tone2 at high as well, voice_sel = both tones (a DTMF pair). Only during FM TX; the next tx() / rx() reapplies the
// mode tables
static bool tone_start(uint16_t hz, uint16_t high, uint16_t dev)
{
	if (!r.tx_on || r.mode != RADIO_MODE_FM) {
		return false;
	}
	if (hz != 0) {
		uint8_t e0;
		bool ok = hrc_read(&r.modem, 4, 0xE0, &e0);
		if (r.tone_hz == 0) {
			uint16_t v;
			r.saved_59 = at1846s_read(&r.rf, 0, 0x59, &v) ? v : 0;
			r.saved_79 = at1846s_read(&r.rf, 0, 0x79, &v) ? v : 0;
			r.saved_3a = at1846s_read(&r.rf, 0, 0x3A, &v) ? v : 0;
			r.saved_7a = at1846s_read(&r.rf, 0, 0x7A, &v) ? v : 0;
		}
		if (ok) {
			count(hrc_write(&r.modem, 4, 0xE0, (uint8_t)(e0 & ~0x40u)));	// microphone muted
		}
		count(at1846s_write(&r.rf, 0, 0x35, (uint16_t)(hz * 10u)));		// tone1 = Hz x 10
		if (high != 0) {
			count(at1846s_write(&r.rf, 0, 0x36, (uint16_t)(high * 10u)));	// tone2 = Hz x 10
		}
		// Single tone mode (0x79 bits 15:14: direct, tone generator; 0x7A bit 14) sends tone1 and tone2 for as
		// long as it is on, where the chip's DTMF mode (0x7A bit 15) would time each digit itself
		count(at1846s_write(&r.rf, 0, 0x79, 0xC000));
		count(at1846s_write(&r.rf, 0, 0x59, (uint16_t)((dev << 6) | (r.saved_59 & 0x3Fu))));	// tone deviation
		count(at1846s_modify(&r.rf, 0, 0x7A, 0xBFFF, 0x4000));			// single tone enable
		// voice_sel, 0x3A bits 14:12 = 001: TX the tone1 frequency, 011: tone1 and tone2 (AT1846S programming guide,
		// TX voice channel select). Bits 6:4 are squelch and sub-audio detect enables, not the voice source: writing
		// the tone there left voice_sel at 000 (TX nothing), an unmodulated carrier
		count(at1846s_modify(&r.rf, 0, 0x3A, 0x8FFF, high != 0 ? 0x3000 : 0x1000));
		r.tone_hz = hz;
	} else {
		tone_restore();
	}
	return true;
}

static bool tx_tone(uint16_t hz)
{
	return tone_start(hz, 0, TONE_DEV);
}

#if CONFIG_DTMF
// DTMF (radio-features.md 3.7): tone1 the low tone, tone2 the high one. A new digit while one sounds only retunes
static bool tx_dtmf(char digit)
{
	uint16_t low = 0, high = 0;
	if (digit != 0 && !dtmf_pair(digit, &low, &high)) {
		return false;
	}
	return tone_start(low, high, DTMF_DEV);
}
#endif

// Morse keying of the FM TX tone (hardware-dm1701.md 5.8): the tone deviation (reg 0x59 bits 15:6) to 0 for key up,
// back to the tone value for key down. The tone generator stays the voice source and the HR_C6000 microphone input
// stays muted, so the gaps are silent. Unverified on the bench, like tx_tone
static bool tx_tone_key(bool down)
{
	if (!r.tx_on || r.mode != RADIO_MODE_FM || r.tone_hz == 0) {
		return false;
	}
	uint16_t dev = down ? TONE_DEV : 0x00u;
	count(at1846s_write(&r.rf, 0, 0x59, (uint16_t)((dev << 6) | (r.saved_59 & 0x3Fu))));
	return true;
}

#if CONFIG_APRS
// APRS AFSK (afsk_dm1701.c, docs/spec/aprs.md 5.2): the FM TX tone generator as for tx_tone, at hz with the deviation
// from level (50 = the tone deviation tx_tone uses), then the TIM6 interrupt retunes tone1 (reg 0x35) every symbol
// with single I2C3 writes. status() keeps off I2C3 meanwhile. Unverified on the bench, like tx_tone
bool dm1701_afsk_begin(uint16_t hz, uint8_t level)
{
	if (!tx_tone(hz)) {
		return false;
	}
	uint32_t dev = (TONE_DEV * level + 25u) / 50u;
	if (dev > 0x3FFu) {
		dev = 0x3FFu;
	}
	count(at1846s_write(&r.rf, 0, 0x59, (uint16_t)((dev << 6) | (r.saved_59 & 0x3Fu))));
	r.afsk = true;		// the last writes were on page 0, as rf_isr_write needs
	return true;
}

// From the TIM6 interrupt: tone1 to hz. False when the write didn't happen
bool dm1701_afsk_tone(uint16_t hz)
{
	return r.afsk && rf_isr_write(0x35, (uint16_t)(hz * 10u));
}

void dm1701_afsk_end(void)
{
	if (r.afsk) {
		r.afsk = false;
		count(at1846s_write_force(&r.rf, 0, 0x35, (uint16_t)(r.tone_hz * 10u)));	// the interrupt wrote it behind
											// the driver's cache
		tone_restore();
	}
}
#endif

// Receiver battery saving (FM receive only): the AT1846S idle and the RX supply off; waking retunes with rx()
static void rx_sleep(bool sleep)
{
	if (r.mode != RADIO_MODE_FM || r.tx_on || !r.rx_valid || sleep == r.asleep) {
		return;
	}
	if (sleep) {
		count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
		rx_supply(false, false);
		r.asleep = true;
	} else {
		subtone_t tone = r.rx_tone;
		rx(r.rx_hz, r.rx_bw, &tone);
	}
}

const hal_radio_t dm1701_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.volume = volume,
	.tx_tone = tx_tone,
	.tx_tail = tx_tail,
	.tx_tone_key = tx_tone_key,
#if CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
	.rx_sleep = rx_sleep,
};

void dm1701_radio_diag(dm1701_radio_diag_t *d)
{
	*d = r.diag;
	d->bus_errors = r.bus_errors;
}

#if CONFIG_AUDIO_PROBE
// The audio probe's register access (hal_audio_tap_t.modem_reg, audio_dm1701.c): one HR_C6000 register, any task.
// Each access is one masked transaction (modem_transfer), so it can't tear a driver's; it can of course change state
// under the driver's feet, which is what the probe is for
bool dm1701_modem_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write)
{
	return write ? hrc_write(&r.modem, page, reg, *val) : hrc_read(&r.modem, page, reg, val);
}

// RAM access (hal_audio_tap_t.modem_ram), in 64-byte transactions so interrupts are masked for well under a
// millisecond at a time
bool dm1701_modem_ram(uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write)
{
	bool ok = true;
	while (ok && len > 0) {
		uint16_t n = len > 64u ? 64u : len;
		ok = hrc_ram(&r.modem, page, addr, buf, n, write);
		addr = (uint16_t)(addr + n);
		buf += n;
		len = (uint16_t)(len - n);
	}
	return ok;
}

// A burst write over the config port (hal_audio_tap_t.modem_burst): page 3 goes to the chip's prompt tone FIFO here,
// not through the voice port as the vocoder's frames do
bool dm1701_modem_burst(uint8_t page, uint8_t reg, const uint8_t *buf, uint16_t len)
{
	return hrc_write_burst(&r.modem, page, reg, buf, len);
}
#endif
