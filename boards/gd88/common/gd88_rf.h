// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What both GD-88 SoCs share (docs/spec/hardware-gd88.md 11): each has its own BK4819 pair (receiver / transmitter and
// the superhet LO) on the same pins, its own HR_C7000 modem and its own NAND. The radio, DMR and NAND code in
// boards/gd88/common runs on both. What differs is how the RF and audio switches are reached: the TX / RX line PTB2,
// the LO chip's GPIOs and the serial chain's audio routing on A (paths_gd88.c), direct GPIO pairs on B (paths_gd88b.c).
#ifndef GD88_RF_H
#define GD88_RF_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"
#include "gd88_cal.h"

// Transmit (config.mk GD88_TX=1, A only; B is built as a receiver). A's main chip makes the signal; its PA, PA
// supplies and PA drive (DAC B) are B's, keyed by B on A's TX line PTB2 with the drive A sends (hardware-gd88.md 10)
#ifndef GD88_TX
#define GD88_TX			0
#endif
// DMR transmit on this SoC's own chip, per burst (dmr_gd88.c). A: with GD88_TX. B: A's second transmitter, the repeat of
// an SFR (core/rx2/rx2_dmr.h, the original firmware's "path 1": B's chip, B's path pins and B's own calibration)
#ifndef GD88_DMR_TX
#define GD88_DMR_TX		GD88_TX
#endif
// The band path is the LO chip's GPIO4 / GPIO5 (A: VHF = GPIO5 high, GPIO4 low; UHF the reverse). B switches its band
// by GPIO instead and builds with 0
#ifndef GD88_BAND_LO_GPIO
#define GD88_BAND_LO_GPIO	1
#endif
// The superhet's IF: A's firmware tunes the main chip to 45 MHz with either LO chip; B's is 51.55 MHz (its config.mk)
#ifndef GD88_IF_HZ
#define GD88_IF_HZ		45000000u
#endif
#define GD88_MIC_GAIN_DB	11u	// GCR_ADCL at init, as the other HR_C7000 radios

// The RF chips' 3-wire bus, the same pins on both SoCs
#define GD88_PIN_RF_SEN_LO	0x13	// PTA19, LO BK4819 enable, low = selected
#define GD88_PIN_RF_CLK		0x30	// PTB16
#define GD88_PIN_RF_DATA	0x38	// PTB24, bidirectional
#define GD88_PIN_RF_SEN_MAIN	0x39	// PTB25, main BK4819 enable, low = selected
#define GD88_PIN_NAND_CS	0x12	// PTA18, SPI NAND chip select, low = selected

// --- Radio (radio_gd88.c) ---
extern const hal_radio_t gd88_radio;
typedef struct {
	uint16_t main_id, lo_id;	// register 0 of each RF chip (0x4819 expected)
	uint16_t rssi_raw, noise_raw;	// main chip 0x67 / 0x65 from the last status poll
	uint32_t rf_writes;
	uint32_t hz;			// the tuned frequency
	uint16_t pa_low, pa_high;	// the PA drive a transmission there would use (0: TX refused)
	bool transmitting;
	bool supported;			// both chips answered as BK4819
} gd88_radio_diag_t;
void gd88_radio_diag(gd88_radio_diag_t *d);
// The squelch now (status() and the receiver's own squelch on B): the main chip's RSSI against the page's threshold
// for level (0 = open), and the CTCSS decoder when a tone is set
bool gd88_radio_squelch_open(uint8_t level);
// The core's last volume() in percent (A passes it on to B's audio)
uint8_t gd88_radio_volume(void);

// --- RF and audio paths, one implementation per SoC ---
void gd88_path_init(void);			// every switch at its safe level: TX off, RX path off, speaker off
// The receive path of hz's band on (dmr: on a DMR channel), or every RX switch off
void gd88_path_rx(uint32_t hz, bool on, bool dmr);
void gd88_path_rx_front_off(void);		// TX start: the receive path off (A: PTB2 high)
void gd88_path_tx_off(void);			// TX switches off (A: PTB2 low). Any task
#if GD88_DMR_TX
// The PA for the next transmissions: the drive and the page's delays on hz. A: B's PA (rx2_gd88.c, RX2_TX), false when
// B is down or didn't take it within a short wait: nothing may transmit then. B: its own (pa_gd88b.c). Disarm after the
// last one
bool gd88_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us);
void gd88_pa_disarm(void);
#endif
#if GD88_DMR_TX && !GD88_TX
// B's own transmitter, per burst (radio_gd88.c gd88_burst_on / off): its path pins to its chip in hz's band and its RX
// path off, or back (paths_gd88b.c); its PA up to the armed drive (select, 200 us, supply, ramp) or down (pa_gd88b.c)
void gd88_path_own_tx(bool on, uint32_t hz);
void gd88_pa_own(bool on);
#endif
#if GD88_TX && CONFIG_SFR
// The SFR's repeat on B's transmitter (core/rx2/rx2_dmr.h) turned on or off: DMR TX armed here moves there or back
// (rx2_gd88.c)
void gd88_dmr_split(bool on);
bool gd88_dmr_listen(uint32_t *hz, uint8_t *power);	// rx2_dmr_main_cfg_t.listen: A's DMR receive frequency
#endif
// The speaker: on or off, fed from the modem's line out (DMR voice, beeps) or the main chip's AF (FM)
void gd88_path_speaker(bool on, bool from_modem);

// --- DMR (dmr_gd88.c, CONFIG_DMR) ---
#if CONFIG_DMR
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "vocoder.h"
#include "rf/bk4819/bk4819.h"
#include "modem/hrc7000/hrc7000.h"

void gd88_dmr_init(bk4819_t *main_chip, hrc7000_t *modem);
void gd88_dmr_enter(uint32_t rx_hz);
void gd88_dmr_leave(void);
// The LO's side for the modem's RX IQ inversion (RF_MODE bit 15: set with the LO above the signal, modem-hrc7000.md)
void gd88_dmr_lo_side(bool high);
void gd88_speaker_pcm(bool on);		// radio_gd88.c: the speaker on while the modem plays CPU samples
uint32_t gd88_dmr_phase(void);		// HR_C7000 PHASE_OUT: b29:16 / b13:0, the largest / smallest AF phase

#if GD88_DMR_TX
void gd88_dmr_tx_arm(void);
void gd88_dmr_tx_disarm(void);
typedef void (*gd88_wait_fn)(uint32_t us);
void gd88_burst_on(gd88_wait_fn wait);
uint32_t gd88_burst_off(gd88_wait_fn wait, uint32_t elapsed_us, bool now);
typedef struct {
	uint32_t bursts, ends, rfrx_ints, late, last_late_us;
	char last_by;		// what woke the last TX-off: 'R' RF RX interrupt, 'T' the engine, 'A' abort
} gd88_dmr_tx_stats_t;
void gd88_dmr_tx_stats(gd88_dmr_tx_stats_t *st);
#endif
#endif

// --- SPI NAND (nand_gd88.c) ---
// RadioFoundry's own block range, the only blocks it programs or erases: 32 blocks (4 MB) from block 1920 of the 2048,
// far above the original firmware's partitions (all below 0x420000) and below its spare pool, the top 10 usable blocks
// (hardware-gd88.md 7)
#define GD88_NAND_RFOUNDRY_FIRST	1920u
#define GD88_NAND_RFOUNDRY_BLOCKS	32u
bool gd88_nand_init(void);
bool gd88_nand_ready(void);
uint16_t gd88_nand_id(void);
uint32_t gd88_nand_corrected(void);
bool gd88_nand_read(uint32_t addr, void *buf, uint32_t len);
// A stock partition's bytes at its logical address, through the stock block map (spi-nand-lut.md); false without one
bool gd88_nand_read_stock(uint32_t addr, void *buf, uint32_t len);
void gd88_nand_lock(void);
void gd88_nand_unlock(void);
#if CONFIG_NAND_STORAGE
#include "flash/nand_sectors/nand_sectors.h"
extern const nand_dev_t gd88_nand_dev;
uint32_t gd88_nand_program_fails(void);
#endif

#endif
