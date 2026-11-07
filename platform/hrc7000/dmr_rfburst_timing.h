// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The burst timing of DMR transmit with a whole RF key-up per burst (dmr_rfburst.h): plain arithmetic, for the host
// tests
#ifndef HRC_DMR_RFBURST_TIMING_H
#define HRC_DMR_RFBURST_TIMING_H

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
		return 0;	// up before the first symbol even with no lead
	}
	return (uint8_t)((up_us - HRC_RFBURST_SYMBOL_AFTER_LEAD_US + 99u) / 100u);
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

#endif
