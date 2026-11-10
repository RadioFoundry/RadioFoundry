// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The burst timing of DMR transmit with a whole RF key-up per burst (dmr_rfburst.h): plain arithmetic, for the host
// tests
#ifndef HRC_DMR_RFBURST_TIMING_H
#define HRC_DMR_RFBURST_TIMING_H

#include <stdbool.h>
#include <stdint.h>

// --- Burst timing ---
// The modem raises the RF TX interrupt rf_tx_lead (100 us steps, RF_CONTROL b13:8) before its TX burst, and the burst's
// first symbol is on air 0.85 ms after that point (measured on the SDR bench). up_us: how long the board's
// TX-on sequence takes from the interrupt until the PA is fully up. The lead is the shortest that has the PA up for
// the first symbol: every 100 us more switches the receiver off earlier in the slot before ours, where an SFR
// repeater hears the inbound burst
#define HRC_RFBURST_SYMBOL_AFTER_LEAD_US	850u
#define HRC_RFBURST_BURST_US			27500u
static inline uint8_t hrc_rfburst_lead(uint32_t up_us)
{
	if (up_us <= HRC_RFBURST_SYMBOL_AFTER_LEAD_US) {
		return 1;	// up even with the shortest lead (0 is the driver's default, 3.1 ms)
	}
	return (uint8_t)((up_us - HRC_RFBURST_SYMBOL_AFTER_LEAD_US + 99u) / 100u);
}
// A TX on in two points, both counted from the RF TX interrupt: the RF switched to TX switch_lead_us ahead of the lead
// point, and the PA keyed pa_ahead_us before the first symbol
static inline uint32_t hrc_rfburst_switch_at_us(uint8_t lead, uint32_t switch_lead_us)
{
	uint32_t at = lead * 100u;
	return at > switch_lead_us ? at - switch_lead_us : 0;
}
static inline uint32_t hrc_rfburst_pa_at_us(uint8_t lead, uint32_t pa_ahead_us)
{
	uint32_t at = lead * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US;
	return at > pa_ahead_us ? at - pa_ahead_us : 0;
}
// The ramp down starts with the burst's last symbol, lead + 0.85 ms + 27.5 ms after the RF TX interrupt, so the
// receiver is back for the slot after ours. How long to wait still, elapsed_us after the RF TX interrupt (0 once that
// point is past), and by how much it was missed (0 when on time)
static inline uint32_t hrc_rfburst_off_at_us(uint8_t lead)
{
	return lead * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US + HRC_RFBURST_BURST_US;
}
static inline uint32_t hrc_rfburst_hold_us(uint32_t elapsed_us, uint8_t lead)
{
	uint32_t at = hrc_rfburst_off_at_us(lead);
	return elapsed_us < at ? at - elapsed_us : 0;
}
static inline uint32_t hrc_rfburst_late_us(uint32_t elapsed_us, uint8_t lead)
{
	uint32_t at = hrc_rfburst_off_at_us(lead);
	return elapsed_us > at ? elapsed_us - at : 0;
}

// --- The first burst against the slot clock ---
// With a long lead the modem raises the first burst's RF TX interrupt late, at the same point every key-up, while the
// burst itself keeps the slot grid (on the SDR bench at lead 26: 0.42 ms, the later bursts on time; less with a
// shorter lead). Timed from that stamp, the whole first burst's key-up and key-down came 0.42 ms late. The slot
// engine's tick before the interrupt is steady (30 ms), so the later bursts' time after it is the reference for the
// first. Bursts on the 60 ms grid teach it: the largest such time, as a masked tick comes late and makes it shorter,
// while a late stamp is off the grid and not taken
#define HRC_RFBURST_FIRST_LATE_MAX_US	1000u	// later than this against the reference: not the modem's late interrupt
static inline uint32_t hrc_rfburst_tick_ref(uint32_t ref_us, uint32_t stamp, uint32_t tick, uint32_t slot_us)
{
	uint32_t off = stamp - tick;
	return off < slot_us && off > ref_us ? off : ref_us;
}
// The first burst's time: the reference after its tick when its stamp is later than that by more than slack_us (and
// no more than HRC_RFBURST_FIRST_LATE_MAX_US), else the stamp. ref_us 0: nothing learned yet
static inline uint32_t hrc_rfburst_first_on(uint32_t stamp, uint32_t tick, uint32_t ref_us, uint32_t slack_us)
{
	uint32_t off = stamp - tick;
	if (ref_us == 0 || off <= ref_us + slack_us || off - ref_us > HRC_RFBURST_FIRST_LATE_MAX_US) {
		return stamp;
	}
	return tick + ref_us;
}
// A burst d_us after the one before is taken back to the frame grid when later than frame_us + slack_us. Not right
// after a retimed first burst: a retime from a wrong tick (or a reference from another mode) would put the grid early,
// and later bursts, only ever moved earlier onto it, would keep it. That burst's own stamp starts the grid instead
static inline bool hrc_rfburst_regrid(uint32_t d_us, uint32_t frame_us, uint32_t slack_us, bool after_retime)
{
	return !after_retime && d_us > frame_us + slack_us;
}

#endif
