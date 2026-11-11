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
#if CONFIG_POSITION_HOST
	fix_t host;
#endif
#if CONFIG_GPS
	fix_t gps;
	bool gps_off;		// Settings > Location > GPS is off: the board's fix doesn't count (position_gps_enable)
	bool gps_heard;		// the board GPS has sent a valid sentence (fix or not), last at gps_heard_ms
	uint32_t gps_heard_ms;
	uint8_t gps_sats;	// satellites used, from its last GGA
	uint16_t gps_hdop10;	// ... and its dilution x 10, 0xFFFF unknown
#if CONFIG_GPS_SKY
	nmea_sky_t sky;		// the satellites (GSV, GSA) for the GPS screen
#endif
#endif
} p;

#if CONFIG_GPS || CONFIG_POSITION_HOST
static void clear_fix(fix_t *f)
{
	memset(f, 0, sizeof(*f));
	f->f.utc_hms = 0xFFFFFFFFu;
	f->f.alt_m = POSITION_ALT_NONE;
	f->f.speed_kmh = 0xFFFFu;
	f->f.course = 0xFFFFu;
}

static bool fresh(const fix_t *f, uint32_t now_ms)
{
	return f->valid && (int32_t)(now_ms - f->f.ms) < (int32_t)POSITION_STALE_MS;	// a fix newer than now_ms too
}
#endif

void position_set_lock(void (*lock)(void), void (*unlock)(void))
{
	lock_fn = lock != NULL ? lock : nolock;
	unlock_fn = unlock != NULL ? unlock : nolock;
}

static void cfg_default(position_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->pref = POSITION_AUTO;
	c->fix_alt_m = INT16_MIN;
}

void position_init(void)
{
	LOCK();
	memset(&p, 0, sizeof(p));
	cfg_default(&p.cfg);
#if CONFIG_POSITION_HOST
	clear_fix(&p.host);
#endif
#if CONFIG_GPS
	clear_fix(&p.gps);
	p.gps_hdop10 = 0xFFFFu;
#if CONFIG_GPS_SKY
	nmea_sky_init(&p.sky);
#endif
#endif
	UNLOCK();
}

// The position settings sit in the settings store's APRS block (docs/spec/settings-store.md), where they were first
// kept, with a marker of their own: they are there without APRS, and a record APRS wrote before the marker existed
// still counts
static void cfg_from_settings(const settings_t *s, position_cfg_t *c)
{
	cfg_default(c);
	if ((s->aprs_misc & SETTINGS_APRS_MISC_POSITION) == 0 && s->aprs_stored != 1) {
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

static void cfg_to_settings(const position_cfg_t *c, settings_t *s)
{
	s->aprs_pos_src = c->pref;
	s->aprs_lat = c->fix_lat;
	s->aprs_lon = c->fix_lon;
	s->aprs_alt_m = c->fix_alt_m;
	s->aprs_misc = (uint8_t)((s->aprs_misc & 0x0Fu) | ((c->ambiguity & 7u) << 4) | SETTINGS_APRS_MISC_POSITION);
	s->aprs_flags2 = (uint8_t)((s->aprs_flags2 & ~SETTINGS_APRS2_DMR_GPS) | (c->dmr_gps ? SETTINGS_APRS2_DMR_GPS : 0));
}

void position_load(const settings_t *s)
{
	position_cfg_t c;
	cfg_from_settings(s, &c);
	LOCK();
	p.cfg = c;
	UNLOCK();
}

void position_get_cfg(position_cfg_t *c)
{
	LOCK();
	*c = p.cfg;
	UNLOCK();
}

void position_set_cfg(const position_cfg_t *c, uint32_t now_ms)
{
	position_cfg_t v = *c;
	if (v.pref >= POSITION_PREF_COUNT) {
		v.pref = POSITION_AUTO;
	}
	if (v.ambiguity > 4) {
		v.ambiguity = 4;
	}
	settings_t *s = settings_begin();
	cfg_to_settings(&v, s);
	settings_end(now_ms);
	LOCK();
	p.cfg = v;
	UNLOCK();
}

void position_cfg_store(settings_t *s)
{
	position_cfg_t c;
	position_get_cfg(&c);
	cfg_to_settings(&c, s);
}

uint8_t position_ambiguity(void)
{
	return p.cfg.ambiguity;		// one byte: no lock
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
#if CONFIG_GPS
	if (allows(POSITION_GPS) && board_has_gps() && fresh(&p.gps, now_ms)) {
		*out = p.gps.f;
		return POSITION_SRC_GPS;
	}
#endif
#if CONFIG_POSITION_HOST
	if (allows(POSITION_HOST) && fresh(&p.host, now_ms)) {
		*out = p.host.f;
		return POSITION_SRC_HOST;
	}
#endif
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

// The position for a DMR report and the "send in DMR calls" setting, in one locked read
static uint8_t report(position_fix_t *out, uint32_t *err_m, bool *dmr_gps, uint32_t now_ms)
{
	// Position ambiguity (APRS101 ch. 8): the same blur for DMR. Grid in 1e-7 degree and the error it means
	static const int32_t grid[] = { 1, 16667, 166667, 1666667, 10000000 };
	static const uint32_t blur_m[] = { 0, 185, 1852, 18520, 111120 };
	LOCK();
	uint8_t src = choose(out, now_ms);
	uint8_t amb = p.cfg.ambiguity;
	*dmr_gps = p.cfg.dmr_gps;
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

uint8_t position_report(position_fix_t *out, uint32_t *err_m, uint32_t now_ms)
{
	bool dmr_gps;
	return report(out, err_m, &dmr_gps, now_ms);
}

bool position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps, uint32_t now_ms)
{
	position_fix_t f;
	if (report(&f, err_m, dmr_gps, now_ms) == POSITION_SRC_NONE) {
		return false;
	}
	*lat = f.lat;
	*lon = f.lon;
	return true;
}

#if CONFIG_GPS || CONFIG_POSITION_HOST
void position_nmea(const char *s, size_t n, bool gps, uint32_t now_ms)
{
	nmea_fix_t x;
	bool pos = nmea_parse(s, n, &x);
#if !CONFIG_GPS
	if (gps) {
		return;
	}
#elif !CONFIG_POSITION_HOST
	if (!gps) {
		return;
	}
#endif
	if (!pos && !(CONFIG_GPS_SKY && gps)) {
		return;
	}
	LOCK();
#if CONFIG_GPS
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
	if (gps && pos) {
		p.gps_heard = true;
		p.gps_heard_ms = now_ms;
		if (x.kind == NMEA_GGA) {
			p.gps_sats = x.sats;
			p.gps_hdop10 = x.hdop10;
		}
	}
#endif
	if (!pos) {
		UNLOCK();
		return;
	}
#if CONFIG_GPS && CONFIG_POSITION_HOST
	fix_t *f = gps ? &p.gps : &p.host;
#elif CONFIG_GPS
	fix_t *f = &p.gps;
#else
	fix_t *f = &p.host;
#endif
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
#endif

#if CONFIG_GPS
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
#endif
