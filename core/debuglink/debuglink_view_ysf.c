// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fusion's view section, "ysf" (docs/spec/debuglink.md 3, docs/testing/ysf-bench.md): the channel's state, the
// transmission heard last, and the counters the first radio tests read (the whitening order, the sync bit errors, the
// clock and carrier offsets). A section of its own: on the fork it rode in the features section, which main has
// filled to its reply size
#include "config.h"
#include "debuglink/debuglink.h"
#include "audio/miclimit/miclimit.h"

void debuglink_view_ysf(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_YSF
	static const char *const modes[] = { "none", "dn", "vd1", "vw", "data" };
	static const char *const faults[] = { "ok", "no_path", "rx_refused", "no_cal" };
	const radio_ysf_view_t *y = &v->ysf;
	radio_ysf_stats_t s;
	radio_ysf_stats(&s);
	dl_text(t, "ysf.built=1\nysf.on=%u\nysf.fault=%s\nysf.codec=%u\nysf.rx=%u\nysf.playing=%u\n", y->on,
		y->fault < 4 ? faults[y->fault] : "?", y->codec, y->rx, y->playing);
	dl_text(t, "ysf.mode=%s\nysf.dgid=%u\nysf.muted=%u\nysf.narrow=%u\nysf.src=%s\nysf.dest=%s\nysf.up=%s\nysf.down=%s\n",
		y->mode < 5 ? modes[y->mode] : "?", y->dgid, y->muted, y->narrow, y->src, y->dest, y->up, y->down);
	dl_text(t, "ysf.tx_src=%s\nysf.tx_dest=%s\nysf.tx_dgid=%u\nysf.rx_dgid=%u\nysf.no_call=%u\n", y->tx_src,
		y->tx_dest, y->tx_dgid, y->rx_dgid, y->no_call);
	// Voice FR: whether this build plays and sends it, and whether PTT on this channel sends it (ysf-modem.md 4.1)
	dl_text(t, "ysf.vw_built=%u\nysf.tx_vw=%u\n", (unsigned)CONFIG_YSF_VW, y->tx_vw);
	dl_text(t, "ysf.syncs=%u\nysf.late=%u\nysf.fich_good=%u\nysf.fich_bad=%u\nysf.csd_good=%u\nysf.csd_bad=%u\n",
		(unsigned)s.syncs, (unsigned)s.late, (unsigned)s.fich_good, (unsigned)s.fich_bad, (unsigned)s.csd_good,
		(unsigned)s.csd_bad);
	dl_text(t, "ysf.order_all=%u\nysf.sync_bits=%u\nysf.sync_errors=%u\nysf.ppm=%d\nysf.offset_hz=%d\nysf.rx_invert=%u\n",
		(unsigned)s.order_all, (unsigned)s.sync_bits, (unsigned)s.sync_errors, (int)s.ppm, (int)s.offset_hz,
		s.rx_invert);
	dl_text(t, "ysf.rx_overs=%u\nysf.rx_frames=%u\nysf.rx_played=%u\nysf.rx_concealed=%u\nysf.rx_muted=%u\nysf.rx_vw=%u\n",
		(unsigned)s.rx_overs, (unsigned)s.rx_frames, (unsigned)s.rx_played, (unsigned)s.rx_concealed,
		(unsigned)s.rx_muted, (unsigned)s.rx_vw);
	dl_text(t, "ysf.tx_overs=%u\nysf.tx_frames=%u\nysf.tx_underruns=%u\nysf.tx_overflows=%u\nysf.tx_samples=%u\n",
		(unsigned)s.tx_overs, (unsigned)s.tx_frames, (unsigned)s.tx_underruns, (unsigned)s.tx_overflows,
		(unsigned)s.tx_samples);
	// DN's mic in the latest over: the board's peak, the encoder's after the gain, the limiter's lowest gain
	dl_text(t, "ysf.tx_mic_peak=%u\nysf.tx_enc_peak=%u\n", (unsigned)s.tx_mic_peak, (unsigned)s.tx_enc_peak);
	if (s.tx_mic_low_q8 != 0) {
		dl_text(t, "ysf.tx_mic_low_db=%d\n", miclimit_q8_db((uint16_t)s.tx_mic_low_q8));
	} else {
		dl_text(t, "ysf.tx_mic_low_db=-\n");
	}
#else
	(void)v;
	dl_text(t, "ysf.built=0\n");
#endif
}
