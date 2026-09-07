// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The roam view section (docs/spec/debuglink.md 3.7, docs/spec/dmr-roaming.md 6.3): DMR repeater roaming's state,
// the sites with their last measurements, the counters and the roaming settings
#include "config.h"
#include "debuglink/debuglink.h"

#if CONFIG_DMR_ROAMING
static const char *roam_word(uint8_t v, const char *const *names, uint8_t n)
{
	return v < n ? names[v] : "?";
}
#endif

void debuglink_view_roam(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_DMR_ROAMING
	static const char *const states[] = { "off", "watch", "search", "locked", "survey" };
	static const char *const surveys[] = { "off", "sync", "site" };
	static const char *const whys[] = { "none", "weak", "tx_fail", "manual", "check", "stale" };
	static const char *const blocks[] = { "none", "mode", "hotspot", "scan", "tx", "call", "reply", "quiet",
					      "override", "emergency" };
	const radio_roam_view_t *r = &v->roam;
	radio_settings_t s;

	dl_text(t, "roam.built=1\nroam.zone_on=%u\nroam.state=%s\nroam.why=%s\nroam.pending=%s\nroam.blocked=%s\n",
		r->zone_on, roam_word(r->state, states, 5), roam_word(r->why, whys, 6), roam_word(r->pending, whys, 6),
		roam_word(r->blocked, blocks, 10));
	dl_text(t, "roam.sites=%u\nroam.site=%u\nroam.home=%u\nroam.channel=%u\nroam.rssi_dbm=%d\nroam.weak=%u\n",
		r->sites, r->site, r->home, r->sites != 0 ? r->site_number[r->site] : 0, r->rssi_dbm, r->weak);
	dl_text(t, "roam.searches=%u\nroam.roams=%u\nroam.stays=%u\nroam.aborts=%u\nroam.probes=%u\nroam.triggers=%u\n",
		(unsigned)r->searches, (unsigned)r->roams, (unsigned)r->stays, (unsigned)r->aborts, (unsigned)r->probes,
		(unsigned)r->triggers);
	for (uint8_t i = 0; i < r->sites && i < RADIO_ROAM_SITES; i++) {
		dl_text(t, "site.%u=%u:%d\n", i, r->site_number[i], r->site_rssi[i]);
	}
	// The survey (dmr-roaming.md 6.3): visit -1 = home; per site its survey average, the data's age in s (-1 never),
	// its beacon's start in the interval in ms (-1 unknown) and whether the data counts (fresh)
	dl_text(t, "roam.visit=%d\nroam.win_conf=%u\nroam.win_next_ms=%u\nroam.confirm=%d\n",
		r->visit == 0xFF ? -1 : (int)r->visit, r->win_conf, (unsigned)r->win_next_ms, (int)r->confirm - 1);
	dl_text(t, "roam.surveys=%u\nroam.visits=%u\nroam.cuts=%u\nroam.beacons=%u\nroam.confirms=%u\n",
		(unsigned)r->surveys, (unsigned)r->visits, (unsigned)r->cuts, (unsigned)r->beacons, (unsigned)r->confirms);
	for (uint8_t i = 0; i < r->sites && i < RADIO_ROAM_SITES; i++) {
		dl_text(t, "surv.%u=%d:%d:%d:%u\n", i, r->site_surv[i], r->site_age_s[i] == 0xFFFF ? -1 : (int)r->site_age_s[i],
			r->site_phase_ms[i] == UINT32_MAX ? -1 : (int)r->site_phase_ms[i], (r->site_fresh16 >> i) & 1u);
	}
	radio_get_settings(&s);
	dl_text(t, "set.roam_rssi_dbm=%d\nset.roam_hyst_db=%u\nset.roam_probe=%s\nset.roam_check_s=%u\n", s.roam_rssi_dbm,
		s.roam_hyst_db, s.roam_probe ? "wake" : "passive", s.roam_check_s);
	dl_text(t, "set.roam_beacon_s=%u\nset.roam_beacon_len_s=%u\nset.roam_survey=%s\nset.rpt_beacon=%u\n",
		s.roam_beacon_s, s.roam_beacon_len_s, roam_word(s.roam_survey, surveys, 3), s.rpt_beacon);
#else
	(void)v;
	dl_text(t, "roam.built=0\n");
#endif
}
