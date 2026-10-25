// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 radio: the AT1846S (I2C bus A) with the radio's own register profile, the HR_C6000 (SPI0) as the FM audio chain
// and DMR modem, the RF path lines, DAC0 and the calibration (docs/spec/hardware-gd77.md 5, 8). The structure
// of boards/dm1701/radio_dm1701.c with this radio's wiring.
//
// Switch sequences (rf_gd77.h): receive is TX enable off, drive ramped down, TX path off, tune, the AT1846S to
// receive, the band's RX path on, the receive DAC value. Transmit is the RX path off, tune, the AT1846S to transmit,
// the band's TX path on, TX enable, drive ramped up. In DMR, tx() only arms: the modem's RF TX interrupt keys each
// burst and the RF RX interrupt ends it (gd77_rf_dmr_switch, from dmr_gd77.c), with single AT1846S writes from the
// interrupt that the task-level driver resyncs after the transmission.
//
// Transmit is built only with CONFIG_GD77_TX=y (off by default, boards/gd77/config.mk), and even then tx() refuses
// without a valid calibration point for the frequency (cal_gd77.h). Receive applies the point's AT1846S receive values
// on every retune. Every pin role and value here is unverified on a radio. The RSSI scale is
// boards/dm1701/radio_dm1701.c's.
#include <stddef.h>
#include "gd77.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/at1846s/at1846s.h"
#include "modem/hrc6000/hrc6000.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "rf_gd77.h"
#include "cal_gd77.h"
#include "c6k_gd77.h"
#include "dmr_gd77.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define AT1846S_ADDR		0x71u	// 7-bit (0xE2 / 0xE3 on the wire)
#define C6K_PWD_HIGH_MS		10u	// the power-down line's high pulse at start-up
#define C6K_PWD_WAIT_MS		10u	// PWD low to the first configuration access (modem-hrc6000.md 1)
#define FM_MIC_GAIN		7u	// 0..31
#define DMR_MIC_GAIN		7u
#define C6K_MOD_FALLBACK	0x80u	// 4:0x45 / 4:0x46 when the power-on values can't be read

_Static_assert(AT1846S_BW_12K5 == 0 && AT1846S_BW_25K == 1 && GD77_CAL_BWS == 2, "cal_gd77.h bandwidth index");

// The RF path lines by gd77_rf_band_t index (rf_gd77.h)
static const uint8_t rf_line[GD77_LINE_COUNT] = {
	[GD77_LINE_TX_UHF] = PIN_RF_SW1,	// PTE2
	[GD77_LINE_TX_VHF] = PIN_RF_SW2,	// PTE3
	[GD77_LINE_RX_UHF] = PIN_RF_SW3,	// PTC15
	[GD77_LINE_RX_VHF] = PIN_RF_SW4,	// PTC13
};

static struct {
	at1846s_t rf;
	hrc_t modem;
	radio_mode_t mode;
	bool tx_on;			// FM TX keyed, or DMR TX armed
	bool rx_valid, asleep;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	uint8_t drive_now;		// DAC0 now, in the 8-bit drive unit
	// HR_C6000 4:0x45 / 4:0x46 for every write of the pair. 4:0x45 has no calibration and keeps the value the chip
	// has after power-on; 4:0x46 starts the same and takes the calibration's value at each transmission
	uint8_t c6k_mod2, c6k_mod1;
	gd77_radio_diag_t diag;
	radio_status_t last_status;	// returned without bus traffic while DMR transmits

	// DMR per-burst keying (shared with the modem interrupts)
	volatile bool dmr_armed;
	volatile bool dmr_keyed;	// the drive is up for a burst
	volatile uint32_t dmr_tx_hz;
	volatile uint8_t dmr_drive;
	volatile bool rf_isr_dirty;	// the interrupt wrote the AT1846S behind the task-level driver
	volatile bool rf_rx_pending;	// a back-to-receive switch was skipped: retry at the next timeslot interrupt
	volatile bool rf_rx_done;	// the interrupt path has put the receiver back since the last burst
	volatile uint8_t rf_page;	// AT1846S page last written to reg 0x7F, 0xFF unknown
	uint32_t rf_skipped, rf_unarmed;
} r = { .rf_page = 0xFF };

static void count(bool ok)
{
	if (!ok) {
		r.diag.rf_errors++;
	}
}

// --- AT1846S bus: 8-bit register address, then 16 bits high byte first ---

static bool rf_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	if (reg == 0x7F) {
		r.rf_page = 0xFF;	// unknown until the write is through (the interrupt path writes page 0 only)
	}
	bool ok = i2c_a_write(AT1846S_ADDR, b, sizeof b);
	if (ok && reg == 0x7F) {
		r.rf_page = (uint8_t)val;
	}
	return ok;
}

static bool rf_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	uint8_t b[2];
	if (!i2c_a_write_read(AT1846S_ADDR, &reg, 1, b, sizeof b)) {
		return false;
	}
	*val = (uint16_t)((b[0] << 8) | b[1]);
	return true;
}

static const at1846s_bus_t rf_bus = { .write = rf_write, .read = rf_read, .delay_ms = hal_delay_ms, .ctx = NULL };

static const hrc_bus_t modem_bus = {
	.transfer = gd77_c6k_transfer,
	.delay_ms = hal_delay_ms,
	.delay_us = hal_delay_us,
	.ctx = NULL,
#if CONFIG_DMR && GD77_VSPI_ON_SPI1
	.voice = gd77_vspi_xfer,	// V_SPI on SPI1 (dmr_gd77.c)
#else
	.voice = NULL,			// page 3 over the configuration port
#endif
};

// --- Helpers ---

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

// DMR runs on the 12.5 kHz profile whatever the channel says
static at1846s_bw_t rf_bw(radio_bw_t bw)
{
	return bw == RADIO_BW_25K && r.mode != RADIO_MODE_DMR ? AT1846S_BW_25K : AT1846S_BW_12K5;
}

static void apply_rx_tone(void)
{
	uint16_t value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	count(at1846s_set_rx_tone(&r.rf, type, value));
}

// --- RF path lines and DAC0 ---

static void lines_off(bool tx)
{
	k22f_pin_write(rf_line[tx ? GD77_LINE_TX_UHF : GD77_LINE_RX_UHF], false);
	k22f_pin_write(rf_line[tx ? GD77_LINE_TX_VHF : GD77_LINE_RX_VHF], false);
}

static void drive_set(uint8_t drive)
{
	k22f_dac_write((uint16_t)((uint16_t)drive << GD77_DAC_SHIFT));
	r.drive_now = drive;
}

// Transmitter off, now (hal/board.h, docs/spec/crash-log.md 8): TX enable low, both bands' TX path lines low, the
// drive DAC to 0. Register writes only, for the fault handlers, the crash capture and the jump to the flash stub. The
// GPIO data registers are never clock gated; DAC0 is written only once its clock runs (a gated write faults)
void board_rf_safe(void)
{
	k22f_pin_write(PIN_TX_EN, false);
	lines_off(true);
	if (SIM_SCGC6 & SIM_SCGC6_DAC0) {
		k22f_dac_write(0);
	}
}

// Busy waits of a few tens of microseconds: callable from the modem interrupts
static void ramp(uint8_t drive, bool up)
{
	for (uint8_t k = 1; k <= GD77_RAMP_STEPS; k++) {
		k22f_dac_write(gd77_ramp_value(drive, k, up));
		hal_delay_us(gd77_ramp_wait_us(k, up));
	}
	drive_set(up ? drive : 0);
}

// One sequence at task level through the AT1846S driver. chip: the transmit state for GD77_RF_CHIP_TX
typedef struct {
	uint32_t hz;
	gd77_rf_band_t band;
	at1846s_state_t chip_tx;
	uint8_t drive;
} seq_t;

static void run_seq(const uint8_t *seq, uint32_t n, const seq_t *c)
{
	for (uint32_t i = 0; i < n; i++) {
		switch (seq[i]) {
		case GD77_RF_RX_PATH_OFF: lines_off(false); break;
		case GD77_RF_RX_PATH_ON:
			k22f_pin_write(rf_line[c->band.rx_other], false);
			k22f_pin_write(rf_line[c->band.rx_on], true);
			break;
		case GD77_RF_TX_PATH_OFF: lines_off(true); break;
		case GD77_RF_TX_PATH_ON: k22f_pin_write(rf_line[c->band.tx_on], true); break;
		case GD77_RF_TUNE: count(at1846s_set_freq(&r.rf, c->hz)); break;
		case GD77_RF_CHIP_TX: count(at1846s_set_state(&r.rf, c->chip_tx)); break;
		case GD77_RF_CHIP_RX: count(at1846s_set_state(&r.rf, AT1846S_STATE_RX)); break;
		case GD77_RF_TX_EN_ON: k22f_pin_write(PIN_TX_EN, true); break;
		case GD77_RF_TX_EN_OFF: k22f_pin_write(PIN_TX_EN, false); break;
		case GD77_RF_RAMP_UP: ramp(c->drive, true); break;
		case GD77_RF_RAMP_DOWN:
			if (r.drive_now != 0) {
				ramp(r.drive_now, false);
			}
			drive_set(0);
			break;
		default: break;
		}
	}
}

#define SEQ(a)	(a), (uint32_t)(sizeof(a) / sizeof((a)[0]))

// --- DMR per-burst RF switching (modem interrupt context) ---

#if CONFIG_DMR
// One AT1846S page 0 register write from a modem interrupt: refused when the chip is on another page or a task-level
// transaction holds the bus. Never touches the task-level driver's cache
static bool rf_isr_write(uint8_t reg, uint16_t val)
{
	if (r.rf_page != 0) {
		return false;
	}
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return i2c_a_write_isr(AT1846S_ADDR, b, sizeof b);
}

static bool rf_isr_tune(uint32_t hz, at1846s_state_t then)
{
	uint32_t word = at1846s_freq_word(hz);
	return rf_isr_write(0x30, at1846s_ctrl_value(&r.rf, AT1846S_BW_12K5, AT1846S_STATE_IDLE)) &&
	       rf_isr_write(0x05, at1846s_reg05_value(&r.rf, hz)) &&
	       rf_isr_write(0x29, (uint16_t)((word >> 16) & 0x3FFFu)) &&
	       rf_isr_write(0x2A, (uint16_t)(word & 0xFFFFu)) &&
	       rf_isr_write(0x30, at1846s_ctrl_value(&r.rf, AT1846S_BW_12K5, then));
}

bool gd77_rf_dmr_switch(bool tx)
{
	gd77_rf_band_t rx_band = gd77_rf_band(r.rx_hz);
	if (!tx) {
		if (!r.dmr_keyed && r.rf_rx_done) {
			return true;	// the RF RX interrupt has already ended the burst
		}
		if (r.dmr_keyed) {
			k22f_pin_write(PIN_TX_EN, false);
			ramp(r.drive_now, false);
			lines_off(true);
			r.dmr_keyed = false;
		}
		r.rf_isr_dirty = true;
		if (!rf_isr_tune(r.rx_hz, AT1846S_STATE_RX)) {
			r.rf_rx_pending = true;
			r.rf_skipped++;
			return false;
		}
		k22f_pin_write(rf_line[rx_band.rx_other], false);
		k22f_pin_write(rf_line[rx_band.rx_on], true);
		r.rf_rx_pending = false;
		r.rf_rx_done = true;
		return true;
	}
	if (!r.dmr_armed) {
		r.rf_unarmed++;
		return false;
	}
	gd77_rf_band_t band = gd77_rf_band(r.dmr_tx_hz);
	lines_off(false);
	r.rf_isr_dirty = true;
	r.rf_rx_done = false;
	if (!rf_isr_tune(r.dmr_tx_hz, AT1846S_STATE_DMR_TX)) {
		r.rf_skipped++;
		return false;	// no drive without the AT1846S in transmit
	}
	r.rf_rx_pending = false;
	k22f_pin_write(rf_line[band.tx_on], true);
	k22f_pin_write(PIN_TX_EN, true);
	ramp(r.dmr_drive, true);
	r.dmr_keyed = true;
	return true;
}

void gd77_rf_dmr_service(void)
{
	if (r.rf_rx_pending) {
		(void)gd77_rf_dmr_switch(false);
	}
}

bool gd77_rf_dmr_keyed(void)
{
	return r.dmr_keyed;
}

void gd77_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed)
{
	*skipped = r.rf_skipped;
	*unarmed = r.rf_unarmed;
}
#endif

// Stop per-burst keying (task level): disarm, drop a transmission still running, drive down
static void dmr_disarm(void)
{
	r.dmr_armed = false;
	if (gd77_dmr_tx_active()) {
		gd77_dmr_tx_drop();
	}
	if (r.dmr_keyed) {
		k22f_pin_write(PIN_TX_EN, false);
		ramp(r.drive_now, false);
		lines_off(true);
		r.dmr_keyed = false;
	}
}

// The AT1846S again as the task-level driver believes it: the interrupts may have retuned it and switched reg 0x30
static void rf_resync(void)
{
	if (!r.rf_isr_dirty) {
		return;
	}
	r.rf_isr_dirty = false;
	r.rf_rx_pending = false;
	if (r.rx_valid) {
		count(at1846s_set_freq(&r.rf, r.rx_hz));	// forced writes of 0x30 (idle), 0x05, 0x29, 0x2A
	}
	count(at1846s_set_state(&r.rf, r.rx_valid ? AT1846S_STATE_RX : AT1846S_STATE_IDLE));
}

// --- Speaker ---

// The speaker serves two requests: the core's FM squelch (audio(), radio task) and PCM (beeps, DMR voice;
// gd77_speaker_pcm(), audio or radio task). The amplifier is on for either; while PCM plays the FM feed-through
// (4:0x36) is off and the codec's DAC drives the line out. A mutex keeps the two callers' writes apart
static bool audio_wanted, pcm_wanted;
static SemaphoreHandle_t speaker_lock;

static void speaker_apply(void)
{
	bool locked = speaker_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (locked) {
		xSemaphoreTake(speaker_lock, portMAX_DELAY);
	}
	bool pcm = pcm_wanted;
	bool fm = audio_wanted && !pcm;
	count(hrc6000_fm_audio(&r.modem, fm));
	if (pcm) {
		count(gd77_c6k_pcm_out(&r.modem, r.mode == RADIO_MODE_DMR));
	}
	k22f_pin_write(PIN_AUDIO_EN, fm || pcm);	// the speaker amplifier (PTB0)
	if (locked) {
		xSemaphoreGive(speaker_lock);
	}
}

static void audio(bool on)
{
	audio_wanted = on;
	speaker_apply();
}

void gd77_speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	speaker_apply();
}

// --- hal_radio_t ---

// The HR_C6000 out of power-down: the control line high for 10 ms, then low; the other control line stays high
// (hardware-gd77.md 5.2: which line is reset and which is power-down is unverified)
static void c6k_power_up(void)
{
	k22f_pin_write(PIN_C6K_E0, true);
	k22f_pin_write(PIN_C6K_PWD, true);
	hal_delay_ms(C6K_PWD_HIGH_MS);
	k22f_pin_write(PIN_C6K_PWD, false);
}

static void init(void)
{
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
	}
	gd77_i2c_init();
	gd77_c6k_bus_init();
	k22f_pin_write(PIN_TX_EN, false);
	lines_off(true);
	lines_off(false);
	k22f_dac_init();	// DAC0 at 0
	r.drive_now = 0;
	c6k_power_up();
	uint32_t pwd_ms = hal_millis();

	at1846s_attach(&r.rf, &rf_bus);
	at1846s_set_profile(&r.rf, &gd77_at1846s);
	r.diag.rf_ok = at1846s_init(&r.rf);
	count(r.diag.rf_ok);
	count(at1846s_read(&r.rf, 0, 0x00, &r.diag.rf_id));

	while ((uint32_t)(hal_millis() - pwd_ms) < C6K_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc6000_attach(&r.modem, &modem_bus);
	r.diag.c6k_ok = hrc6000_power_on_init_clock(&r.modem, DMR_MIC_GAIN, &hrc6000_clock_12m288) &&
			gd77_c6k_board_init(&r.modem);
	count(r.diag.c6k_ok);
	// The modulation pair as the chip came up (nothing above writes it), 0x80 each if it can't be read
	if (!hrc_read(&r.modem, HRC_PAGE_SYS, 0x45, &r.c6k_mod2) || !hrc_read(&r.modem, HRC_PAGE_SYS, 0x46, &r.c6k_mod1)) {
		count(false);
		r.c6k_mod2 = r.c6k_mod1 = C6K_MOD_FALLBACK;
	}
	count(at1846s_init_finish(&r.rf));

	r.mode = RADIO_MODE_FM;
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	audio(false);
	gd77_dmr_board_init(&r.modem);
}

#if CONFIG_DMR
// HR_C6000 into DMR (modem-hrc6000.md 3.1), then the slot engine. The modulation pair is the one last written: the
// calibration's 4:0x46 comes with each transmission (tx())
static void dmr_enter(void)
{
	count(hrc6000_dmr_enter(&r.modem, r.c6k_mod2, r.c6k_mod1, DMR_MIC_GAIN));
	gd77_dmr_enter();
}
#endif

static void set_mode(radio_mode_t mode)
{
#if CONFIG_DMR
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
			gd77_dmr_leave();
			rf_resync();
			r.tx_on = false;
		}
		count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
		if (!r.tx_on) {
			count(hrc6000_set_mode_fm_rx(&r.modem));
		}
	}
	apply_rx_tone();
#else
	(void)mode;	// FM only in this build
	r.mode = RADIO_MODE_FM;
#endif
}

// The calibration's receive values for the frequency and bandwidth: the block's, or the fallbacks without one, so
// a band without a valid block doesn't keep the other band's values. The squelch base (sq_open) isn't applied: the
// core's squelch works on RSSI and noise
static void apply_rx_cal(uint32_t hz, at1846s_bw_t bw)
{
	gd77_cal_point_t cal;
	(void)gd77_cal_at(hz, &cal);
	count(at1846s_set_rx_volume(&r.rf, cal.rx_dac_gain, cal.rx_volume));
	count(at1846s_write(&r.rf, 0, 0x48, cal.sq_reg[bw][0]));
	count(at1846s_write(&r.rf, 0, 0x60, cal.sq_reg[bw][1]));
	count(at1846s_write(&r.rf, 0, 0x3F, cal.sq_reg[bw][2]));
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	if (!board_in_band(hz, false)) {
		return false;
	}
	bool same = r.rx_valid && hz == r.rx_hz && bw == r.rx_bw;
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();
	}
	r.asleep = false;
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_tone = rx_tone != NULL ? *rx_tone : (subtone_t){ TONE_NONE, 0 };
	r.rx_valid = true;

	at1846s_bw_t abw = rf_bw(bw);
	count(at1846s_set_bandwidth(&r.rf, abw));
	count(at1846s_set_mode(&r.rf, r.mode == RADIO_MODE_DMR ? AT1846S_MODE_DMR : AT1846S_MODE_FM));
	apply_rx_tone();
	apply_rx_cal(hz, abw);
	seq_t c = { .hz = hz, .band = gd77_rf_band(hz) };
	run_seq(SEQ(gd77_rf_rx_seq), &c);
	r.rf_isr_dirty = false;
	r.rf_rx_pending = false;

	if (r.mode == RADIO_MODE_FM) {
		count(hrc6000_set_mode_fm_rx(&r.modem));
	} else if (!same) {
		// New channel: the receiver searches again. Back from our own transmission on the same channel the engine
		// has already restarted it, or kept a repeater's timing
		gd77_dmr_rx_restart();
	}
	r.tx_on = false;
	return true;
}

#if CONFIG_GD77_TX
// The calibration's transmit values: the AT1846S gains, PA driver current and deviation (the tone deviation for the
// tone being sent), then the HR_C6000 modulation amplitude, bias and offset (the same in FM and DMR)
static void apply_tx_cal(const gd77_cal_point_t *cal, at1846s_bw_t bw, at1846s_tone_t tone)
{
	bool dcs = tone == AT1846S_TONE_DCS || tone == AT1846S_TONE_DCS_INV;
	at1846s_tx_cal_t a = {
		.pa_drv = cal->pa_drv,
		.mic_pga = cal->mic_pga,
		.voice_gain = cal->voice_gain,
		.gain_tx = cal->gain_tx,
		.fm_dev = cal->fm_dev[bw],
		.tone_dev = dcs ? cal->dcs_dev[bw] : cal->ctcss_dev[bw],
	};
	count(at1846s_set_tx_cal(&r.rf, &a));
	r.c6k_mod1 = cal->c6k_mod1;
	count(hrc_set_mod_gain(&r.modem, r.c6k_mod2, r.c6k_mod1));
	count(hrc6000_set_mod_bias(&r.modem, cal->c6k_bias, cal->c6k_offset));
}

// Transmit: refused unless the band allows it and the calibration has the frequency
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	if (!board_in_band(hz, true)) {
		r.diag.tx_refused++;
		return false;
	}
	gd77_cal_point_t cal;
	if (!gd77_cal_at(hz, &cal) || !cal.valid) {
		r.diag.tx_refused++;
		return false;	// never an uncalibrated drive
	}
	if (power >= GD77_CAL_POWER_LEVELS) {
		power = GD77_CAL_POWER_LEVELS - 1u;
	}
	uint8_t drive = cal.power_drive[power];

	if (r.mode == RADIO_MODE_DMR) {
		// Arm only: the receiver stays on the RX frequency until our first burst. The per-burst switching
		// writes only the tuning registers, so the values set here hold for every burst
		apply_tx_cal(&cal, rf_bw(bw), AT1846S_TONE_NONE);
		r.dmr_tx_hz = hz;
		r.dmr_drive = drive;
		__DMB();
		r.dmr_armed = true;
		r.tx_on = true;
		return true;
	}

	uint16_t tone_value;
	at1846s_tone_t tone = rf_tone(tx_tone, &tone_value);
	hrc6000_fm_tx_t fm = {
		.uhf = gd77_rf_band(hz).uhf,
		.wide = bw == RADIO_BW_25K,
		.tone = tone == AT1846S_TONE_NONE ? HRC6000_TONE_NONE
			: tone == AT1846S_TONE_CTCSS ? HRC6000_TONE_CTCSS : HRC6000_TONE_DCS,
		.mod2 = r.c6k_mod2,
		.mod1 = cal.c6k_mod1,
		.ref_offset = 0,	// replaced by the calibration's bias and offset below
		.mic_gain = FM_MIC_GAIN,
	};
	count(hrc6000_set_mode_fm_tx(&r.modem, &fm));
	at1846s_bw_t abw = rf_bw(bw);
	count(at1846s_set_bandwidth(&r.rf, abw));
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(at1846s_set_tx_tone(&r.rf, tone, tone_value));
	apply_tx_cal(&cal, abw, tone);
	seq_t c = { .hz = hz, .band = gd77_rf_band(hz), .chip_tx = AT1846S_STATE_FM_TX, .drive = drive };
	run_seq(SEQ(gd77_rf_tx_seq), &c);
	r.tx_on = true;
	return true;
}
#else
// Receive-only build: every transmission is refused
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)hz;
	(void)bw;
	(void)power;
	(void)tx_tone;
	r.diag.tx_refused++;
	return false;
}
#endif

static void tx_off(void)
{
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();	// after DMR_EV_TX_DONE this only disarms; before it, the transmission is cut
		r.rf_isr_dirty = !r.rx_valid;	// rx() below retunes with forced writes; without it, resync here
		rf_resync();
	}
	k22f_pin_write(PIN_TX_EN, false);
	if (r.drive_now != 0) {
		ramp(r.drive_now, false);
	}
	lines_off(true);
	if (r.rx_valid) {
		subtone_t tone = r.rx_tone;
		(void)rx(r.rx_hz, r.rx_bw, &tone);
		return;
	}
	count(at1846s_set_tx_tone(&r.rf, AT1846S_TONE_NONE, 0));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	r.tx_on = false;
}

static void status(radio_status_t *st)
{
	if (gd77_dmr_tx_active()) {
		*st = r.last_status;	// keep bus A free for the per-burst switching
		return;
	}
	uint8_t rssi, noise;
	uint16_t flags, value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	if (!at1846s_read_signal(&r.rf, &rssi, &noise) || !at1846s_read_flags(&r.rf, &flags)) {
		count(false);
		st->rssi_dbm = -140;
		st->noise = 255;
		st->carrier = false;
		st->tone_ok = type == AT1846S_TONE_NONE;
		return;
	}
	r.diag.rssi = rssi;
	r.diag.noise = noise;
	st->rssi_dbm = gd77_rf_band(r.rx_hz).uhf ? (int16_t)(rssi - 151) : (int16_t)(-164 + rssi * 32 / 27);
	st->noise = noise;
	st->carrier = (flags & AT1846S_FLAG_SQ) != 0;
	st->tone_ok = at1846s_tone_match(flags, type);
	r.last_status = *st;
}

static void rx_sleep(bool sleep)
{
	if (r.mode != RADIO_MODE_FM || r.tx_on || !r.rx_valid || sleep == r.asleep) {
		return;
	}
	if (sleep) {
		count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
		lines_off(false);
		r.asleep = true;
	} else {
		subtone_t tone = r.rx_tone;
		(void)rx(r.rx_hz, r.rx_bw, &tone);
	}
}

void gd77_radio_diag(gd77_radio_diag_t *d)
{
	*d = r.diag;
	d->drive = r.drive_now;
	d->tx_build = CONFIG_GD77_TX != 0;
}

const hal_radio_t gd77_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
};
