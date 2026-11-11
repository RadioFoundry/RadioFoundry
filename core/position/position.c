// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The position service (position.h). One state block, p, under LOCK(): the GPS driver writes it, the radio, UI and
// debug link tasks read it.
#include <string.h>
#include "position/position.h"
#include "position/nmea.h"
#include "board.h"

typedef struct {
	bool valid;
	position_fix_t f;
} fix_t;

static void nolock(void) {}
static void (*lock_fn)(void) = nolock;
static void (*unlock_fn)(void) = nolock;
#define LOCK()		lock_fn()
#define UNLOCK()	unlock_fn()

static struct {
	position_cfg_t cfg;
	fix_t host, gps;
	bool gps_off;		// Settings > Location > GPS is off: the board's fix doesn't count (position_gps_enable)
	bool gps_heard;		// the board GPS has sent a valid sentence (fix or not), last at gps_heard_ms
	uint32_t gps_heard_ms;
	uint8_t gps_sats;	// satellites used, from its last GGA
	uint16_t gps_hdop10;	// ... and its dilution x 10, 0xFFFF unknown
#if CONFIG_GPS_SKY
	nmea_sky_t sky;		// the satellites (GSV, GSA) for the GPS screen
#endif
} p;

static void clear_fix(fix_t *f)
{
	memset(f, 0, sizeof(*f));
	f->f.utc_hms = 0xFFFFFFFFu;
	f->f.alt_m = POSITION_ALT_NONE;
	f->f.speed_kmh = 0xFFFFu;
	f->f.course = 0xFFFFu;
}

void position_set_lock(void (*lock)(void), void (*unlock)(void))
{
	lock_fn = lock != NULL ? lock : nolock;
	unlock_fn = unlock != NULL ? unlock : nolock;
}

void position_init(void)
{
	LOCK();
	memset(&p, 0, sizeof(p));
	p.cfg.fix_alt_m = INT16_MIN;
	clear_fix(&p.host);
	clear_fix(&p.gps);
	p.gps_hdop10 = 0xFFFFu;
#if CONFIG_GPS_SKY
	nmea_sky_init(&p.sky);
#endif
	UNLOCK();
}

void position_set_cfg(const position_cfg_t *c)
{
	LOCK();
	p.cfg = *c;
	if (p.cfg.pref >= POSITION_PREF_COUNT) {
		p.cfg.pref = POSITION_AUTO;
	}
	if (p.cfg.ambiguity > 4) {
		p.cfg.ambiguity = 0;
	}
	UNLOCK();
}

// The position settings are stored with APRS's (docs/spec/settings-store.md), read here so they apply without APRS
void position_cfg_from_settings(const settings_t *s, position_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->pref = POSITION_AUTO;
	c->fix_alt_m = INT16_MIN;
	if (s->aprs_stored != 1) {
		return;
	}
	c->pref = s->aprs_pos_src < POSITION_PREF_COUNT ? s->aprs_pos_src : POSITION_AUTO;
	c->fix_lat = s->aprs_lat;
	c->fix_lon = s->aprs_lon;
	c->fix_alt_m = s->aprs_alt_m;
	uint8_t amb = (uint8_t)((s->aprs_misc >> 4) & 7u);
	c->ambiguity = amb <= 4 ? amb : 0;
	c->dmr_gps = (s->aprs_flags2 & SETTINGS_APRS2_DMR_GPS) != 0;
}

static bool fresh(const fix_t *f, uint32_t now_ms)
{
	return f->valid && (int32_t)(now_ms - f->f.ms) < (int32_t)POSITION_STALE_MS;	// a fix newer than now_ms too
}

static bool fixed_set(void)
{
	return p.cfg.fix_lat != 0 || p.cfg.fix_lon != 0;
}

static bool allows(uint8_t pref)
{
	return p.cfg.pref == POSITION_AUTO || p.cfg.pref == pref;
}

// The position in use (lock held)
static uint8_t choose(position_fix_t *out, uint32_t now_ms)
{
	if (allows(POSITION_GPS) && board_has_gps() && fresh(&p.gps, now_ms)) {
		*out = p.gps.f;
		return POSITION_SRC_GPS;
	}
	if (allows(POSITION_HOST) && fresh(&p.host, now_ms)) {
		*out = p.host.f;
		return POSITION_SRC_HOST;
	}
	if (allows(POSITION_FIXED) && fixed_set()) {
		out->lat = p.cfg.fix_lat;
		out->lon = p.cfg.fix_lon;
		out->alt_m = p.cfg.fix_alt_m != INT16_MIN ? p.cfg.fix_alt_m : POSITION_ALT_NONE;
		out->speed_kmh = 0xFFFFu;
		out->course = 0xFFFFu;
		out->utc_hms = 0xFFFFFFFFu;
		out->date = 0;
		out->ms = now_ms;
		return POSITION_SRC_FIXED;
	}
	return POSITION_SRC_NONE;
}

uint8_t position_fix(position_fix_t *out, uint32_t now_ms)
{
	LOCK();
	uint8_t src = choose(out, now_ms);
	UNLOCK();
	return src;
}

// The middle of the ambiguity cell (grid in 1e-7 degree, within the whole degree) the position is in
static int32_t blur(int32_t v, int32_t grid)
{
	int32_t m = v < 0 ? -v : v;
	int32_t f = m % 10000000 / grid * grid + grid / 2;
	m = m / 10000000 * 10000000 + (f < 10000000 ? f : 9999999);
	return v < 0 ? -m : m;
}

uint8_t position_report(position_fix_t *out, uint32_t *err_m, uint32_t now_ms)
{
	// Position ambiguity (APRS101 ch. 8): the same blur for DMR. Grid in 1e-7 degree and the error it means
	static const int32_t grid[] = { 1, 16667, 166667, 1666667, 10000000 };
	static const uint32_t blur_m[] = { 0, 185, 1852, 18520, 111120 };
	LOCK();
	uint8_t src = choose(out, now_ms);
	uint8_t amb = p.cfg.ambiguity;
	UNLOCK();
	if (src == POSITION_SRC_NONE) {
		return src;
	}
	*err_m = src == POSITION_SRC_FIXED ? 200u : 20u;	// GPS / host fixes: tens of meters; a typed position: less sure
	if (amb != 0) {
		out->lat = blur(out->lat, grid[amb]);
		out->lon = blur(out->lon, grid[amb]);
		*err_m = blur_m[amb];
	}
	return src;
}

bool position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps, uint32_t now_ms)
{
	position_fix_t f;
	LOCK();
	*dmr_gps = p.cfg.dmr_gps;
	UNLOCK();
	if (position_report(&f, err_m, now_ms) == POSITION_SRC_NONE) {
		return false;
	}
	*lat = f.lat;
	*lon = f.lon;
	return true;
}

void position_nmea(const char *s, size_t n, bool gps, uint32_t now_ms)
{
	nmea_fix_t x;
	bool pos = nmea_parse(s, n, &x);
	if (!pos && !(CONFIG_GPS_SKY && gps)) {
		return;
	}
	LOCK();
	if (gps && p.gps_off) {
		UNLOCK();
		return;
	}
#if CONFIG_GPS_SKY
	if (gps && nmea_sky_sentence(&p.sky, s, n)) {
		p.gps_heard = true;
		p.gps_heard_ms = now_ms;
	}
#endif
	if (!pos) {
		UNLOCK();
		return;
	}
	fix_t *f = gps ? &p.gps : &p.host;
	if (gps) {
		p.gps_heard = true;
		p.gps_heard_ms = now_ms;
		if (x.kind == NMEA_GGA) {
			p.gps_sats = x.sats;
			p.gps_hdop10 = x.hdop10;
		}
	}
	if (!x.fix) {
		f->valid = false;
	} else {
		f->valid = true;
		f->f.lat = x.lat;
		f->f.lon = x.lon;
		f->f.ms = now_ms;
		f->f.utc_hms = x.utc_hms;
		if (x.kind == NMEA_RMC) {
			f->f.speed_kmh = x.speed_kn10 == 0xFFFFu ? 0xFFFFu :
					 (uint16_t)(((uint32_t)x.speed_kn10 * 1852u + 5000u) / 10000u);
			f->f.course = x.course10 == 0xFFFFu ? 0xFFFFu : (uint16_t)((x.course10 + 5u) / 10u % 360u);
			f->f.date = x.date;
			if (f->f.speed_kmh == 0) {
				f->f.course = 0xFFFFu;	// standing still: no meaningful course
			}
		} else {
			f->f.alt_m = x.alt_dm == INT32_MIN ? POSITION_ALT_NONE : x.alt_dm / 10;
		}
	}
	UNLOCK();
}

void position_gps_enable(bool on)
{
	LOCK();
	p.gps_off = !on;
	if (!on) {
		p.gps.valid = false;
		p.gps_heard = false;
#if CONFIG_GPS_SKY
		nmea_sky_init(&p.sky);
#endif
	}
	UNLOCK();
}

bool position_gps(position_gps_t *out, uint32_t now_ms)
{
	LOCK();
	bool on = !p.gps_off && p.gps_heard && (int32_t)(now_ms - p.gps_heard_ms) < (int32_t)POSITION_STALE_MS;
	if (on) {
		out->fix = fresh(&p.gps, now_ms);
		out->sats = p.gps_sats;
		out->lat = p.gps.f.lat;
		out->lon = p.gps.f.lon;
		out->alt_m = p.gps.f.alt_m;
		out->hdop10 = p.gps_hdop10;
		out->speed_kmh = p.gps.f.speed_kmh;
		out->course = p.gps.f.course;
		out->utc_hms = p.gps.f.utc_hms;
#if CONFIG_GPS_SKY
		out->mode = p.sky.mode;
		out->in_view = p.sky.n;
#else
		out->mode = 0;
		out->in_view = 0;
#endif
	}
	UNLOCK();
	return on;
}

#if CONFIG_GPS_SKY
bool position_gps_sat(uint8_t i, sky_sat_t *out, bool *used)
{
	LOCK();
	bool ok = i < p.sky.n;
	if (ok) {
		*out = p.sky.sat[i];
		*used = nmea_sky_used(&p.sky, out);
	}
	UNLOCK();
	return ok;
}
#endif
