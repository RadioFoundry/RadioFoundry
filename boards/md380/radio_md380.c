// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 radio: SKY72310 synthesizer + GT3136 receiver + HR_C5000 (the HR_C6000 driver in its C5000 variant) + RF path
// GPIOs + the two DACs + calibration (docs/spec/hardware-md380.md 4-8, 11). Nothing here has run on a radio.
//
// Unlike the DM-1701 there is no RF transceiver chip: the MCU tunes the synthesizer itself, selects the receive or
// transmit VCO, switches the receiver and transmitter supplies, and drives the PA power loop from DAC1 (which tunes
// the receiver front end while receiving). The HR_C5000 does the FM and DMR modulation (two-point, MOD1 to the VCO,
// MOD2 to the reference) and takes the receiver's IF for DMR.
//
// The frequency plan (hardware-md380.md 5.3, unconfirmed): a 16.8 MHz TCXO; receive on a 49.95 MHz first IF with the
// UHF LO below the signal and the VHF VCO at twice the LO above it; transmit with the VCO on the carrier (UHF) or at
// twice it (VHF). MD380_IF_HZ, MD380_RX_LO_ABOVE and MD380_VCO_MULT hold the plan for each band.
//
// DMR (modem-hrc6000.md 4.3 with the HR_C5000 notes of hardware-md380.md 4): the synthesizer is moved to the TX
// frequency at the first burst of a transmission (from the RF TX interrupt, without waiting for lock: the interrupt
// comes some milliseconds before the burst) and back with tx_off(); between bursts only the PA supply and loop are
// switched, so no burst waits for the synthesizer.
#include <stddef.h>
#include "config.h"
#include "md380.h"
#include "pins.h"
#include "hal_time.h"
#include "rf/sky72310/sky72310.h"
#include "modem/hrc6000/hrc6000.h"
#include "modem/hrc6000/hrc6000_dmr.h"
#include "dmr_md380.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// Frequency plan (hardware-md380.md 5.3, all ?)
#ifndef MD380_TCXO_HZ
#define MD380_TCXO_HZ		16800000u
#endif
#ifndef MD380_IF_HZ
#define MD380_IF_HZ		49950000u
#endif
#if MD380_VHF
#define MD380_RX_LO_ABOVE	1	// LO = f + IF
#define MD380_VCO_MULT		2u	// the VCO runs at twice the LO / carrier
#else
#define MD380_RX_LO_ABOVE	0	// LO = f - IF
#define MD380_VCO_MULT		1u
#endif

// Measured TX carrier correction (Hz, added to the TX frequency): none known
#define MD380_TX_OFFSET_HZ	0

#define DAC_MAX			4095u
#define PLL_PD_GAIN		31u	// phase detector gain, the data sheet's largest (1 mA/rad): a guess (?)
#define PLL_LOCK_US		5000u	// task-level wait for lock detect after a retune
#define FM_MIC_GAIN		7
#define DMR_MIC_GAIN		7
#define C5000_PWD_WAIT_MS	12	// as the HR_C6000: at least 10 ms after PWD low before the first access

// Voice SPI2 (hardware-md380.md 4.4): PB13 SCK, PB14 MISO, PB15 MOSI (AF5), chip select PIN_C5000_VCS
#define VOICE_SPI_PINS		(GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15)
#define VOICE_SPI_TIMEOUT_MS	2

// RSSI and noise from the ADC (hardware-md380.md 5.5): placeholders until a bench measurement gives the curves
#define RSSI_FLOOR_DBM		(-130)
#define RSSI_SPAN_DB		90	// full ADC scale above the floor (?)
#define CARRIER_DBM		(-118)

static struct {
	sky72310_t pll;
	hrc6000_t modem;
	radio_mode_t mode;
	bool tx_on;
	bool rx_valid;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	bool asleep;
	md380_radio_diag_t diag;
	uint32_t bus_errors;

	// DMR per-burst keying (shared with the modem ISRs)
	volatile bool dmr_armed;	// tx() in DMR mode: TX frequency and drive known
	volatile bool dmr_tx_tuned;	// the synthesizer and the HR_C5000 are on the transmit side
	volatile uint32_t dmr_tx_hz;
	volatile uint32_t dmr_drive;
	uint8_t dmr_mod2, dmr_mod1;
	uint32_t rf_skipped, rf_unarmed;
} r;

static DAC_HandleTypeDef dac;
static SPI_HandleTypeDef spi2;

static void count(bool ok)
{
	if (!ok) {
		r.bus_errors++;
	}
}

// --- The shared three-wire bus: HR_C5000 configuration port and SKY72310 (hardware-md380.md 4.2, 5.2) ---

uint32_t md380_rfbus_lock(void)
{
	return portSET_INTERRUPT_MASK_FROM_ISR();
}

void md380_rfbus_unlock(uint32_t mask)
{
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

// About 110 ns at 72 MHz on top of the GPIO writes
static inline void bus_delay(void)
{
	__NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
}

// Clock idles high; data set, clock low, sample MISO, clock high (both chips latch on the rising edge, ?)
static uint8_t bus_byte(uint8_t out)
{
	uint8_t in = 0;
	for (int i = 0; i < 8; i++) {
		PIN_WRITE(PIN_C5000_MOSI, (out & 0x80u) != 0);
		out = (uint8_t)(out << 1);
		PIN_WRITE(PIN_C5000_CLK, false);
		bus_delay();
		in = (uint8_t)((in << 1) | (PIN_READ(PIN_C5000_MISO) ? 1u : 0u));
		PIN_WRITE(PIN_C5000_CLK, true);
		bus_delay();
	}
	return in;
}

static bool modem_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_C5000_CLK, true);
	PIN_WRITE(PIN_C5000_CS, false);
	bus_delay();
	for (uint8_t i = 0; i < hdr_len; i++) {
		bus_byte(hdr[i]);
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t in = bus_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = in;
		}
	}
	bus_delay();
	PIN_WRITE(PIN_C5000_CS, true);
	md380_rfbus_unlock(mask);
	return true;
}

// One 16-bit synthesizer word, latched when its chip select goes high
static bool pll_write(void *ctx, uint16_t word)
{
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_C5000_CLK, true);
	PIN_WRITE(PIN_PLL_CS, false);
	bus_delay();
	bus_byte((uint8_t)(word >> 8));
	bus_byte((uint8_t)word);
	bus_delay();
	PIN_WRITE(PIN_PLL_CS, true);
	md380_rfbus_unlock(mask);
	return true;
}

static bool pll_locked(void *ctx)
{
	return PIN_READ(PIN_PLL_LD);
}

static const sky72310_bus_t pll_bus = { .write = pll_write, .locked = pll_locked, .delay_us = hal_delay_us };

// Voice port: STM32 SPI2 master, mode 2, prescaler 8 (4.5 MHz), as the DM-1701's HR_C6000 (?)
static void voice_spi_setup(void)
{
	__HAL_RCC_SPI2_CLK_ENABLE();
	stm32_pin_af(GPIOB, VOICE_SPI_PINS, GPIO_AF5_SPI2, false, GPIO_NOPULL);
	PIN_WRITE(PIN_C5000_VCS, true);
	spi2.Instance = SPI2;
	spi2.Init.Mode = SPI_MODE_MASTER;
	spi2.Init.Direction = SPI_DIRECTION_2LINES;
	spi2.Init.DataSize = SPI_DATASIZE_8BIT;
	spi2.Init.CLKPolarity = SPI_POLARITY_HIGH;
	spi2.Init.CLKPhase = SPI_PHASE_1EDGE;
	spi2.Init.NSS = SPI_NSS_SOFT;
	spi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
	spi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
	spi2.Init.TIMode = SPI_TIMODE_DISABLE;
	spi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	spi2.Init.CRCPolynomial = 7;
	HAL_SPI_Init(&spi2);
}

// One voice frame: 0x03 / 0x83, 0x00, then the data. Atomic with respect to the modem ISRs
static bool voice_xfer(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	uint8_t hdr[2] = { write ? 0x03 : 0x83, 0x00 };
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_C5000_VCS, false);
	bool ok = HAL_SPI_Transmit(&spi2, hdr, 2, VOICE_SPI_TIMEOUT_MS) == HAL_OK;
	if (ok) {
		ok = write ? HAL_SPI_Transmit(&spi2, buf, len, VOICE_SPI_TIMEOUT_MS) == HAL_OK
			   : HAL_SPI_Receive(&spi2, buf, len, VOICE_SPI_TIMEOUT_MS) == HAL_OK;
	}
	PIN_WRITE(PIN_C5000_VCS, true);
	md380_rfbus_unlock(mask);
	return ok;
}

static const hrc6000_bus_t modem_bus = {
	.transfer = modem_transfer, .delay_ms = hal_delay_ms, .delay_us = hal_delay_us, .ctx = NULL,
	.voice = voice_xfer,
};

// --- DACs and RF path pins (hardware-md380.md 5.4, 5.6) ---

static void dac_setup(void)
{
	__HAL_RCC_DAC_CLK_ENABLE();
	stm32_pin_analog(PIN_PORT(PIN_APC_DAC), PIN_MASK(PIN_APC_DAC) | PIN_MASK(PIN_REF_DAC));
	dac.Instance = DAC1;
	HAL_DAC_Init(&dac);
	DAC_ChannelConfTypeDef ch = { .DAC_Trigger = DAC_TRIGGER_NONE, .DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE };
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_1);
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_2);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, hrc6000_c5000_ref_dac(0x80));
	HAL_DAC_Start(&dac, DAC_CHANNEL_1);
	HAL_DAC_Start(&dac, DAC_CHANNEL_2);
}

// DAC1: the PA power reference while transmitting, the receiver front-end tuning voltage while receiving. Direct
// register write so the modem ISRs can use it
static void dac1_set(uint32_t value)
{
	DAC->DHR12R1 = value > DAC_MAX ? DAC_MAX : value;
}

static void dac2_set(uint32_t value)
{
	DAC->DHR12R2 = value > DAC_MAX ? DAC_MAX : value;
}

// PA loop and transmitter supply off; DAC1 left at 0 until the receiver takes it
static void pa_off(void)
{
	PIN_WRITE(PIN_APC_EN, false);
	PIN_WRITE(PIN_TX_5V, false);
	dac1_set(0);
}

static void pa_on(uint32_t drive)
{
	dac1_set(drive);
	PIN_WRITE(PIN_TX_5V, true);
	PIN_WRITE(PIN_APC_EN, true);
}

static void set_wide(bool wide)
{
#ifndef MD380_KEEP_SWD
	PIN_WRITE(PIN_WIDE, wide);	// polarity ?
#else
	(void)wide;
#endif
}

// --- Synthesizer ---

static uint32_t rx_vco_hz(uint32_t hz)
{
	uint32_t lo = MD380_RX_LO_ABOVE ? hz + MD380_IF_HZ : hz - MD380_IF_HZ;
	return lo * MD380_VCO_MULT;
}

static uint32_t tx_vco_hz(uint32_t hz)
{
	return (uint32_t)((int32_t)hz + MD380_TX_OFFSET_HZ) * MD380_VCO_MULT;
}

// The smallest reference divider that keeps the divide ratio in the fractional range (finest resolution, fastest
// loop): at 16.8 MHz and a VCO of 270-480 MHz that is 2 or 3
static uint8_t pick_ref_div(uint32_t vco_hz)
{
	sky72310_ratio_t ratio;
	for (uint8_t d = 1; d <= 32; d++) {
		if (sky72310_ratio(MD380_TCXO_HZ, d, vco_hz, &ratio)) {
			return d;
		}
	}
	return 0;
}

// Retune; tx selects the transmit VCO. wait: task level, wait for lock detect
static bool pll_tune(uint32_t vco_hz, bool tx, bool wait)
{
	uint8_t d = pick_ref_div(vco_hz);
	PIN_WRITE(PIN_VCO_SEL, tx);	// polarity ?
	if (d == 0 || !sky72310_set_vco(&r.pll, d, vco_hz)) {
		r.diag.pll_fails++;
		r.diag.pll_locked = false;
		return false;
	}
	r.diag.pll_vco_hz = vco_hz;
	r.diag.pll_locked = wait ? sky72310_wait_lock(&r.pll, PLL_LOCK_US) : PIN_READ(PIN_PLL_LD);
	if (wait && !r.diag.pll_locked) {
		r.diag.pll_fails++;
	}
	return !wait || r.diag.pll_locked;
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

// Receiver on for the current mode: supply, IF to the HR_C5000, FM or DMR audio path
static void rx_path(bool on)
{
	PIN_WRITE(PIN_RX_5V, on);
	PIN_WRITE(PIN_DMR_SW, on);			// high: receiver IF and audio to the HR_C5000
	PIN_WRITE(PIN_FM_SW, on && r.mode == RADIO_MODE_FM);	// polarity ?
}

// --- DMR per-burst RF switching: modem ISR context ---

bool md380_rf_dmr_switch(bool tx)
{
	if (!tx) {
		pa_off();
		return true;
	}
	if (!r.dmr_armed) {
		r.rf_unarmed++;
		return false;
	}
	if (!r.dmr_tx_tuned) {
		// First burst: receiver off, the HR_C5000's transmit set, the synthesizer to the transmit VCO
		rx_path(false);
		count(hrc6000_c5000_dmr_path(&r.modem, true, r.dmr_mod2, r.dmr_mod1, DMR_MIC_GAIN));
		if (!pll_tune(tx_vco_hz(r.dmr_tx_hz), true, false)) {
			r.rf_skipped++;
			return false;	// out of range: no RF
		}
		r.dmr_tx_tuned = true;
	}
	pa_on(r.dmr_drive);
	return true;
}

void md380_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed)
{
	*skipped = r.rf_skipped;
	*unarmed = r.rf_unarmed;
}

// Stop per-burst keying (task level): disarm, drop a transmission still running, PA off
static void dmr_disarm(void)
{
	r.dmr_armed = false;
	if (md380_dmr_tx_active()) {
		md380_dmr_tx_drop();
	}
	pa_off();
}

static void dmr_enter(void)
{
	md380_cal_point_t cal;
	md380_cal_at(r.rx_valid ? r.rx_hz : board.bands[0].lo_hz, &cal);	// on false it fills safe defaults
#if CONFIG_DMR
	count(hrc6000_dmr_enter(&r.modem, cal.dmr_i, cal.dmr_q, DMR_MIC_GAIN));
	count(hrc6000_c5000_dmr_path(&r.modem, false, cal.dmr_i, cal.dmr_q, DMR_MIC_GAIN));
#endif
	md380_dmr_enter();
}

// --- Speaker (hardware-md380.md 6) ---
//
// FM receive audio and DMR voice PCM both reach the amplifier through the HR_C5000 (its FM feed-through, 0x36, or its
// codec DAC), with FM_MUTE in front of the FM path (?). The volume is the analog knob after the amplifier input.
static bool audio_wanted;
static bool pcm_wanted;
static SemaphoreHandle_t speaker_lock;

static void pcm_codec_on(void)
{
	uint8_t v;
	if (hrc6000_read(&r.modem, 4, 0xE2, &v) && (v & 0x02u) == 0) {
		count(hrc6000_write(&r.modem, 4, 0xE2, (uint8_t)(v | 0x02u)));
	}
	if (r.mode == RADIO_MODE_DMR) {
		count(hrc6000_write(&r.modem, 4, 0xE0, 0xC9));
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
	if (fm || pcm) {
		count(hrc6000_fm_audio(&r.modem, fm));
		if (pcm) {
			pcm_codec_on();
		}
		PIN_WRITE(PIN_FM_MUTE, !fm);
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		PIN_WRITE(PIN_SPK_MUTE, true);
		PIN_WRITE(PIN_AMP_EN, false);
		PIN_WRITE(PIN_FM_MUTE, true);
		count(hrc6000_fm_audio(&r.modem, false));
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
void md380_speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	speaker_apply();
}
#endif

// --- hal_radio_t ---

static void init(void)
{
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
	}
	stm32_pin_in(PIN_PORT(PIN_C5000_MISO), PIN_MASK(PIN_C5000_MISO), GPIO_PULLUP);
	stm32_pin_in(PIN_PORT(PIN_PLL_LD), PIN_MASK(PIN_PLL_LD), GPIO_NOPULL);
	PIN_WRITE(PIN_C5000_CS, true);
	PIN_WRITE(PIN_PLL_CS, true);
	PIN_WRITE(PIN_C5000_CLK, true);
#ifndef MD380_KEEP_SWD
	// The SWD pins become the IF bandwidth switch and the microphone supply (hardware-md380.md 2.6)
	stm32_pin_out(PIN_PORT(PIN_WIDE), PIN_MASK(PIN_WIDE), false, false);
	stm32_pin_out(PIN_PORT(PIN_MIC_POWER), PIN_MASK(PIN_MIC_POWER), true, false);
#endif
	rx_path(false);
	dac_setup();
	pa_off();
	voice_spi_setup();

	PIN_WRITE(PIN_C5000_PWD, false);
	uint32_t pwd_ms = hal_millis();

	r.pll.mux = 0;
	sky72310_attach(&r.pll, &pll_bus, MD380_TCXO_HZ, PLL_PD_GAIN);
	count(sky72310_init(&r.pll));

	while ((uint32_t)(hal_millis() - pwd_ms) < C5000_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc6000_attach_chip(&r.modem, &modem_bus, HRC6000_CHIP_C5000);
	count(hrc6000_power_on_init(&r.modem, DMR_MIC_GAIN));

	r.mode = RADIO_MODE_FM;
	count(hrc6000_set_mode_fm_rx(&r.modem));
	audio(false);
	r.diag.c5000_ok = hrc6000_read(&r.modem, 4, 0x10, &r.diag.c5000_r10);
	md380_dmr_board_init(&r.modem);
}

static void set_mode(radio_mode_t mode)
{
	radio_mode_t prev = r.mode;
	r.mode = mode;
	if (mode == RADIO_MODE_DMR) {
		if (prev != RADIO_MODE_DMR) {
			dmr_enter();
		}
	} else if (prev == RADIO_MODE_DMR) {
		dmr_disarm();
		md380_dmr_leave();
		r.dmr_tx_tuned = false;
		r.tx_on = false;
		count(hrc6000_set_mode_fm_rx(&r.modem));
	}
	if (r.rx_valid && !r.tx_on && !r.asleep) {
		rx_path(true);	// FM_SW follows the mode
	}
}

// Enter RX: PA off, synthesizer to the receive VCO, receiver supply, front-end tuning, reference, HR_C5000
static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	(void)rx_tone;	// no sub-audio decoding on this board yet (config.mk CONFIG_SUBTONE)
	if (find_band(hz) == NULL) {
		return false;
	}
	bool same = r.rx_valid && hz == r.rx_hz && bw == r.rx_bw;
	bool from_tx = r.dmr_tx_tuned;
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();
	}
	r.dmr_tx_tuned = false;
	r.asleep = false;
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_valid = true;

	pa_off();
	set_wide(bw == RADIO_BW_25K);
	(void)pll_tune(rx_vco_hz(hz), false, true);	// a failure shows on the diagnostics page
	rx_path(true);

	md380_cal_point_t cal;
	md380_cal_at(hz, &cal);	// on false it fills safe defaults
	dac1_set(cal.rx_tune_dac);
	dac2_set(hrc6000_c5000_ref_dac(cal.ref_tune));
	count(hrc6000_c5000_set_ref(&r.modem, cal.ref_tune));
	if (r.mode == RADIO_MODE_FM) {
		count(hrc6000_set_mode_fm_rx(&r.modem));
	} else {
		if (from_tx || !same) {
			count(hrc6000_c5000_dmr_path(&r.modem, false, cal.dmr_i, cal.dmr_q, DMR_MIC_GAIN));
		}
		if (!same) {
			count(hrc6000_set_mod_gain(&r.modem, cal.dmr_i, cal.dmr_q));
			md380_dmr_rx_restart();
		}
	}
	r.tx_on = false;
	return true;
}

// Enter FM TX: receiver off, HR_C5000 FM TX, synthesizer to the transmit VCO and locked, then the PA. Refused
// without an accepted calibration table (the APC values would be guesses)
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)tx_tone;	// no sub-audio encoder yet
	const band_t *band = find_band(hz);
	if (band == NULL || !band->tx) {
		return false;
	}
	if (board.power_count > 0 && power >= board.power_count) {
		power = (uint8_t)(board.power_count - 1);
	}
	md380_cal_point_t cal;
	if (!md380_cal_at(hz, &cal) || !cal.valid) {
		return false;
	}
	uint32_t drive = cal.power_dac[power > 1 ? 1 : power];

	if (r.mode == RADIO_MODE_DMR) {
		// Arm only: the receiver stays on the RX frequency until the first burst (md380_rf_dmr_switch)
		r.dmr_tx_hz = hz;
		r.dmr_drive = drive;
		r.dmr_mod2 = cal.dmr_i;
		r.dmr_mod1 = cal.dmr_q;
		__DMB();
		r.dmr_armed = true;
		r.tx_on = true;
		return true;
	}

	rx_path(false);
	hrc6000_fm_tx_t fm = {
		.uhf = !MD380_VHF,
		.wide = bw == RADIO_BW_25K,
		.tone = HRC6000_TONE_NONE,
		.mod2 = cal.fm_i,
		.mod1 = cal.fm_q,
		.ref_offset = (int8_t)((int)cal.ref_tune - 128),
		.mic_gain = FM_MIC_GAIN,
	};
	count(hrc6000_set_mode_fm_tx(&r.modem, &fm));
	count(hrc6000_c5000_set_ref(&r.modem, cal.ref_tune));	// the C5000's own reference registers (4.5)
	set_wide(bw == RADIO_BW_25K);
	if (!pll_tune(tx_vco_hz(hz), true, true)) {
		// Not locked: no carrier off frequency. Back to receive
		r.tx_on = false;
		rx(r.rx_hz, r.rx_bw, NULL);
		return false;
	}
	pa_on(drive);
	r.tx_on = true;
	return true;
}

static void tx_off(void)
{
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();	// after DMR_EV_TX_DONE this only disarms; before it, the transmission is cut
	}
	pa_off();
	if (r.rx_valid) {
		rx(r.rx_hz, r.rx_bw, NULL);
		return;
	}
	r.dmr_tx_tuned = false;
	count(hrc6000_set_mode_fm_rx(&r.modem));
	r.tx_on = false;
}

static void status(radio_status_t *st)
{
	uint32_t rssi = md380_adc_raw(MD380_ADC_RSSI);
	uint32_t busy = md380_adc_raw(MD380_ADC_BUSY);
	st->rssi_dbm = (int16_t)(RSSI_FLOOR_DBM + (int32_t)(rssi * RSSI_SPAN_DB / 4095u));
	st->noise = (uint8_t)(busy >> 4);	// noise detector level, larger = more noise (?)
	st->carrier = st->rssi_dbm >= CARRIER_DBM;
	st->tone_ok = true;
}

// Receiver battery saving (FM receive only): receiver supply off and the synthesizer powered down
static void rx_sleep(bool sleep)
{
	if (r.mode != RADIO_MODE_FM || r.tx_on || !r.rx_valid || sleep == r.asleep) {
		return;
	}
	if (sleep) {
		rx_path(false);
		count(sky72310_power_down(&r.pll, true));
		r.asleep = true;
	} else {
		count(sky72310_power_down(&r.pll, false));
		rx(r.rx_hz, r.rx_bw, NULL);
	}
}

const hal_radio_t md380_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
};

void md380_radio_diag(md380_radio_diag_t *d)
{
	*d = r.diag;
	d->bus_errors = r.bus_errors;
}
