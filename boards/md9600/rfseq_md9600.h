// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 RF sequencing: the order of supply, VCO, synthesizer, IF and PA steps for receive, transmit and the way back,
// per board revision (docs/spec/hardware-md9600.md 5.5, 19.8). Pure: the lines, both three-wire buses, the DAC, the
// ADC, the calibration and the delays come in through md9600_rf_ops_t, so the host test (tests/host/md9600RfSeqTest.c)
// logs every step. radio_md9600.c wires it to the pins.
//
// Transmit is gated three times: the build (CONFIG_MD9600_TX, off by default), a run-time arm that starts off at every
// power-up (md9600_rf_tx_arm), and the board state (a stored revision, a plausible PA calibration). Every error path
// and every refusal leaves the PA loop and both transmitter supplies off.
#ifndef RFSEQ_MD9600_H
#define RFSEQ_MD9600_H

#include <stdint.h>
#include <stdbool.h>
#include "rfplan_md9600.h"

// The lines the sequencer drives. Board pins in radio_md9600.c; levels are logical (true = on, RX VCO, VHF, high)
typedef enum {
	MD9600_LN_POWER,	// PD15 Power_Control: RF and audio sections
	MD9600_LN_R5_V,		// PA14 VHF receiver 5 V
	MD9600_LN_R5_U,		// PA13 UHF receiver 5 V
	MD9600_LN_C5_V,		// PC9 VHF 5 V (receiver side)
	MD9600_LN_C5_U,		// PA8 UHF 5 V (receiver side)
	MD9600_LN_T5_V,		// PD5 VHF transmitter 5 V
	MD9600_LN_T5_U,		// PD6 UHF transmitter 5 V
	MD9600_LN_APC,		// PC4 PA power loop
	MD9600_LN_VCO_V,	// PE9 true = VHF RX VCO, false = TX VCO
	MD9600_LN_VCO_U,	// PE8 true = UHF RX VCO, false = TX VCO
	MD9600_LN_MOD_VHF,	// PB7 HR_C6000 MOD2 to the VHF (true) or UHF (false) VCO
	MD9600_LN_IF_RST,	// PE15 AK2365A reset (V1-V4), false = in reset
	MD9600_LN_V5_BW_V,	// PE11 on V5: VHF IF filter, true = 25 kHz
	MD9600_LN_V5_BW_U,	// PE12 on V5: UHF IF filter
	MD9600_LN_V5_ROUTE_V,	// PE13 on V5: the VHF IF to the HR_C6000
	MD9600_LN_V5_ROUTE_U,	// PE14 on V5: the UHF IF to the HR_C6000
	MD9600_LN_FM_SW,	// PB2 FM audio stages on
	MD9600_LN_AF_VHF,	// PC13 receive audio from the VHF (true) or UHF (false) IF chip
	MD9600_LN_COUNT
} md9600_line_t;

// The DAC channels: DAC1 (PA4) tunes the UHF front end, DAC2 (PA5) tunes the VHF front end in receive and is the
// APC reference for both bands in transmit
enum { MD9600_DAC_TUNE_U = 0, MD9600_DAC_APC_TUNE_V = 1 };

typedef struct {
	void (*line)(void *ctx, uint8_t line, bool level);
	// One word on the synthesizer bus (band = chip select) or the IF bus (V1-V4). False: the bus refused it
	bool (*pll)(void *ctx, uint8_t band, uint32_t word, uint8_t bits);
	bool (*ifw)(void *ctx, uint8_t band, uint16_t word);
	void (*dac)(void *ctx, uint8_t ch, uint16_t value);
	void (*delay_us)(void *ctx, uint32_t us);
	// Optional: a lock indication for the band's synthesizer. NULL on this board (no lock detect line is known): the
	// sequencer then waits the synthesizer's settle time and trusts it
	bool (*locked)(void *ctx, uint8_t band);
	// Calibration: the front-end tuning DAC for a receive frequency, and the APC DAC for a transmit frequency and
	// power level (false: no usable PA calibration there, transmit is refused)
	uint16_t (*rx_tune)(void *ctx, uint32_t hz);
	bool (*apc)(void *ctx, uint32_t hz, uint8_t power, uint16_t *dac);
	// Optional: the APC drive where the band's PA output reaches zero. The power loop loses control under it (the PA
	// jumps to about 10 W, drive 0 included), so the reference waits there while the transmitter supply and the loop
	// come on and off, and every ramp starts and ends there. NULL: 0
	uint16_t (*apc_floor)(void *ctx, uint8_t band);
	// A 12-bit noise reading of the band's receiver (the probe)
	uint16_t (*noise)(void *ctx, uint8_t band);
	void *ctx;
} md9600_rf_ops_t;

typedef enum {
	MD9600_RF_OK,
	MD9600_RF_E_REV,	// no revision this image drives: the receiver stays off
	MD9600_RF_E_BAND,	// outside both bands, or no ratio for it
	MD9600_RF_E_BUS,	// a bus write failed
	MD9600_RF_E_LOCK,	// the lock indication didn't come within the timeout
	MD9600_RF_E_TX_BUILD,	// transmit isn't in this build (CONFIG_MD9600_TX)
	MD9600_RF_E_TX_ARM,	// transmit isn't armed (md9600_rf_tx_arm)
	MD9600_RF_E_TX_REV,	// the revision isn't confirmed by a stored record
	MD9600_RF_E_TX_CAL,	// no plausible PA calibration for the frequency and level
	MD9600_RF_E_STATE,	// not in a state that allows it (PA keying outside transmit)
	MD9600_RF_E_COUNT
} md9600_rf_err_t;

typedef enum { MD9600_RF_OFF, MD9600_RF_RX, MD9600_RF_TX } md9600_rf_state_t;

// Timing (unverified unless noted: no radio has been measured). The synthesizers have no lock detect line, so each
// gets a fixed settle time after its words; with a lock indication the wait polls it every MD9600_LOCK_POLL_US up to
// the timeout. The LMX2571 transmit settle and its reset wait are what the original V26.45 firmware waits (12 and 10
// system ticks)
#define MD9600_SETTLE_SKY72310_US	2000u
#define MD9600_SETTLE_PLL1590_US	2000u
#define MD9600_SETTLE_LMX2571_US	2000u	// receive: includes the calibration after the 0x3F / 0x3E pair
#define MD9600_SETTLE_LMX2571_TX_US	12000u	// transmit: from the last word to the transmitter supply
#define MD9600_LMX2571_RESET_US		10000u	// after the power-on reset of both chips
#define MD9600_LOCK_TIMEOUT_US		10000u
#define MD9600_LOCK_POLL_US		100u
#define MD9600_IF_CAL_US		2000u	// AK2365A discriminator calibration: 1.3 ms in the data sheet
#define MD9600_IF_RESET_US		10u	// AK2365A reset low: at least 1 us
#define MD9600_RX_SETTLE_US		3000u	// supplies and front end before the receiver is trusted
#define MD9600_TX_SUPPLY_US		1000u	// transmitter supply on, before the PA loop
#define MD9600_APC_RAMP_STEPS		8u	// APC reference up and down in steps, so the PA doesn't splash
#define MD9600_APC_RAMP_STEP_US		250u
// DMR bursts: the RF TX interrupt comes about 1.3 ms ahead of the slot (on the other HR_C6000 radios), so the burst's
// supply settle and ramp are short. The supply goes off between bursts: left on, the PA leaks the carrier at about
// -13 dB (measured off air). That leak counts as on air: the supply on at the interrupt made every burst 31.7-31.9 ms
// long (bench, 2026-11-06; the bench allows 27-31.5 ms). The key-up starts MD9600_BURST_KEY_DELAY_US after the
// interrupt, from a timer (md9600_rf_pa_step), and reaches full power about 0.2 ms after the slot boundary, inside the
// guard ahead of the payload (hardware-md9600.md 19.8)
#define MD9600_BURST_KEY_DELAY_US	700u
#define MD9600_BURST_T5_US		500u
#define MD9600_BURST_RAMP_STEPS		4u
#define MD9600_BURST_RAMP_STEP_US	100u
#define MD9600_SLOT_US			30000u
// The split duplex BS outbound's one key-up starts this far ahead of a slot's timeslot interrupt: full power 6 ms
// before it, after that slot's sync (2.5 + 11.25 to 18.75 ms into it) and well ahead of the next CACH
#define MD9600_BS_KEY_LEAD_US \
	(MD9600_BURST_T5_US + (MD9600_BURST_RAMP_STEPS - 1u) * MD9600_BURST_RAMP_STEP_US + 6000u)
#define MD9600_V4_KICK_HZ		145000000u	// V4: the VHF synthesizer is set right after power-on or it never locks

typedef struct {
	const md9600_rf_ops_t *ops;
	md9600_rev_t rev;		// MD9600_REV_UNKNOWN: nothing is programmed, the receiver stays off
	const md9600_rev_desc_t *desc;	// NULL with MD9600_REV_UNKNOWN
	bool confirmed;			// the stored record named the revision (the only source that allows transmit)
	bool tx_armed;			// run-time arm, off at every start
	bool dmr;			// DMR mode: narrow IF, the FM audio stages off
	uint8_t state;			// md9600_rf_state_t
	// The reference trim (hal_radio_t.ref_trim, ppb): every synthesizer is programmed for f x (1 + trim), taken at the
	// next tune. The IF stays as it is: both synthesizers and the IF chip run from the one TCXO
	volatile int16_t trim_ppb;
	// The last receive settings (tx_off returns to them)
	bool rx_valid;
	uint32_t rx_hz;
	bool rx_wide;
	// The last transmit settings
	uint32_t tx_hz;
	uint8_t tx_band;
	uint16_t apc;			// APC DAC value of the running transmission
	uint16_t apc_floor;		// its band's zero-output drive (ops.apc_floor)
	bool pa_on;
	// Diagnostics
	uint32_t lo_hz;			// the synthesizer's last frequency
	uint8_t band;			// its band
	uint32_t retunes, refused, errors;
	uint8_t last_err;		// md9600_rf_err_t of the last refusal or failure
} md9600_rf_t;

// Hold everything off: PA loop and transmitter supplies first, then the receivers. rev must be usable (its driver
// built) or MD9600_REV_UNKNOWN
void md9600_rf_init(md9600_rf_t *rf, const md9600_rf_ops_t *ops, md9600_rev_t rev, bool confirmed);
// After power-up: both receivers powered, the AK2365A reset pulse (V1-V4), the V4 VHF synthesizer kick, the LMX2571
// reset (V4A / V5)
md9600_rf_err_t md9600_rf_power_up(md9600_rf_t *rf);

// Receive on hz (wide = 25 kHz). From any state: a running transmission is ramped down first
md9600_rf_err_t md9600_rf_rx(md9600_rf_t *rf, uint32_t hz, bool wide);
// FM (false) or DMR (true); takes effect at the next rx() or tx()
void md9600_rf_set_dmr(md9600_rf_t *rf, bool dmr);
// Receivers and synthesizers powered down (no receive until the next rx)
void md9600_rf_sleep(md9600_rf_t *rf);

// Transmit on hz at a board power level (0 = 5 W .. 3 = 40 W, labels unverified). Refused unless built, armed,
// confirmed and calibrated; on any refusal or failure the PA stays off and the receiver returns
md9600_rf_err_t md9600_rf_tx(md9600_rf_t *rf, uint32_t hz, bool wide, uint8_t power);
// The transmit side tuned with the PA off (DMR, ahead of the first burst): the same checks as md9600_rf_tx, the
// receivers off, the TX VCO on the carrier. settle false: the synthesizer words only, without the wait (from an
// interrupt or under a lock); the caller waits md9600_rf_tx_settle_us() before the first burst
md9600_rf_err_t md9600_rf_tx_tune(md9600_rf_t *rf, uint32_t hz, uint8_t power, bool settle);
uint32_t md9600_rf_tx_settle_us(const md9600_rf_t *rf);
// SFR (a DMR transmission that receives between its bursts): from the modem interrupts after a burst's PA is off, the
// synthesizer back on the receive LO and the receivers on, without the waits; the IF chip keeps its set-up. The next
// burst retunes with md9600_rf_tx_tune(settle false) and waits for the lock before its PA
md9600_rf_err_t md9600_rf_burst_rx(md9600_rf_t *rf);
// Receiving again after md9600_rf_burst_rx and md9600_rf_tx_off, on the same channel: the front-end tune once more (on
// VHF it shares the DAC with the PA drive, which the end of the transmission set to 0), without synthesizer words or
// waits, so a reply arriving now isn't cut. E_STATE when not receiving on a valid channel with the PA off
md9600_rf_err_t md9600_rf_rx_resume(md9600_rf_t *rf);
// One DMR burst's PA: on, the band's transmitter supply, MD9600_BURST_T5_US, the loop closed and a short APC ramp
// (MD9600_BURST_RAMP_*); off, the ramp down, the loop open and the supply off. Busy waits only: callable from the
// modem interrupts
md9600_rf_err_t md9600_rf_pa_burst(md9600_rf_t *rf, bool on);
// The burst's key-up in steps, for a timer (no busy waits): step 0 the supply, then the loop and each ramp step.
// Call with step 0, 1, ... each *next_us later, until *next_us comes back 0. Step 0 with the PA already on does
// nothing. md9600_rf_pa_burst(rf, false) ends it at any step
md9600_rf_err_t md9600_rf_pa_step(md9600_rf_t *rf, uint32_t step, uint32_t *next_us);
// While transmitting: the PA alone on or off (the transmitter supply and the loop, the synthesizer stays), for DMR
// bursts
md9600_rf_err_t md9600_rf_pa(md9600_rf_t *rf, bool on);
// End a transmission: APC ramped down, the loop and the supplies off, then the last receive settings
void md9600_rf_tx_off(md9600_rf_t *rf);
// Arm or disarm transmit for this power cycle. Arming fails without CONFIG_MD9600_TX or a confirmed revision.
// Disarming drops the PA at once (lines and DAC only, safe from any task); the transmission ends at tx_off
bool md9600_rf_tx_arm(md9600_rf_t *rf, bool on);
// What a tx() on hz at power would be refused for now, MD9600_RF_OK when it would be allowed
md9600_rf_err_t md9600_rf_tx_check(const md9600_rf_t *rf, uint32_t hz, uint8_t power);

// The PA loop and both transmitter supplies off, the APC reference at zero: the safe state every path ends in
void md9600_rf_pa_safe(md9600_rf_t *rf);

// Receiver probe (MD9600_REV_PROBE): with the transmitter held off and the RX VCOs selected, program the UHF
// synthesizer and IF with each built revision's words for hz and read the UHF noise; the verdict is
// md9600_rev_probe_pick over the unprogrammed baseline. Leaves the radio off (md9600_rf_init again before use)
#define MD9600_PROBE_HZ		446000000u
#define MD9600_PROBE_SETTLE_US	50000u
#define MD9600_PROBE_THRESHOLD	200u	// ADC counts. Unmeasured: the probe is off by default
md9600_rev_t md9600_rf_probe(const md9600_rf_ops_t *ops, uint32_t hz, uint16_t threshold);

// Short names of the errors for the diagnostics page and the debug link
const char *md9600_rf_err_name(uint8_t err);

// --- Receiver readings ---

// RSSI from the band's 12-bit average (hardware-md9600.md 5.7, a fit, unverified)
int16_t md9600_rf_rssi_dbm(md9600_rev_t rev, uint8_t band, uint16_t raw12);
// The noise reading on the core's squelch scale (1 dB steps, larger = more noise; core/radio/radio_sm.h). With the
// calibration's squelch thresholds (raw readings where level 1 and level 9 open), raw maps linearly so level 1 opens
// at the core's level 1 line and level 9 at its level 9 line. Without them (l1 == l9): raw / 16
#define MD9600_SQL_CORE_L1	64
#define MD9600_SQL_CORE_L9	40
uint8_t md9600_rf_noise(uint16_t raw12, uint16_t l1_open, uint16_t l9_open);

// The squelch's noise window (hardware-md9600.md 5.7). The noise line follows the modulation: under a strong carrier it
// reads about 100 counts in a pause and peaks over 1000 in speech for tens of ms, while an empty channel holds
// 1700-1900. The squelch gets the lowest reading over the last MD9600_NOISE_BUCKETS buckets of MD9600_NOISE_BUCKET_MS,
// so speech peaks don't close it. RSSI holds steady through the modulation and falls about 11 dB within one poll when
// the carrier ends (UHF, -127 dBm): a fall of MD9600_NOISE_RSSI_DROP_DB or more since the last reading starts the
// window over, so the squelch closes on the next reading instead of 160 ms of noise later
#define MD9600_NOISE_BUCKET_MS		40u
#define MD9600_NOISE_BUCKETS		4u
#define MD9600_NOISE_RSSI_DROP_DB	8
typedef struct {
	uint16_t min[MD9600_NOISE_BUCKETS];
	uint32_t bucket;	// the bucket number (ms / MD9600_NOISE_BUCKET_MS) of min[bucket % MD9600_NOISE_BUCKETS]
	int16_t last_dbm;
	bool valid;
} md9600_noise_win_t;
// Forget every reading (each retune and wake-up, so a scan never sees the last channel's)
void md9600_noise_clear(md9600_noise_win_t *w);
// Add one reading; returns the window's lowest
uint16_t md9600_noise_floor(md9600_noise_win_t *w, uint16_t raw12, int16_t rssi_dbm, uint32_t now_ms);
// The squelch's reading for the core (radio_status_t.noise): the window's lowest, mapped (md9600_rf_noise). While the
// receiver is off (transmit, sleep) the noise line reads 700-800, which looks like a carrier: no reading of it enters
// the window and the squelch reads closed (255), so a transmission's readings can't open the squelch after it ends
uint8_t md9600_rf_squelch_noise(md9600_noise_win_t *w, bool receiving, uint16_t raw12, int16_t rssi_dbm,
				uint32_t now_ms, uint16_t l1_open, uint16_t l9_open);

#endif
