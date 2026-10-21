// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 RF sequencing: the order of supply, VCO, synthesizer, IF and PA steps for receive, transmit and the way back,
// per board revision (docs/spec/hardware-md9600.md 5.5, 19.5). Pure: the lines, both three-wire buses, the DAC, the
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

// Timing (all unverified: no radio has been measured). The synthesizers have no lock detect line, so each gets a fixed
// settle time after its words; with a lock indication the wait polls it every MD9600_LOCK_POLL_US up to the timeout
#define MD9600_SETTLE_SKY72310_US	2000u
#define MD9600_SETTLE_PLL1590_US	2000u
#define MD9600_SETTLE_LMX2571_US	2000u	// includes the VCO calibration after the 0x3F / 0x3E pair
#define MD9600_LOCK_TIMEOUT_US		10000u
#define MD9600_LOCK_POLL_US		100u
#define MD9600_IF_CAL_US		2000u	// AK2365A discriminator calibration: 1.3 ms in the data sheet
#define MD9600_IF_RESET_US		10u	// AK2365A reset low: at least 1 us
#define MD9600_RX_SETTLE_US		3000u	// supplies and front end before the receiver is trusted
#define MD9600_TX_SUPPLY_US		1000u	// transmitter supply on, before the PA loop
#define MD9600_APC_RAMP_STEPS		8u	// APC reference up and down in steps, so the PA doesn't splash
#define MD9600_APC_RAMP_STEP_US		250u
#define MD9600_V4_KICK_HZ		145000000u	// V4: the VHF synthesizer is set right after power-on or it never locks

typedef struct {
	const md9600_rf_ops_t *ops;
	md9600_rev_t rev;		// MD9600_REV_UNKNOWN: nothing is programmed, the receiver stays off
	const md9600_rev_desc_t *desc;	// NULL with MD9600_REV_UNKNOWN
	bool confirmed;			// the stored record named the revision (the only source that allows transmit)
	bool tx_armed;			// run-time arm, off at every start
	bool dmr;			// DMR mode: narrow IF, the FM audio stages off
	uint8_t state;			// md9600_rf_state_t
	// The last receive settings (tx_off returns to them)
	bool rx_valid;
	uint32_t rx_hz;
	bool rx_wide;
	// The last transmit settings
	uint32_t tx_hz;
	uint8_t tx_band;
	uint16_t apc;			// APC DAC value of the running transmission
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
// After power-up: both receivers powered, the AK2365A reset pulse (V1-V4), the V4 VHF synthesizer kick
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

#endif
