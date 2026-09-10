// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The APRS engine (aprs.h). One state block, a: the radio task changes it in aprs_tick / aprs_rf_* / aprs_rx_*, other
// tasks through the any-task calls; every access is under LOCK() (the scheduler held: short copies only). Settings
// and codeplug reads take the storage mutex, so they happen outside LOCK().
#include <string.h>
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"
#include "aprs/aprs_beacon.h"
#include "aprs/aprs_nmea.h"
#include "aprs/aprs_igate.h"
#include "aprs/aprs_dmr.h"
#include "aprs/hdlc.h"
#include "aprs/kiss.h"
#include "aprs/afsk.h"
#include "board.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"

#define QUEUE_LEN	2		// frames waiting for the air
#define TONES_BYTES	512		// one transmission: preamble, frames, tail (4096 symbols, 3.4 s)
#define TAIL_FLAGS	3
#define MIN_FLAGS	8
#define RF_TRIES	3		// a frame given up after this many failed transmissions
#define RF_RETRY_MS	15000u		// after a failed transmission
#define POS_STALE_MS	60000u		// a GPS / host fix older than this doesn't count
#define AUTO_MIN_S	30		// hard minimum between automatic beacons
#define PTT_GAP_S	120		// "after PTT" only when the last beacon is older than this
#define HOST_BURST	3		// KISS frames from the host: a burst of 3, then one per HOST_REFILL_MS
#define HOST_REFILL_MS	10000u
#if CONFIG_APRS_RX
#define OUT_RING	512		// KISS bytes for the host: received frames, iGate lines
#else
#define OUT_RING	256		// only our own lines to APRS-IS
#endif
#define MSG_RETRY_MS	30000u		// message retries at 30, 60, 120 s
#define MSG_DUP_MS	30000u
#define IDENT_CHECK_MS	5000u

enum { K_BEACON, K_STATUS, K_MSG, K_ACK, K_HOST, K_GATE, K_DIGI };

typedef struct {
	uint16_t len;
	uint8_t kind;
	uint8_t tries;
	uint8_t data[AX25_FRAME_MAX];
} qframe_t;

typedef struct {
	bool valid;
	int32_t lat, lon;
	int32_t alt_m;		// APRS_ALT_NONE
	uint16_t speed_kmh;	// 0xFFFF
	uint16_t course;	// 0..359, 0xFFFF
	uint32_t utc_hms;	// 0xFFFFFFFF
	uint32_t date;
	uint32_t ms;		// last update
} fix_t;

static void nolock(void) {}
static void (*lock_fn)(void) = nolock;
static void (*unlock_fn)(void) = nolock;
#define LOCK()		lock_fn()
#define UNLOCK()	unlock_fn()

static struct {
	aprs_cfg_t cfg;
	aprs_beacon_cfg_t bcfg;
	aprs_beacon_t bc;
	char comment[CODEPLUG_APRS_COMMENT + 1];	// from the codeplug's APRS configuration 1
	char test_call[AX25_CALL_LEN + 4];
	ax25_addr_t me;
	bool me_ok;
	uint8_t block;
	uint32_t now;
	fix_t host, gps;
	qframe_t q[QUEUE_LEN];
	uint8_t qn;
	uint8_t taken;		// frames in the transmission under way
	uint32_t rf_next_ms;	// no transmission before
	uint8_t tones[TONES_BYTES];
	bool dmr_due;		// a digital beacon to send (outside the lock)
	struct {
		bool active;
		char to[AX25_CALL_LEN + 4];
		char text[APRS_MSG_TEXT_MAX + 1];
		char msgno[APRS_MSGNO_MAX + 1];
		uint8_t tries;
		uint32_t next_ms;
		bool acked;
	} out;
	uint16_t msg_seq;
	uint8_t ring[OUT_RING];
	uint16_t r_head, r_len;
	uint8_t host_tokens;
	uint32_t host_refill_ms;
	bool reload;
	uint32_t ident_ms;
	uint32_t cp_serial;
	char info[AX25_INFO_MAX + 1];	// scratch for information fields (under the lock)
	aprs_view_t v;
#if CONFIG_APRS_RX
	aprs_station_t st[APRS_STATIONS];
	uint8_t nst;
	aprs_msg_t msgs[APRS_MESSAGES];
	uint8_t nmsg;
	aprs_dedup_t dedup_gate, dedup_digi, dedup_is;
	afsk_demod_t demod;
	bool rx_running;
	int16_t rxbuf[160];
	char gate[AX25_TNC2_MAX + 20];
#endif
} a;

static void changed(void);
static uint8_t rf_block(void);

// --- Configuration ---

static const uint32_t region_hz[APRS_REGION_COUNT] = {
	144390000u, 144800000u, 145175000u, 144575000u, 144660000u, 144640000u, 0u,
};
static const char *const region_names[APRS_REGION_COUNT] = { "N America", "Europe", "Australia", "New Zealand",
							      "Japan", "China", "Custom" };
static const char *const path_texts[APRS_PATH_COUNT] = { "", "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE2-1", "WIDE2-2" };

uint32_t aprs_region_hz(uint8_t region)
{
	return region < APRS_REGION_COUNT ? region_hz[region] : 0u;
}

const char *aprs_region_name(uint8_t region)
{
	return region < APRS_REGION_COUNT ? region_names[region] : "?";
}

const char *aprs_path_text(uint8_t path)
{
	return path < APRS_PATH_COUNT ? path_texts[path] : "";
}

uint32_t aprs_cfg_freq(const aprs_cfg_t *c)
{
	return c->region == APRS_REGION_CUSTOM ? c->freq_hz : aprs_region_hz(c->region);
}

bool aprs_amateur_hz(uint32_t hz)
{
	return (hz >= 144000000u && hz <= 148000000u) || (hz >= 222000000u && hz <= 225000000u) ||
	       (hz >= 420000000u && hz <= 450000000u);
}

bool aprs_callsign_ok(const char *call)
{
	size_t n = strlen(call);
	bool digit = false, letter = false;
	if (n < 3 || n > AX25_CALL_LEN || strcmp(call, "N0CALL") == 0 || strcmp(call, "NOCALL") == 0) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = call[i];
		if (c >= '0' && c <= '9') {
			digit = digit || i > 0;
		} else if (c >= 'A' && c <= 'Z') {
			letter = true;
		} else {
			return false;
		}
	}
	return digit && letter;
}

void aprs_cfg_default(aprs_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->region = APRS_REGION_NA;
	c->ssid = APRS_SSID_DEFAULT;
	c->sym_table = '/';
	c->sym_code = '[';
	c->path = APRS_PATH_WIDE1_WIDE2;
	c->compressed = true;
	c->messaging = CONFIG_APRS_RX && board.afsk != NULL && board.afsk->rx_start != NULL;
	c->pos_src = APRS_POS_AUTO;
	c->fix_alt_m = INT16_MIN;
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
	c->dmr_gps = (f2 & SETTINGS_APRS2_DMR_GPS) != 0;
	c->region = s->aprs_region < APRS_REGION_COUNT ? s->aprs_region : APRS_REGION_NA;
	c->ssid = s->aprs_ssid <= 15 ? s->aprs_ssid : APRS_SSID_DEFAULT;
	c->freq_hz = s->aprs_freq_hz;
	c->fix_lat = s->aprs_lat;
	c->fix_lon = s->aprs_lon;
	c->fix_alt_m = s->aprs_alt_m;
	c->dmr_dst = s->aprs_dmr_dst != 0 && s->aprs_dmr_dst <= 0xFFFFFFu ? s->aprs_dmr_dst : APRS_DMR_DST_DEFAULT;
	c->interval_s = s->aprs_interval_s;
	c->distance_m = s->aprs_distance_m;
	if (s->aprs_symbol[1] >= '!' && s->aprs_symbol[1] <= '~') {
		c->sym_table = s->aprs_symbol[0];
		c->sym_code = s->aprs_symbol[1];
	}
	c->path = s->aprs_path < APRS_PATH_COUNT ? s->aprs_path : APRS_PATH_WIDE1_WIDE2;
	c->pos_src = s->aprs_pos_src < APRS_POS_COUNT ? s->aprs_pos_src : APRS_POS_AUTO;
	c->level = s->aprs_level <= 100 ? s->aprs_level : APRS_LEVEL_DEFAULT;
	c->txdelay = s->aprs_txdelay != 0 ? s->aprs_txdelay : APRS_TXDELAY_DEFAULT;
	uint8_t via = (uint8_t)(s->aprs_misc & 3u);
	c->via = via < APRS_VIA_COUNT ? via : APRS_VIA_RF;
	c->digi = (uint8_t)((s->aprs_misc >> 2) & 3u);
	if (c->digi > APRS_DIGI_WIDE) {
		c->digi = APRS_DIGI_OFF;
	}
	uint8_t amb = (uint8_t)((s->aprs_misc >> 4) & 7u);
	c->ambiguity = amb <= 4 ? amb : 0;
}

static void cfg_to_settings(const aprs_cfg_t *c, settings_t *s)
{
	s->aprs_flags = (uint8_t)((c->tx_on ? SETTINGS_APRS_TX : 0) | (c->smart ? SETTINGS_APRS_SMART : 0) |
				  (c->compressed ? SETTINGS_APRS_COMPRESSED : 0) |
				  (c->after_ptt ? SETTINGS_APRS_AFTER_PTT : 0) | (c->boot ? SETTINGS_APRS_BOOT : 0) |
				  (c->kiss ? SETTINGS_APRS_KISS : 0) | (c->narrow ? SETTINGS_APRS_NARROW : 0) |
				  (c->rx_on ? SETTINGS_APRS_RX : 0));
	s->aprs_flags2 = (uint8_t)((c->igate ? SETTINGS_APRS2_IGATE : 0) | (c->igate_tx ? SETTINGS_APRS2_IGATE_TX : 0) |
				   (c->messaging ? SETTINGS_APRS2_MESSAGING : 0) |
				   (c->dmr_udt ? SETTINGS_APRS2_DMR_UDT : 0) | (c->to_is ? SETTINGS_APRS2_TO_IS : 0) |
				   (c->dmr_gps ? SETTINGS_APRS2_DMR_GPS : 0));
	s->aprs_region = c->region;
	s->aprs_ssid = c->ssid;
	s->aprs_freq_hz = c->freq_hz;
	s->aprs_lat = c->fix_lat;
	s->aprs_lon = c->fix_lon;
	s->aprs_dmr_dst = c->dmr_dst;
	s->aprs_alt_m = c->fix_alt_m;
	s->aprs_interval_s = c->interval_s;
	s->aprs_distance_m = c->distance_m;
	s->aprs_symbol[0] = c->sym_table;
	s->aprs_symbol[1] = c->sym_code;
	s->aprs_path = c->path;
	s->aprs_pos_src = c->pos_src;
	s->aprs_level = c->level;
	s->aprs_txdelay = c->txdelay;
	s->aprs_misc = (uint8_t)((c->via & 3u) | ((c->digi & 3u) << 2) | ((c->ambiguity & 7u) << 4));
	s->aprs_stored = 1;
}

// The trigger settings from the configuration (lock held)
static void apply_cfg(void)
{
	const aprs_cfg_t *c = &a.cfg;
	aprs_beacon_cfg_t *b = &a.bcfg;
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

// Who we are: the debug link's test call, the settings' radio name, the codeplug's callsign; its first word, upper
// case, without an SSID (ours comes from the configuration). Reads storage: outside the lock
static void load_identity(void)
{
	char name[CODEPLUG_NAME_LEN + 1];
	settings_t st;
	codeplug_aprs_t cp;
	settings_read(&st);
	bool have_cp = codeplug_aprs(1, &cp);
	LOCK();
	memcpy(name, a.test_call, sizeof(a.test_call));
	UNLOCK();
	if (name[0] == '\0') {
		memcpy(name, st.radio_name, SETTINGS_NAME_LEN);
		name[SETTINGS_NAME_LEN] = '\0';
	}
	if (name[0] == '\0') {
		codeplug_radio_name(name);
	}
	char call[AX25_CALL_LEN + 1];
	size_t n = 0;
	const char *p = name;
	while (*p == ' ') {
		p++;
	}
	while (p[n] != '\0' && p[n] != ' ' && p[n] != '-' && n < sizeof(call) - 1u) {
		char ch = p[n];
		call[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
	}
	call[n] = '\0';
	bool long_name = p[n] != '\0' && p[n] != ' ' && p[n] != '-';
	LOCK();
	a.me_ok = !long_name && aprs_callsign_ok(call);
	memset(&a.me, 0, sizeof(a.me));
	if (a.me_ok) {
		memcpy(a.me.call, call, n + 1u);
		a.me.ssid = a.cfg.ssid;
	}
	a.comment[0] = '\0';
	if (have_cp) {
		memcpy(a.comment, cp.comment, sizeof(a.comment));
	}
	a.block = rf_block();
	changed();
	UNLOCK();
	a.cp_serial = codeplug_serial();
}

static void reload_cfg(void)
{
	settings_t st;
	aprs_cfg_t c;
	settings_read(&st);
	cfg_from_settings(&st, &c);
	LOCK();
	a.cfg = c;
	apply_cfg();
	UNLOCK();
	load_identity();
}

// --- Small helpers (lock held) ---

static void changed(void)
{
	a.v.serial++;
}

static bool band_tx(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (board.bands[i].tx && hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return true;
		}
	}
	return false;
}

static uint8_t rf_block(void)
{
	uint32_t hz = aprs_cfg_freq(&a.cfg);
	if (!a.me_ok) {
		return APRS_NO_CALL;
	}
	if (hz == 0) {
		return APRS_NO_FREQ;
	}
	if (!aprs_amateur_hz(hz) || !band_tx(hz)) {
		return APRS_NOT_AMATEUR;
	}
	if (board.afsk == NULL || board.afsk->tx_start == NULL) {
		return APRS_NO_AFSK;
	}
	return APRS_OK;
}

static bool fresh(const fix_t *f)
{
	return f->valid && (a.now - f->ms) < POS_STALE_MS;
}

// The position in use (lock held)
static const fix_t *position(uint8_t *src)
{
	static fix_t fixed;
	bool gps_ok = (board.caps & CAP_GPS) != 0 && fresh(&a.gps);
	bool host_ok = fresh(&a.host);
	bool fixed_ok = a.cfg.fix_lat != 0 || a.cfg.fix_lon != 0;
	uint8_t s = a.cfg.pos_src;
	if ((s == APRS_POS_AUTO || s == APRS_POS_GPS) && gps_ok) {
		*src = APRS_SRC_GPS;
		return &a.gps;
	}
	if ((s == APRS_POS_AUTO || s == APRS_POS_HOST) && host_ok) {
		*src = APRS_SRC_HOST;
		return &a.host;
	}
	if ((s == APRS_POS_AUTO || s == APRS_POS_FIXED) && fixed_ok) {
		memset(&fixed, 0, sizeof(fixed));
		fixed.valid = true;
		fixed.lat = a.cfg.fix_lat;
		fixed.lon = a.cfg.fix_lon;
		fixed.alt_m = a.cfg.fix_alt_m != INT16_MIN ? a.cfg.fix_alt_m : APRS_ALT_NONE;
		fixed.speed_kmh = 0xFFFFu;
		fixed.course = 0xFFFFu;
		fixed.utc_hms = 0xFFFFFFFFu;
		fixed.ms = a.now;
		*src = APRS_SRC_FIXED;
		return &fixed;
	}
	*src = APRS_SRC_NONE;
	return NULL;
}

static uint16_t kmh_to_kn(uint16_t kmh)
{
	return kmh == 0xFFFFu ? APRS_SPEED_NONE : (uint16_t)(((uint32_t)kmh * 1000u + 926u) / 1852u);
}

// Queue an AX.25 UI frame from us (lock held); path: a comma list or "" / NULL
static bool queue_frame(uint8_t kind, const char *path, const char *info, size_t n)
{
	if (a.qn >= QUEUE_LEN || !a.me_ok) {
		return false;
	}
	ax25_frame_t f;
	memset(&f, 0, sizeof(f));
	ax25_addr_parse(APRS_TOCALL, sizeof(APRS_TOCALL) - 1u, &f.dst);
	f.dst.h = true;		// command (AX.25 2.2 6.1.2: C bit set in the destination, clear in the source)
	f.src = a.me;
	f.src.h = false;
	while (path != NULL && *path != '\0' && f.ndigi < AX25_MAX_DIGIS) {
		const char *e = strchr(path, ',');
		size_t l = e != NULL ? (size_t)(e - path) : strlen(path);
		if (ax25_addr_parse(path, l, &f.digi[f.ndigi])) {
			f.digi[f.ndigi].h = false;
			f.ndigi++;
		}
		path = e != NULL ? e + 1 : NULL;
	}
	f.control = AX25_CONTROL_UI;
	f.pid = AX25_PID_NONE;
	f.info = (const uint8_t *)info;
	f.info_len = (uint16_t)n;
	qframe_t *q = &a.q[a.qn];
	q->len = (uint16_t)ax25_encode(&f, q->data, sizeof(q->data));
	if (q->len == 0) {
		return false;
	}
	q->kind = kind;
	q->tries = 0;
	a.qn++;
	changed();
	return true;
}

// Raw frame (digipeated, from the host) into the queue (lock held)
static bool queue_raw(uint8_t kind, const uint8_t *frame, size_t n)
{
	if (a.qn >= QUEUE_LEN || n > AX25_FRAME_MAX) {
		return false;
	}
	qframe_t *q = &a.q[a.qn++];
	memcpy(q->data, frame, n);
	q->len = (uint16_t)n;
	q->kind = kind;
	q->tries = 0;
	changed();
	return true;
}

static void queue_drop(uint8_t count)
{
	if (count > a.qn) {
		count = a.qn;
	}
	memmove(&a.q[0], &a.q[count], (size_t)(a.qn - count) * sizeof(qframe_t));
	a.qn = (uint8_t)(a.qn - count);
}

// --- Host output (lock held) ---

static bool ring_put(uint8_t b)
{
	if (a.r_len >= OUT_RING) {
		return false;
	}
	a.ring[(a.r_head + a.r_len) % OUT_RING] = b;
	a.r_len++;
	return true;
}

// One KISS frame into the ring, whole or not at all
static void host_emit(uint8_t type, const uint8_t *data, size_t n)
{
	if (!a.cfg.kiss) {
		return;
	}
	size_t need = 3u;
	for (size_t i = 0; i < n; i++) {
		need += (data[i] == KISS_FEND || data[i] == KISS_FESC) ? 2u : 1u;
	}
	if (need + (type == KISS_FEND || type == KISS_FESC ? 1u : 0u) > (size_t)(OUT_RING - a.r_len)) {
		a.v.host_dropped++;
		return;
	}
	ring_put(KISS_FEND);
	ring_put(type);
	for (size_t i = 0; i < n; i++) {
		if (data[i] == KISS_FEND) {
			ring_put(KISS_FESC);
			ring_put(KISS_TFEND);
		} else if (data[i] == KISS_FESC) {
			ring_put(KISS_FESC);
			ring_put(KISS_TFESC);
		} else {
			ring_put(data[i]);
		}
	}
	ring_put(KISS_FEND);
	a.v.host_out++;
}

// An APRS-IS line to the host helper: SetHardware, 'G', the line
static void host_is_line(const char *line, size_t n)
{
	uint8_t buf[1 + AX25_TNC2_MAX];
	if (n + 1u > sizeof(buf)) {
		return;
	}
	buf[0] = KISS_TAG_IS_LINE;
	memcpy(buf + 1, line, n);
	host_emit(KISS_CMD_SETHW, buf, n + 1u);
}

// Our own packet straight to APRS-IS: "CALL>APZODR,TCPIP*:info"
static void own_to_is(const char *info, size_t n)
{
	if (!a.cfg.to_is || !a.cfg.kiss || !a.me_ok) {
		return;
	}
	char line[AX25_CALL_LEN + 4 + 20 + AX25_INFO_MAX];
	size_t k = ax25_addr_format(&a.me, false, line);
	static const char mid[] = ">" APRS_TOCALL ",TCPIP*:";
	if (k + sizeof(mid) - 1u + n > sizeof(line)) {
		return;
	}
	memcpy(line + k, mid, sizeof(mid) - 1u);
	k += sizeof(mid) - 1u;
	memcpy(line + k, info, n);
	host_is_line(line, k + n);
}

// --- Beacons (lock held) ---

static size_t build_beacon(const fix_t *f)
{
	aprs_report_t r;
	memset(&r, 0, sizeof(r));
	r.pos.lat = f->lat;
	r.pos.lon = f->lon;
	r.pos.course = f->course == 0xFFFFu ? APRS_COURSE_NONE : (f->course == 0 ? 360u : f->course);
	r.pos.speed_kn = kmh_to_kn(f->speed_kmh);
	r.pos.alt_m = f->alt_m;
	r.pos.sym_table = a.cfg.sym_table;
	r.pos.sym_code = a.cfg.sym_code;
	r.compressed = a.cfg.compressed;
	r.messaging = a.cfg.messaging;
	r.ambiguity = a.cfg.compressed ? 0 : a.cfg.ambiguity;
	r.comment = a.comment;
	return aprs_build_position(&r, a.info, sizeof(a.info));
}

static void note_info(size_t n)
{
	size_t k = n < sizeof(a.v.last_info) - 1u ? n : sizeof(a.v.last_info) - 1u;
	memcpy(a.v.last_info, a.info, k);
	a.v.last_info[k] = '\0';
}

// A beacon is due for why: RF (queued), APRS-IS, DMR (flagged for after the lock). True when anything went
static bool beacon(uint8_t why, const fix_t *f)
{
	bool any = false;
	size_t n = build_beacon(f);
	if (n == 0) {
		return false;
	}
	note_info(n);
	if (a.cfg.via != APRS_VIA_DMR && a.block == APRS_OK && queue_frame(K_BEACON, aprs_path_text(a.cfg.path), a.info, n)) {
		any = true;
	}
	if (a.cfg.to_is && a.cfg.kiss && a.me_ok) {
		own_to_is(a.info, n);
		any = true;
	}
	if (a.cfg.via != APRS_VIA_RF) {
		a.dmr_due = true;
		any = true;
	}
	if (any) {
		a.v.last_why = why;
		a.v.last_ms = a.now != 0 ? a.now : 1;
		changed();
	}
	return any;
}

static void message_service(void)
{
	if (!a.out.active || a.block != APRS_OK || (int32_t)(a.now - a.out.next_ms) < 0) {
		return;
	}
	if (a.out.acked || a.out.tries >= APRS_MSG_TRIES) {
		a.out.active = false;
		changed();
		return;
	}
	size_t n = aprs_build_message(a.out.to, a.out.text, a.out.msgno, a.info, sizeof(a.info));
	if (n != 0 && queue_frame(K_MSG, aprs_path_text(a.cfg.path), a.info, n)) {
		own_to_is(a.info, n);
		a.out.tries++;
		a.out.next_ms = a.now + (MSG_RETRY_MS << (a.out.tries - 1u));
		changed();
	}
}

// --- Receive ---

#if CONFIG_APRS_RX
static void addr_text(const ax25_addr_t *x, char *out)
{
	ax25_addr_format(x, false, out);
}

static aprs_station_t *station_find(const char *call)
{
	for (uint8_t i = 0; i < a.nst; i++) {
		if (strcmp(a.st[i].call, call) == 0) {
			return &a.st[i];
		}
	}
	return NULL;
}

static bool heard(const char *call, uint8_t ssid, uint32_t secs, bool direct_only)
{
	char key[AX25_CALL_LEN + 4];
	ax25_addr_t x;
	memset(&x, 0, sizeof(x));
	memcpy(x.call, call, strnlen(call, AX25_CALL_LEN));
	x.ssid = ssid;
	addr_text(&x, key);
	const aprs_station_t *s = station_find(key);
	return s != NULL && (!direct_only || s->direct) && (a.now - s->heard_ms) / 1000u < secs;
}

static bool heard_direct(const char *call, uint8_t ssid, uint32_t secs, void *ctx)
{
	(void)ctx;
	return heard(call, ssid, secs, true);
}

static bool heard_rf(const char *call, uint8_t ssid, uint32_t secs, void *ctx)
{
	(void)ctx;
	return heard(call, ssid, secs, false);
}

// The station's entry, moved to the top (a new one at the top, the oldest dropped when full)
static aprs_station_t *station_touch(const char *call)
{
	aprs_station_t *s = station_find(call);
	if (s == NULL) {
		if (a.nst < APRS_STATIONS) {
			a.nst++;
		}
		memmove(&a.st[1], &a.st[0], (size_t)(a.nst - 1u) * sizeof(aprs_station_t));
		s = &a.st[0];
		memset(s, 0, sizeof(*s));
		memcpy(s->call, call, strnlen(call, sizeof(s->call) - 1u));
	} else if (s != &a.st[0]) {
		aprs_station_t t = *s;	// most recent first
		memmove(&a.st[1], &a.st[0], (size_t)(s - &a.st[0]) * sizeof(aprs_station_t));
		a.st[0] = t;
		s = &a.st[0];
	}
	return s;
}

static void station_update(const ax25_frame_t *f, const aprs_parsed_t *p)
{
	char call[AX25_CALL_LEN + 4];
	addr_text(&f->src, call);
	aprs_station_t *s = station_touch(call);
	bool direct = true;
	for (uint8_t i = 0; i < f->ndigi; i++) {
		if (f->digi[i].h) {
			direct = false;
		}
	}
	s->heard_ms = a.now;
	s->direct = direct;
	s->type = p->type;
	s->count++;
	if (p->has_pos) {
		s->has_pos = true;
		s->lat = p->pos.lat;
		s->lon = p->pos.lon;
		s->sym_table = p->pos.sym_table;
		s->sym_code = p->pos.sym_code;
		s->course = p->pos.course;
		s->speed_kn = p->pos.speed_kn;
	}
	if (p->text != NULL && p->type != APRS_T_MESSAGE) {
		size_t k = p->text_len < sizeof(s->text) - 1u ? p->text_len : sizeof(s->text) - 1u;
		memcpy(s->text, p->text, k);
		s->text[k] = '\0';
	}
}

void aprs_dmr_heard(uint32_t id, const char *alias, int32_t lat, int32_t lon, uint32_t now_ms)
{
	// The alias's first word when it is a callsign, else the DMR ID (AX.25 calls are 6 characters plus the SSID)
	char call[AX25_CALL_LEN + 4];
	size_t n = 0;
	while (alias != NULL && alias[n] != '\0' && alias[n] != ' ' && n < AX25_CALL_LEN) {
		call[n] = alias[n] >= 'a' && alias[n] <= 'z' ? (char)(alias[n] - 32) : alias[n];
		n++;
	}
	call[n] = '\0';
	if (n == 0 || (alias[n] != '\0' && alias[n] != ' ') || !aprs_callsign_ok(call)) {
		uint32_t v = id % 100000000u;
		char d[9];
		uint8_t k = 0;
		do {
			d[k++] = (char)('0' + v % 10u);
			v /= 10u;
		} while (v != 0 && k < 8);
		for (n = 0; n < k; n++) {
			call[n] = d[k - 1u - n];
		}
		call[n] = '\0';
	}
	LOCK();
	a.now = now_ms;
	aprs_station_t *s = station_find(call);
	bool same = s != NULL && s->has_pos && s->lat == lat && s->lon == lon && now_ms - s->heard_ms < 30000u;
	if (!same) {
		s = station_touch(call);
		s->heard_ms = now_ms;
		s->direct = true;
		s->type = APRS_T_POSITION;
		s->count++;
		s->has_pos = true;
		s->lat = lat;
		s->lon = lon;
		s->sym_table = '/';
		s->sym_code = '[';
		s->course = s->speed_kn = 0xFFFFu;
		memcpy(s->text, "DMR", 4);
		a.v.stations = a.nst;
		changed();
	}
	UNLOCK();
}

static void message_in(const ax25_frame_t *f, const aprs_parsed_t *p)
{
	char from[AX25_CALL_LEN + 4], me[AX25_CALL_LEN + 4];
	addr_text(&f->src, from);
	addr_text(&a.me, me);
	if (strcmp(p->addressee, me) != 0) {
		return;
	}
	if (p->ack || p->rej) {
		if (a.out.active && strcmp(a.out.to, from) == 0 && strcmp(a.out.msgno, p->msgno) == 0) {
			a.out.acked = true;
			a.out.next_ms = a.now;
			changed();
		}
		return;
	}
	bool dup = false;
	for (uint8_t i = 0; i < a.nmsg; i++) {
		if (strcmp(a.msgs[i].from, from) == 0 && p->msgno[0] != '\0' && strcmp(a.msgs[i].msgno, p->msgno) == 0 &&
		    a.now - a.msgs[i].ms < MSG_DUP_MS * 20u) {
			dup = true;	// a retry: acked again, not stored again
		}
	}
	if (!dup) {
		if (a.nmsg < APRS_MESSAGES) {
			a.nmsg++;
		}
		memmove(&a.msgs[1], &a.msgs[0], (size_t)(a.nmsg - 1u) * sizeof(aprs_msg_t));
		aprs_msg_t *m = &a.msgs[0];
		memset(m, 0, sizeof(*m));
		memcpy(m->from, from, sizeof(from));
		size_t k = p->text_len < sizeof(m->text) - 1u ? p->text_len : sizeof(m->text) - 1u;
		memcpy(m->text, p->text, k);
		memcpy(m->msgno, p->msgno, sizeof(m->msgno));
		m->ms = a.now;
		changed();
	}
	if (p->msgno[0] != '\0' && a.block == APRS_OK) {
		size_t n = aprs_build_ack(from, p->msgno, false, a.info, sizeof(a.info));
		if (n != 0) {
			queue_frame(K_ACK, aprs_path_text(a.cfg.path), a.info, n);
		}
	}
}

void aprs_rx_frame(const uint8_t *frame, size_t n, uint32_t now_ms)
{
	ax25_frame_t f;
	aprs_parsed_t p;
	bool ok = ax25_decode(frame, n, &f) && f.control == AX25_CONTROL_UI && f.pid == AX25_PID_NONE;
	LOCK();
	a.now = now_ms;
	if (!ok) {
		a.v.rx_bad++;
		UNLOCK();
		return;
	}
	a.v.rx_frames++;
	host_emit(KISS_CMD_DATA, frame, n);
	if (a.me_ok && ax25_addr_equal(&f.src, &a.me)) {
		UNLOCK();
		return;		// our own packet, digipeated back
	}
	aprs_parse(&f.dst, f.info, f.info_len, &p);
	station_update(&f, &p);
	if (p.type == APRS_T_MESSAGE && a.me_ok) {
		message_in(&f, &p);
	}
	if (a.cfg.igate && a.cfg.kiss && a.me_ok && CONFIG_APRS_IGATE &&
	    aprs_igate_rf_to_is(&a.dedup_gate, &f, &a.me, now_ms, a.gate, sizeof(a.gate)) == APRS_GATE_OK) {
		host_is_line(a.gate, strlen(a.gate));
		a.v.igated++;
	}
	if (a.cfg.digi != APRS_DIGI_OFF && a.me_ok && a.block == APRS_OK) {
		ax25_frame_t out;
		uint8_t buf[AX25_FRAME_MAX];
		if (aprs_digi(&a.dedup_digi, &f, &out, &a.me, a.cfg.digi, now_ms)) {
			size_t k = ax25_encode(&out, buf, sizeof(buf));
			if (k != 0 && queue_raw(K_DIGI, buf, k)) {
				a.v.digi_sent++;
			}
		}
	}
	changed();
	UNLOCK();
}

static void demod_frame(const uint8_t *frame, size_t n, void *ctx)
{
	(void)ctx;
	aprs_rx_frame(frame, n, a.now);
}

void aprs_rx_audio(const int16_t *s, size_t n, uint32_t now_ms)
{
	a.now = now_ms;
	afsk_demod(&a.demod, s, n);
}

// The receive path: on while receiving FM with the setting on (radio task, outside the lock)
static void rx_service(const aprs_radio_t *r, uint32_t now_ms)
{
	const hal_afsk_t *h = board.afsk;
	bool want = a.cfg.rx_on && r->fm_rx && h != NULL && h->rx_start != NULL && h->rx_read != NULL;
	if (want && !a.rx_running) {
		a.rx_running = afsk_demod_init(&a.demod, h->rx_rate_hz, demod_frame, NULL) && h->rx_start();
	} else if (!want && a.rx_running) {
		if (h != NULL && h->rx_stop != NULL) {
			h->rx_stop();
		}
		a.rx_running = false;
	}
	if (a.rx_running) {
		uint16_t k;
		while ((k = h->rx_read(a.rxbuf, (uint16_t)(sizeof(a.rxbuf) / sizeof(a.rxbuf[0])))) != 0) {
			aprs_rx_audio(a.rxbuf, k, now_ms);
		}
	}
}
#endif

// --- The radio task ---

void aprs_init(uint32_t now_ms)
{
	memset(&a, 0, sizeof(a));
	a.now = now_ms;
	a.host.utc_hms = a.gps.utc_hms = 0xFFFFFFFFu;
	a.host.alt_m = a.gps.alt_m = APRS_ALT_NONE;
	a.host.speed_kmh = a.gps.speed_kmh = 0xFFFFu;
	a.host.course = a.gps.course = 0xFFFFu;
	a.host_tokens = HOST_BURST;
	a.host_refill_ms = now_ms;
	a.msg_seq = (uint16_t)(now_ms % 900u);
	reload_cfg();
	LOCK();
	aprs_beacon_init(&a.bc, &a.bcfg, now_ms);
	a.block = rf_block();
	UNLOCK();
}

// The middle of the ambiguity cell (grid in 1e-7 degree, within the whole degree) the position is in
static int32_t blur(int32_t v, int32_t grid)
{
	int32_t m = v < 0 ? -v : v;
	int32_t f = m % 10000000 / grid * grid + grid / 2;
	m = m / 10000000 * 10000000 + (f < 10000000 ? f : 9999999);
	return v < 0 ? -m : m;
}

bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	// Position ambiguity (APRS101 ch. 8): the same blur for DMR. Grid in 1e-7 degree and the error it means
	static const int32_t grid[] = { 1, 16667, 166667, 1666667, 10000000 };
	static const uint32_t blur_m[] = { 0, 185, 1852, 18520, 111120 };
	uint8_t src;
	LOCK();
	const fix_t *f = position(&src);
	bool ok = f != NULL;
	uint8_t amb = a.cfg.ambiguity <= 4 ? a.cfg.ambiguity : 0;
	if (ok) {
		*lat = f->lat;
		*lon = f->lon;
	}
	*dmr_gps = a.cfg.dmr_gps;
	UNLOCK();
	if (!ok) {
		return false;
	}
	*err_m = src == APRS_SRC_FIXED ? 200u : 20u;	// GPS / host fixes: tens of meters; a typed position: less sure
	if (amb != 0) {
		*lat = blur(*lat, grid[amb]);
		*lon = blur(*lon, grid[amb]);
		*err_m = blur_m[amb];
	}
	return true;
}

void aprs_reload(void)
{
	LOCK();
	a.reload = true;
	UNLOCK();
}

void aprs_ptt_end(uint32_t now_ms)
{
	LOCK();
	aprs_beacon_ptt_end(&a.bc, &a.bcfg, now_ms);
	UNLOCK();
}

void aprs_tick(const aprs_radio_t *r, uint32_t now_ms)
{
	LOCK();
	bool reload = a.reload;
	a.reload = false;
	UNLOCK();
	if (reload || codeplug_serial() != a.cp_serial || now_ms - a.ident_ms >= IDENT_CHECK_MS * 12u) {
		a.ident_ms = now_ms;
		if (reload) {
			reload_cfg();
		} else {
			load_identity();
		}
	}

	LOCK();
	a.now = now_ms;
	while (a.host_tokens < HOST_BURST && now_ms - a.host_refill_ms >= HOST_REFILL_MS) {
		a.host_tokens++;
		a.host_refill_ms += HOST_REFILL_MS;
	}
	if (a.host_tokens >= HOST_BURST) {
		a.host_refill_ms = now_ms;
	}
	uint8_t block = rf_block();
	if (block != a.block) {
		a.block = block;
		changed();
	}
	uint8_t src;
	const fix_t *f = position(&src);
	aprs_beacon_pos_t bp;
	memset(&bp, 0, sizeof(bp));
	if (f != NULL) {
		bp.valid = true;
		bp.lat = f->lat;
		bp.lon = f->lon;
		bp.speed_kmh = f->speed_kmh;
		bp.course = f->course;
	}
	uint8_t why = aprs_beacon_due(&a.bc, &a.bcfg, &bp, now_ms);
	if (why != APRS_WHY_NONE && f != NULL) {
		if (beacon(why, f)) {
			aprs_beacon_sent(&a.bc, why, &bp, now_ms);
		} else if (why == APRS_WHY_MANUAL) {
			a.bc.manual = false;	// nowhere to send it (a.v.block says why)
		}
	} else if (why == APRS_WHY_MANUAL) {
		a.bc.manual = false;
	}
	message_service();
	// The view
	a.v.freq_hz = aprs_cfg_freq(&a.cfg);
	a.v.block = a.block;
	a.v.pos_src = src;
	a.v.pos_valid = f != NULL;
	if (f != NULL) {
		if (a.v.lat != f->lat || a.v.lon != f->lon) {
			changed();
		}
		a.v.lat = f->lat;
		a.v.lon = f->lon;
		a.v.speed_kmh = f->speed_kmh;
		a.v.course = f->course;
		a.v.pos_age_s = (now_ms - f->ms) / 1000u;
	}
	a.v.next_s = aprs_beacon_next_s(&a.bc, &a.bcfg, &bp, now_ms);
	a.v.msg_pending = a.out.active;
	a.v.msg_tries = a.out.tries;
	a.v.msg_acked = a.out.acked;
	bool dmr_due = a.dmr_due;
	a.dmr_due = false;
	aprs_dmr_pos_t dp;
	memset(&dp, 0, sizeof(dp));
	if (dmr_due && f != NULL) {
		dp.lat = f->lat;
		dp.lon = f->lon;
		dp.speed_kn = kmh_to_kn(f->speed_kmh);
		dp.course = f->course;
		dp.utc_hms = src == APRS_SRC_FIXED ? 0xFFFFFFFFu : f->utc_hms;
		dp.date = src == APRS_SRC_FIXED ? 0 : f->date;
	}
	bool udt = a.cfg.dmr_udt;
	uint32_t dst = a.cfg.dmr_dst;
	UNLOCK();

	if (dmr_due) {
		bool ok = r->on_dmr && aprs_dmr_send(udt, dst, r->own_dmr_id, &dp);
		LOCK();
		if (ok) {
			a.v.dmr_sent++;
		} else {
			a.v.dmr_failed++;
		}
		changed();
		UNLOCK();
	}
#if CONFIG_APRS_RX
	rx_service(r, now_ms);
#endif
}

bool aprs_rf_pending(void)
{
	LOCK();
	bool p = a.qn > 0 && a.taken == 0 && a.block == APRS_OK && (int32_t)(a.now - a.rf_next_ms) >= 0;
	UNLOCK();
	return p;
}

bool aprs_rf_take(aprs_rf_job_t *job, uint32_t now_ms)
{
	LOCK();
	a.now = now_ms;
	if (a.qn == 0 || a.block != APRS_OK || a.taken != 0) {
		UNLOCK();
		return false;
	}
	uint16_t flags = hdlc_flags_for_ms((uint16_t)(a.cfg.txdelay * 10u), AFSK_BAUD, MIN_FLAGS);
	hdlc_enc_t e;
	uint8_t n = 0;
	for (;;) {
		hdlc_enc_init(&e, a.tones, sizeof(a.tones));
		bool ok = hdlc_enc_flags(&e, flags);
		n = 0;
		// Frames back to back, one flag between them, as many as fit with the tail. A flag leaves the NRZI level as
		// it found it and resets the stuffing count, so taking the tail's extra flags back off is exact
		while (ok && n < a.qn) {
			hdlc_enc_t keep = e;
			if (!hdlc_enc_frame(&e, a.q[n].data, a.q[n].len) || !hdlc_enc_flags(&e, TAIL_FLAGS)) {
				e = keep;
				break;
			}
			e.nbits -= 8u * (TAIL_FLAGS - 1u);
			n++;
		}
		if (n > 0 || flags <= MIN_FLAGS) {
			break;
		}
		flags = MIN_FLAGS;	// a long preamble and a long frame: shorten the preamble
	}
	if (n == 0) {
		queue_drop(1);		// can't fit at all
		a.v.rf_failed++;
		changed();
		UNLOCK();
		return false;
	}
	hdlc_enc_flags(&e, TAIL_FLAGS - 1u);	// room checked above
	a.taken = n;
	job->hz = aprs_cfg_freq(&a.cfg);
	job->narrow = a.cfg.narrow;
	job->level = a.cfg.level;
	job->tones = a.tones;
	job->nbits = (uint16_t)e.nbits;
	a.v.rf_active = true;
	changed();
	UNLOCK();
	return true;
}

void aprs_rf_done(bool sent, bool busy, uint32_t now_ms)
{
	LOCK();
	a.now = now_ms;
	if (sent) {
		queue_drop(a.taken);
		a.v.rf_sent++;
	} else {
		if (busy) {
			a.v.rf_busy++;
		} else {
			a.v.rf_failed++;
		}
		uint8_t keep = 0;
		for (uint8_t i = 0; i < a.taken && i < a.qn; i++) {
			if (++a.q[i].tries < RF_TRIES) {
				a.q[keep++] = a.q[i];
			}
		}
		memmove(&a.q[keep], &a.q[a.taken], (size_t)(a.qn - a.taken) * sizeof(qframe_t));
		a.qn = (uint8_t)(a.qn - a.taken + keep);
		a.rf_next_ms = now_ms + RF_RETRY_MS;
	}
	a.taken = 0;
	a.v.rf_active = false;
	changed();
	UNLOCK();
}

// --- Any task ---

void aprs_set_lock(void (*lock)(void), void (*unlock)(void))
{
	lock_fn = lock != NULL ? lock : nolock;
	unlock_fn = unlock != NULL ? unlock : nolock;
}

void aprs_get_view(aprs_view_t *v)
{
	LOCK();
	*v = a.v;
	v->block = a.block;
	v->call[0] = '\0';
	if (a.me_ok) {
		ax25_addr_format(&a.me, false, v->call);
	}
	v->stations = 0;
	v->messages = 0;
	v->unread = 0;
#if CONFIG_APRS_RX
	v->stations = a.nst;
	v->messages = a.nmsg;
	for (uint8_t i = 0; i < a.nmsg; i++) {
		v->unread = (uint8_t)(v->unread + (a.msgs[i].read ? 0u : 1u));
	}
#endif
	UNLOCK();
}

void aprs_get_cfg(aprs_cfg_t *c)
{
	LOCK();
	*c = a.cfg;
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
	if (v.ambiguity > 4) {
		v.ambiguity = 4;
	}
	settings_t *s = settings_begin();
	cfg_to_settings(&v, s);
	settings_end(now_ms);
	LOCK();
	bool was_on = a.cfg.tx_on;
	a.cfg = v;
	apply_cfg();
	if (v.tx_on && !was_on) {
		aprs_beacon_init(&a.bc, &a.bcfg, a.now);	// turned on: the interval counts from now (boot beacon if set)
	}
	changed();
	UNLOCK();
	load_identity();
}

bool aprs_send_now(void)
{
	LOCK();
	bool ok = a.block == APRS_OK || a.cfg.via != APRS_VIA_RF || (a.cfg.to_is && a.cfg.kiss);
	if (ok) {
		aprs_beacon_manual(&a.bc, a.now);
	}
	UNLOCK();
	return ok;
}

bool aprs_send_status(const char *text)
{
	LOCK();
	size_t n = aprs_build_status(text, a.info, sizeof(a.info));
	bool ok = n != 0 && a.block == APRS_OK && queue_frame(K_STATUS, aprs_path_text(a.cfg.path), a.info, n);
	if (n != 0) {
		own_to_is(a.info, n);
	}
	UNLOCK();
	return ok;
}

bool aprs_send_message(const char *to, const char *text)
{
	ax25_addr_t x;
	size_t tl = strlen(to), xl = strlen(text);
	if (!ax25_addr_parse(to, tl, &x) || xl == 0 || xl > APRS_MSG_TEXT_MAX) {
		return false;
	}
	LOCK();
	if ((a.out.active && !a.out.acked && a.out.tries < APRS_MSG_TRIES) || !a.me_ok) {
		UNLOCK();
		return false;
	}
	memset(&a.out, 0, sizeof(a.out));
	a.out.active = true;
	ax25_addr_format(&x, false, a.out.to);
	memcpy(a.out.text, text, xl + 1u);
	a.msg_seq = (uint16_t)(a.msg_seq % 999u + 1u);
	uint16_t s = a.msg_seq;
	char d[4];
	uint8_t k = 0;
	do {
		d[k++] = (char)('0' + s % 10u);
		s = (uint16_t)(s / 10u);
	} while (s != 0);
	for (uint8_t i = 0; i < k; i++) {
		a.out.msgno[i] = d[k - 1u - i];
	}
	a.out.msgno[k] = '\0';
	a.out.next_ms = a.now;
	changed();
	UNLOCK();
	return true;
}

void aprs_set_test_call(const char *call)
{
	LOCK();
	size_t n = strnlen(call, sizeof(a.test_call) - 1u);
	memcpy(a.test_call, call, n);
	a.test_call[n] = '\0';
	UNLOCK();
	load_identity();
}

#if CONFIG_APRS_RX
uint8_t aprs_station_count(void)
{
	LOCK();
	uint8_t n = a.nst;
	UNLOCK();
	return n;
}

bool aprs_station(uint8_t i, aprs_station_t *out)
{
	LOCK();
	bool ok = i < a.nst;
	if (ok) {
		*out = a.st[i];
	}
	UNLOCK();
	return ok;
}

uint8_t aprs_msg_count(void)
{
	LOCK();
	uint8_t n = a.nmsg;
	UNLOCK();
	return n;
}

bool aprs_msg(uint8_t i, aprs_msg_t *out)
{
	LOCK();
	bool ok = i < a.nmsg;
	if (ok) {
		*out = a.msgs[i];
		if (!a.msgs[i].read) {
			a.msgs[i].read = true;
			changed();
		}
	}
	UNLOCK();
	return ok;
}
#endif

// --- Host link and GPS ---

bool aprs_host_frame(const uint8_t *frame, size_t n, uint32_t now_ms)
{
	ax25_frame_t f;
	bool ok = ax25_decode(frame, n, &f) && f.control == AX25_CONTROL_UI;
	LOCK();
	a.v.host_in++;
	// Station identification: the source must be our callsign (any SSID); the APRS rules (amateur frequency,
	// callsign) apply as to our own packets; a burst of HOST_BURST, then one per HOST_REFILL_MS
	ok = ok && a.cfg.kiss && a.me_ok && strcmp(f.src.call, a.me.call) == 0 && a.host_tokens > 0 &&
	     a.block == APRS_OK && queue_raw(K_HOST, frame, n);
	if (ok) {
		a.host_tokens--;
	} else {
		a.v.host_dropped++;
	}
	(void)now_ms;
	UNLOCK();
	return ok;
}

void aprs_host_is_line(const char *line, size_t n, uint32_t now_ms)
{
#if CONFIG_APRS_IGATE
	LOCK();
	a.now = now_ms;
	if (a.cfg.igate_tx && a.cfg.kiss && a.me_ok && a.block == APRS_OK) {
		aprs_igate_env_t env = { heard_direct, heard_rf, NULL };
		size_t len;
		if (aprs_igate_is_to_rf(&a.dedup_is, line, n, &a.me, &env, now_ms, a.info, sizeof(a.info), &len) &&
		    queue_frame(K_GATE, NULL, a.info, len)) {
			a.v.is_to_rf++;
			changed();
		}
	}
	UNLOCK();
#else
	(void)line;
	(void)n;
	(void)now_ms;
#endif
}

void aprs_nmea(const char *s, size_t n, bool gps, uint32_t now_ms)
{
	nmea_fix_t x;
	if (!nmea_parse(s, n, &x)) {
		return;
	}
	LOCK();
	fix_t *f = gps ? &a.gps : &a.host;
	if (!x.fix) {
		f->valid = false;
	} else {
		f->valid = true;
		f->lat = x.lat;
		f->lon = x.lon;
		f->ms = now_ms;
		f->utc_hms = x.utc_hms;
		if (x.kind == NMEA_RMC) {
			f->speed_kmh = x.speed_kn10 == 0xFFFFu ? 0xFFFFu : (uint16_t)(((uint32_t)x.speed_kn10 * 1852u + 5000u) / 10000u);
			f->course = x.course10 == 0xFFFFu ? 0xFFFFu : (uint16_t)((x.course10 + 5u) / 10u % 360u);
			f->date = x.date;
			if (f->speed_kmh == 0) {
				f->course = 0xFFFFu;	// standing still: no meaningful course
			}
		} else {
			f->alt_m = x.alt_dm == INT32_MIN ? APRS_ALT_NONE : x.alt_dm / 10;
		}
	}
	UNLOCK();
}

size_t aprs_host_output(uint8_t *out, size_t max)
{
	LOCK();
	size_t n = 0;
	while (n < max && a.r_len > 0) {
		out[n++] = a.ring[a.r_head];
		a.r_head = (uint16_t)((a.r_head + 1u) % OUT_RING);
		a.r_len--;
	}
	UNLOCK();
	return n;
}

bool aprs_host_enabled(void)
{
	return a.cfg.kiss;
}
