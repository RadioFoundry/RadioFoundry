// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The APRS engine (aprs.h): the settings, the beacon triggers and the view. What a beacon is sent with is a backend's:
// AX.25 / AFSK on FM (aprs_ax25.c, CONFIG_APRS_AFSK) and position reports over DMR (aprs_dmr.c, CONFIG_APRS_DMR);
// either works without the other. The state and its lock are in aprs_int.h. Settings and codeplug reads take the
// storage mutex, so they happen outside LOCK().
#include <string.h>
#include "aprs/aprs_int.h"
#include "aprs/aprs_dmr.h"
#include "board.h"
#include "settings/settings.h"

#define AUTO_MIN_S	30		// hard minimum between automatic beacons
#define PTT_GAP_S	120		// "after PTT" only when the last beacon is older than this

static void nolock(void) {}
void (*aprs_lock_fn)(void) = nolock;
void (*aprs_unlock_fn)(void) = nolock;

aprs_state_t aprs_s RFOUNDRY_CPU_RAM;	// CPU only

// --- Configuration ---

void aprs_cfg_default(aprs_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->region = APRS_REGION_NA;
	c->ssid = APRS_SSID_DEFAULT;
	c->sym_table = '/';
	c->sym_code = '[';
	c->path = APRS_PATH_WIDE1_WIDE2;
	c->compressed = true;
#if CONFIG_APRS_AFSK
	c->messaging = aprs_can_receive();
#endif
	c->interval_s = APRS_INTERVAL_DEFAULT;
	c->smart = true;
	c->level = APRS_LEVEL_DEFAULT;
	c->txdelay = APRS_TXDELAY_DEFAULT;
	c->via = APRS_VIA_RF;
	c->dmr_dst = APRS_DMR_DST_DEFAULT;
	c->rx_on = c->messaging;
}

static void cfg_from_settings(const settings_t *s, aprs_cfg_t *c)
{
	aprs_cfg_default(c);
	if (s->aprs_stored != 1) {
		return;
	}
	uint8_t f = s->aprs_flags, f2 = s->aprs_flags2;
	c->tx_on = (f & SETTINGS_APRS_TX) != 0;
	c->smart = (f & SETTINGS_APRS_SMART) != 0;
	c->compressed = (f & SETTINGS_APRS_COMPRESSED) != 0;
	c->after_ptt = (f & SETTINGS_APRS_AFTER_PTT) != 0;
	c->boot = (f & SETTINGS_APRS_BOOT) != 0;
	c->kiss = (f & SETTINGS_APRS_KISS) != 0;
	c->narrow = (f & SETTINGS_APRS_NARROW) != 0;
	c->rx_on = (f & SETTINGS_APRS_RX) != 0;
	c->igate = (f2 & SETTINGS_APRS2_IGATE) != 0;
	c->igate_tx = (f2 & SETTINGS_APRS2_IGATE_TX) != 0;
	c->messaging = (f2 & SETTINGS_APRS2_MESSAGING) != 0;
	c->dmr_udt = (f2 & SETTINGS_APRS2_DMR_UDT) != 0;
	c->to_is = (f2 & SETTINGS_APRS2_TO_IS) != 0;
	c->region = s->aprs_region < APRS_REGION_COUNT ? s->aprs_region : APRS_REGION_NA;
	c->ssid = s->aprs_ssid <= 15 ? s->aprs_ssid : APRS_SSID_DEFAULT;
	c->freq_hz = s->aprs_freq_hz;
	c->dmr_dst = s->aprs_dmr_dst != 0 && s->aprs_dmr_dst <= 0xFFFFFFu ? s->aprs_dmr_dst : APRS_DMR_DST_DEFAULT;
	c->interval_s = s->aprs_interval_s;
	c->distance_m = s->aprs_distance_m;
	if (s->aprs_symbol[1] >= '!' && s->aprs_symbol[1] <= '~') {
		c->sym_table = s->aprs_symbol[0];
		c->sym_code = s->aprs_symbol[1];
	}
	c->path = s->aprs_path < APRS_PATH_COUNT ? s->aprs_path : APRS_PATH_WIDE1_WIDE2;
	c->level = s->aprs_level <= 100 ? s->aprs_level : APRS_LEVEL_DEFAULT;
	c->power = s->aprs_power;
	c->txdelay = s->aprs_txdelay != 0 ? s->aprs_txdelay : APRS_TXDELAY_DEFAULT;
	uint8_t via = (uint8_t)(s->aprs_misc & 3u);
	c->via = via < APRS_VIA_COUNT ? via : APRS_VIA_RF;
	c->digi = (uint8_t)((s->aprs_misc >> 2) & 3u);
	if (c->digi > APRS_DIGI_WIDE) {
		c->digi = APRS_DIGI_OFF;
	}
}

// The position settings share this block (the fixed position, the source, the ambiguity, the DMR GPS Info flag): the
// position service's, written by it so a first save here leaves them set
static void cfg_to_settings(const aprs_cfg_t *c, settings_t *s)
{
	position_cfg_store(s);
	s->aprs_flags = (uint8_t)((c->tx_on ? SETTINGS_APRS_TX : 0) | (c->smart ? SETTINGS_APRS_SMART : 0) |
				  (c->compressed ? SETTINGS_APRS_COMPRESSED : 0) |
				  (c->after_ptt ? SETTINGS_APRS_AFTER_PTT : 0) | (c->boot ? SETTINGS_APRS_BOOT : 0) |
				  (c->kiss ? SETTINGS_APRS_KISS : 0) | (c->narrow ? SETTINGS_APRS_NARROW : 0) |
				  (c->rx_on ? SETTINGS_APRS_RX : 0));
	s->aprs_flags2 = (uint8_t)((c->igate ? SETTINGS_APRS2_IGATE : 0) | (c->igate_tx ? SETTINGS_APRS2_IGATE_TX : 0) |
				   (c->messaging ? SETTINGS_APRS2_MESSAGING : 0) |
				   (c->dmr_udt ? SETTINGS_APRS2_DMR_UDT : 0) | (c->to_is ? SETTINGS_APRS2_TO_IS : 0) |
				   (s->aprs_flags2 & SETTINGS_APRS2_DMR_GPS));
	s->aprs_region = c->region;
	s->aprs_ssid = c->ssid;
	s->aprs_freq_hz = c->freq_hz;
	s->aprs_dmr_dst = c->dmr_dst;
	s->aprs_interval_s = c->interval_s;
	s->aprs_distance_m = c->distance_m;
	s->aprs_symbol[0] = c->sym_table;
	s->aprs_symbol[1] = c->sym_code;
	s->aprs_path = c->path;
	s->aprs_level = c->level;
	s->aprs_power = c->power;
	s->aprs_txdelay = c->txdelay;
	s->aprs_misc = (uint8_t)((s->aprs_misc & 0xF0u) | (c->via & 3u) | ((c->digi & 3u) << 2));
	s->aprs_stored = 1;
}

void aprs_cfg_store_defaults(settings_t *s)
{
	aprs_cfg_t c;
	cfg_from_settings(s, &c);
	cfg_to_settings(&c, s);
}

// The trigger settings from the configuration (lock held)
static void apply_cfg(void)
{
	const aprs_cfg_t *c = &aprs_s.cfg;
	aprs_beacon_cfg_t *b = &aprs_s.bcfg;
	memset(b, 0, sizeof(*b));
	b->auto_on = c->tx_on;
	b->interval_s = c->interval_s;
	b->smart = c->smart;
	b->fast_kmh = 90;
	b->slow_kmh = 5;
	b->fast_s = 180;
	b->slow_s = 1800;
	b->turn_deg = 28;
	b->turn_slope = 400;
	b->turn_s = 30;
	b->distance_m = c->distance_m;
	b->after_ptt = c->after_ptt;
	b->ptt_gap_s = PTT_GAP_S;
	b->boot = c->boot;
	b->min_s = AUTO_MIN_S;
}

static void reload_cfg(void)
{
	settings_t st;
	aprs_cfg_t c;
	settings_read(&st);
	cfg_from_settings(&st, &c);
	LOCK();
	aprs_s.cfg = c;
	apply_cfg();
	UNLOCK();
	aprs_ax25_identity_from(&st);
}

// The way out in use: the Send via setting where both backends are built, else the one there is
static uint8_t via_now(void)
{
#if CONFIG_APRS_AFSK && CONFIG_APRS_DMR
	return aprs_s.cfg.via;
#elif CONFIG_APRS_DMR
	return APRS_VIA_DMR;
#else
	return APRS_VIA_RF;
#endif
}

// A beacon is due for why (lock held): the AX.25 half (RF queued, APRS-IS), DMR (flagged for after the lock). True
// when anything went
static bool beacon(uint8_t why, const position_fix_t *f)
{
	uint8_t via = via_now();
	bool any = aprs_ax25_beacon(f, via != APRS_VIA_DMR);
#if CONFIG_APRS_DMR
	if (via != APRS_VIA_RF) {
		aprs_s.dmr_due = true;
		any = true;
	}
#endif
	if (any) {
		aprs_s.v.last_why = why;
		aprs_s.v.last_ms = aprs_s.now != 0 ? aprs_s.now : 1;
		aprs_changed();
	}
	return any;
}

// --- The radio task ---

void aprs_init(uint32_t now_ms)
{
	memset(&aprs_s, 0, sizeof(aprs_s));
	aprs_s.now = now_ms;
	aprs_ax25_init(now_ms);
	reload_cfg();
	LOCK();
	aprs_beacon_init(&aprs_s.bc, &aprs_s.bcfg, now_ms);
	UNLOCK();
}

void aprs_reload(void)
{
	LOCK();
	aprs_s.reload = true;
	UNLOCK();
}

void aprs_ptt_end(uint32_t now_ms)
{
	LOCK();
	aprs_beacon_ptt_end(&aprs_s.bc, &aprs_s.bcfg, now_ms);
	UNLOCK();
}

void aprs_tick(const aprs_radio_t *r, uint32_t now_ms)
{
	LOCK();
	bool reload = aprs_s.reload;
	aprs_s.reload = false;
	UNLOCK();
	if (reload) {
		reload_cfg();
	} else {
		aprs_ax25_poll(now_ms);
	}

	LOCK();
	aprs_s.now = now_ms;
	aprs_ax25_tick(now_ms);
	// The position in use (position.h), exact: the beacon formats apply the ambiguity
	position_fix_t fx;
	uint8_t src = position_fix(&fx, now_ms);
	const position_fix_t *f = src != POSITION_SRC_NONE ? &fx : NULL;
	aprs_beacon_pos_t bp;
	memset(&bp, 0, sizeof(bp));
	if (f != NULL) {
		bp.valid = true;
		bp.lat = f->lat;
		bp.lon = f->lon;
		bp.speed_kmh = f->speed_kmh;
		bp.course = f->course;
	}
	uint8_t why = aprs_beacon_due(&aprs_s.bc, &aprs_s.bcfg, &bp, now_ms);
	if (why != APRS_WHY_NONE && f != NULL) {
		if (beacon(why, f)) {
			aprs_beacon_sent(&aprs_s.bc, why, &bp, now_ms);
		} else if (why == APRS_WHY_MANUAL) {
			aprs_s.bc.manual = false;	// nowhere to send it (the view's block says why)
		}
	} else if (why == APRS_WHY_MANUAL) {
		aprs_s.bc.manual = false;
	}
	aprs_ax25_service();
	// The view
	aprs_s.v.pos_src = src;
	aprs_s.v.pos_valid = f != NULL;
	if (f != NULL) {
		if (aprs_s.v.lat != f->lat || aprs_s.v.lon != f->lon) {
			aprs_changed();
		}
		aprs_s.v.lat = f->lat;
		aprs_s.v.lon = f->lon;
		aprs_s.v.speed_kmh = f->speed_kmh;
		aprs_s.v.course = f->course;
		aprs_s.v.pos_age_s = (now_ms - f->ms) / 1000u;
	}
	aprs_s.v.next_s = aprs_beacon_next_s(&aprs_s.bc, &aprs_s.bcfg, &bp, now_ms);
#if CONFIG_APRS_DMR
	bool dmr_due = aprs_s.dmr_due;
	aprs_s.dmr_due = false;
	aprs_dmr_pos_t dp;
	memset(&dp, 0, sizeof(dp));
	if (dmr_due && f != NULL) {
		uint32_t err_m;
		dp.lat = f->lat;
		dp.lon = f->lon;
		position_blur(&dp.lat, &dp.lon, src, &err_m);	// the ambiguity setting's blur, as LRRP and GPS Info get
		dp.speed_kn = aprs_kmh_to_kn(f->speed_kmh);
		dp.course = f->course;
		dp.utc_hms = src == POSITION_SRC_FIXED ? 0xFFFFFFFFu : f->utc_hms;
		dp.date = src == POSITION_SRC_FIXED ? 0 : f->date;
	}
	bool udt = aprs_s.cfg.dmr_udt;
	uint32_t dst = aprs_s.cfg.dmr_dst;
#endif
	UNLOCK();

#if CONFIG_APRS_DMR
	if (dmr_due) {
		bool ok = r->on_dmr && aprs_dmr_send(udt, dst, r->own_dmr_id, &dp);
		LOCK();
		if (ok) {
			aprs_s.v.dmr_sent++;
		} else {
			aprs_s.v.dmr_failed++;
		}
		aprs_changed();
		UNLOCK();
	}
#endif
#if CONFIG_APRS_RX
	aprs_ax25_rx_service(r, now_ms);
#elif !CONFIG_APRS_DMR
	(void)r;
#endif
}

// --- Any task ---

void aprs_set_lock(void (*lock)(void), void (*unlock)(void))
{
	aprs_lock_fn = lock != NULL ? lock : nolock;
	aprs_unlock_fn = unlock != NULL ? unlock : nolock;
}

void aprs_get_view(aprs_view_t *v)
{
	LOCK();
	*v = aprs_s.v;
	aprs_ax25_view(v);
	UNLOCK();
}

void aprs_get_cfg(aprs_cfg_t *c)
{
	LOCK();
	*c = aprs_s.cfg;
	UNLOCK();
}

void aprs_set_cfg(const aprs_cfg_t *c, uint32_t now_ms)
{
	aprs_cfg_t v = *c;
	if (v.ssid > 15) {
		v.ssid = APRS_SSID_DEFAULT;
	}
	if (v.level > 100) {
		v.level = APRS_LEVEL_DEFAULT;
	}
	settings_t *s = settings_begin();
	cfg_to_settings(&v, s);
	settings_end(now_ms);
	LOCK();
	bool was_on = aprs_s.cfg.tx_on;
	aprs_s.cfg = v;
	apply_cfg();
	if (v.tx_on && !was_on) {
		aprs_beacon_init(&aprs_s.bc, &aprs_s.bcfg, aprs_s.now);	// turned on: the interval counts from now (boot beacon if set)
	}
	aprs_changed();
	UNLOCK();
	aprs_ax25_identity();
}

bool aprs_send_now(void)
{
	LOCK();
	bool ok = via_now() != APRS_VIA_RF || aprs_ax25_can_send();
	if (ok) {
		aprs_beacon_manual(&aprs_s.bc, aprs_s.now);
	}
	UNLOCK();
	return ok;
}

bool aprs_host_enabled(void)
{
	return aprs_s.cfg.kiss;
}
