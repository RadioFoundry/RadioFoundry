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
	static const char *const states[] = { "off", "watch", "search", "locked" };
	static const char *const whys[] = { "none", "weak", "tx_fail", "manual", "check", "stale" };
	static const char *const blocks[] = { "none", "mode", "hotspot", "scan", "tx", "call", "reply", "quiet",
					      "override", "emergency" };
	const radio_roam_view_t *r = &v->roam;
	radio_settings_t s;

	dl_text(t, "roam.built=1\nroam.zone_on=%u\nroam.state=%s\nroam.why=%s\nroam.pending=%s\nroam.blocked=%s\n",
		r->zone_on, roam_word(r->state, states, 4), roam_word(r->why, whys, 6), roam_word(r->pending, whys, 6),
		roam_word(r->blocked, blocks, 10));
	dl_text(t, "roam.sites=%u\nroam.site=%u\nroam.home=%u\nroam.channel=%u\nroam.rssi_dbm=%d\nroam.weak=%u\n",
		r->sites, r->site, r->home, r->sites != 0 ? r->site_number[r->site] : 0, r->rssi_dbm, r->weak);
	dl_text(t, "roam.searches=%u\nroam.roams=%u\nroam.stays=%u\nroam.aborts=%u\nroam.probes=%u\nroam.triggers=%u\n",
		(unsigned)r->searches, (unsigned)r->roams, (unsigned)r->stays, (unsigned)r->aborts, (unsigned)r->probes,
		(unsigned)r->triggers);
	for (uint8_t i = 0; i < r->sites && i < RADIO_ROAM_SITES; i++) {
		dl_text(t, "site.%u=%u:%d\n", i, r->site_number[i], r->site_rssi[i]);
	}
	radio_get_settings(&s);
	dl_text(t, "set.roam_rssi_dbm=%d\nset.roam_hyst_db=%u\nset.roam_probe=%s\nset.roam_check_s=%u\n", s.roam_rssi_dbm,
		s.roam_hyst_db, s.roam_probe ? "wake" : "passive", s.roam_check_s);
#else
	(void)v;
	dl_text(t, "roam.built=0\n");
#endif
}
