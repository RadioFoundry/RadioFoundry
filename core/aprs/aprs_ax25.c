// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// APRS on FM (aprs.h, aprs_int.h): everything AX.25. Who we are (the callsign), the frame queue and the AFSK jobs the
// radio core sends, our packets (position, status, messages with acks), the KISS TNC and APRS-IS lines on the host
// port, and receive (the demodulator, station list, messages to us, digipeater, iGate). Built with CONFIG_APRS_AFSK;
// the engine (aprs.c) calls in through the aprs_ax25_* functions.
#include "config.h"
#if CONFIG_APRS_AFSK
#include <string.h>
#include "aprs/aprs_int.h"
#include "aprs/hdlc.h"
#include "aprs/kiss.h"
#include "board.h"
#if CONFIG_APRS_RX
#include "usbaudio/audio_mux.h"
#endif
#include "settings/settings.h"

#define TAIL_FLAGS	3
#define MIN_FLAGS	8
#define RF_TRIES	3		// a frame given up after this many failed transmissions
#define RF_RETRY_MS	15000u		// after a failed transmission
// A heard station's position older than this is dropped: other packets keep the entry, and its age would wrap at
// 2^32 ms (49.7 days)
#define STATION_POS_MS	(24u * 3600000u)
#define HOST_BURST	3		// KISS frames from the host: a burst of 3, then one per HOST_REFILL_MS
#define HOST_REFILL_MS	10000u
#define MSG_RETRY_MS	30000u		// message retries at 30, 60, 120 s
#define MSG_DUP_MS	30000u
#define IDENT_CHECK_MS	5000u

enum { K_BEACON, K_STATUS, K_MSG, K_ACK, K_HOST, K_GATE, K_DIGI };

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
	       (hz >= 420000000u && hz <= 450000000u) || (hz >= 902000000u && hz <= 928000000u);
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

// The codeplug record's two path entries as an aprs_path_t, APRS_PATH_COUNT for a path we don't offer
static uint8_t path_from_codeplug(const codeplug_aprs_t *cp)
{
	char t[sizeof(cp->path[0]) * 2u + 1u];
	strcpy(t, cp->path[0]);
	if (cp->path[1][0] != '\0') {
		if (t[0] == '\0') {
			return APRS_PATH_COUNT;
		}
		strcat(t, ",");
		strcat(t, cp->path[1]);
	}
	for (uint8_t i = 0; i < APRS_PATH_COUNT; i++) {
		if (strcmp(t, path_texts[i]) == 0) {
			return i;
		}
	}
	return APRS_PATH_COUNT;
}

// A path in the codeplug we haven't taken yet (a CPS wrote it) replaces ours: the path lives in both places and the
// last one changed wins (aprs.md 11). Outside the lock
static void take_codeplug_path(const settings_t *st, const codeplug_aprs_t *cp)
{
	uint8_t path = path_from_codeplug(cp);
	if (path >= APRS_PATH_COUNT || st->aprs_cp_path == path + 1u) {
		return;
	}
	settings_t *s = settings_begin();
	if (s->aprs_stored != 1) {
		aprs_cfg_store_defaults(s);	// so the path can be stored
	}
	s->aprs_path = path;
	s->aprs_cp_path = (uint8_t)(path + 1u);
	settings_end(aprs_s.now);
	LOCK();
	aprs_s.cfg.path = path;
	aprs_changed();
	UNLOCK();
}

// One write of the path into record 1, taken as seen when it lands
static bool write_codeplug_path(uint8_t path)
{
	char p1[16], p2[16];
	const char *t = path_texts[path];
	const char *comma = strchr(t, ',');
	size_t n = comma != NULL ? (size_t)(comma - t) : strlen(t);
	memcpy(p1, t, n);
	p1[n] = '\0';
	strcpy(p2, comma != NULL ? comma + 1 : "");
	if (!codeplug_set_aprs_path(1, p1, p2)) {
		return false;
	}
	settings_t *s = settings_begin();
	s->aprs_cp_path = (uint8_t)(path + 1u);
	settings_end(aprs_s.now);
	return true;
}

bool aprs_path_to_codeplug(void)
{
	// A change that came while the job waited or ran couldn't post a job of its own: write again until the
	// codeplug holds the path in use (a few times at most)
	for (uint8_t tries = 0; tries < 3u; tries++) {
		LOCK();
		uint8_t path = aprs_s.cfg.path < APRS_PATH_COUNT ? aprs_s.cfg.path : APRS_PATH_WIDE1_WIDE2;
		UNLOCK();
		if (!write_codeplug_path(path)) {
			return false;
		}
		LOCK();
		bool same = aprs_s.cfg.path == path;
		UNLOCK();
		if (same) {
			return true;
		}
	}
	return true;
}

// Who we are: the debug link's test call, the settings' radio name, the codeplug's callsign; its first word, upper
// case, without an SSID (ours comes from the configuration). Reads storage: outside the lock
void aprs_ax25_identity(void)
{
	char name[CODEPLUG_NAME_LEN + 1];
	settings_t st;
	codeplug_aprs_t cp;
	settings_read(&st);
	bool have_cp = codeplug_aprs(1, &cp);
	LOCK();
	memcpy(name, aprs_s.test_call, sizeof(aprs_s.test_call));
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
	aprs_s.me_ok = !long_name && aprs_callsign_ok(call);
	memset(&aprs_s.me, 0, sizeof(aprs_s.me));
	if (aprs_s.me_ok) {
		memcpy(aprs_s.me.call, call, n + 1u);
		aprs_s.me.ssid = aprs_s.cfg.ssid;
	}
	aprs_s.comment[0] = '\0';
	if (have_cp) {
		memcpy(aprs_s.comment, cp.comment, sizeof(aprs_s.comment));
	}
	aprs_s.block = rf_block();
	aprs_changed();
	UNLOCK();
	aprs_s.cp_serial = codeplug_serial();
	aprs_s.ident_ms = aprs_s.now;
	if (have_cp) {
		take_codeplug_path(&st, &cp);
	}
}

// --- Small helpers (lock held) ---

static bool band_tx(uint32_t hz)
{
	for (uint8_t i = 0; i < board_band_count(); i++) {
		if (board.bands[i].tx && hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return true;
		}
	}
	return false;
}

static uint8_t rf_block(void)
{
	uint32_t hz = aprs_cfg_freq(&aprs_s.cfg);
	if (!aprs_s.me_ok) {
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


// Queue an AX.25 UI frame from us (lock held); path: a comma list or "" / NULL
static bool queue_frame(uint8_t kind, const char *path, const char *info, size_t n)
{
	if (aprs_s.qn >= QUEUE_LEN || !aprs_s.me_ok) {
		return false;
	}
	ax25_frame_t f;
	memset(&f, 0, sizeof(f));
	ax25_addr_parse(APRS_TOCALL, sizeof(APRS_TOCALL) - 1u, &f.dst);
	f.dst.h = true;		// command (AX.25 2.2 6.1.2: C bit set in the destination, clear in the source)
	f.src = aprs_s.me;
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
	qframe_t *q = &aprs_s.q[aprs_s.qn];
	q->len = (uint16_t)ax25_encode(&f, q->data, sizeof(q->data));
	if (q->len == 0) {
		return false;
	}
	q->kind = kind;
	q->tries = 0;
	aprs_s.qn++;
	aprs_changed();
	return true;
}

// Raw frame (digipeated, from the host) into the queue (lock held)
static bool queue_raw(uint8_t kind, const uint8_t *frame, size_t n)
{
	if (aprs_s.qn >= QUEUE_LEN || n > AX25_FRAME_MAX) {
		return false;
	}
	qframe_t *q = &aprs_s.q[aprs_s.qn++];
	memcpy(q->data, frame, n);
	q->len = (uint16_t)n;
	q->kind = kind;
	q->tries = 0;
	aprs_changed();
	return true;
}

static void queue_drop(uint8_t count)
{
	if (count > aprs_s.qn) {
		count = aprs_s.qn;
	}
	memmove(&aprs_s.q[0], &aprs_s.q[count], (size_t)(aprs_s.qn - count) * sizeof(qframe_t));
	aprs_s.qn = (uint8_t)(aprs_s.qn - count);
}

// --- Host output (lock held) ---

static bool ring_put(uint8_t b)
{
	if (aprs_s.r_len >= OUT_RING) {
		return false;
	}
	aprs_s.ring[(aprs_s.r_head + aprs_s.r_len) % OUT_RING] = b;
	aprs_s.r_len++;
	return true;
}

// One KISS frame into the ring, whole or not at all
static void host_emit(uint8_t type, const uint8_t *data, size_t n)
{
	if (!aprs_s.cfg.kiss) {
		return;
	}
	size_t need = 3u;
	for (size_t i = 0; i < n; i++) {
		need += (data[i] == KISS_FEND || data[i] == KISS_FESC) ? 2u : 1u;
	}
	if (need + (type == KISS_FEND || type == KISS_FESC ? 1u : 0u) > (size_t)(OUT_RING - aprs_s.r_len)) {
		aprs_s.v.host_dropped++;
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
	aprs_s.v.host_out++;
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

// Our own packet straight to APRS-IS: "CALL>APZRDF,TCPIP*:info"
static void own_to_is(const char *info, size_t n)
{
	if (!aprs_s.cfg.to_is || !aprs_s.cfg.kiss || !aprs_s.me_ok) {
		return;
	}
	char line[AX25_CALL_LEN + 4 + 20 + AX25_INFO_MAX];
	size_t k = ax25_addr_format(&aprs_s.me, false, line);
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

static size_t build_beacon(const position_fix_t *f)
{
	aprs_report_t r;
	memset(&r, 0, sizeof(r));
	r.pos.lat = f->lat;
	r.pos.lon = f->lon;
	r.pos.course = f->course == 0xFFFFu ? APRS_COURSE_NONE : (f->course == 0 ? 360u : f->course);
	r.pos.speed_kn = aprs_kmh_to_kn(f->speed_kmh);
	r.pos.alt_m = f->alt_m;
	r.pos.sym_table = aprs_s.cfg.sym_table;
	r.pos.sym_code = aprs_s.cfg.sym_code;
	r.compressed = aprs_s.cfg.compressed;
	r.messaging = aprs_s.cfg.messaging;
	r.ambiguity = aprs_s.cfg.compressed ? 0 : position_ambiguity();
	r.comment = aprs_s.comment;
	return aprs_build_position(&r, aprs_s.info, sizeof(aprs_s.info));
}

static void note_info(size_t n)
{
	size_t k = n < sizeof(aprs_s.v.last_info) - 1u ? n : sizeof(aprs_s.v.last_info) - 1u;
	memcpy(aprs_s.v.last_info, aprs_s.info, k);
	aprs_s.v.last_info[k] = '\0';
}

bool aprs_ax25_beacon(const position_fix_t *f, bool rf)
{
	bool any = false;
	size_t n = build_beacon(f);
	if (n == 0) {
		return false;
	}
	note_info(n);
	if (rf && aprs_s.block == APRS_OK && queue_frame(K_BEACON, aprs_path_text(aprs_s.cfg.path), aprs_s.info, n)) {
		any = true;
	}
	if (aprs_s.cfg.to_is && aprs_s.cfg.kiss && aprs_s.me_ok) {
		own_to_is(aprs_s.info, n);
		any = true;
	}
	return any;
}

bool aprs_ax25_can_send(void)
{
	return aprs_s.block == APRS_OK || (aprs_s.cfg.to_is && aprs_s.cfg.kiss);
}

void aprs_ax25_view(aprs_view_t *v)
{
	v->block = aprs_s.block;
	v->call[0] = '\0';
	if (aprs_s.me_ok) {
		ax25_addr_format(&aprs_s.me, false, v->call);
	}
	v->stations = 0;
	v->messages = 0;
	v->unread = 0;
#if CONFIG_APRS_RX
	v->stations = aprs_s.nst;
	v->messages = aprs_s.nmsg;
	for (uint8_t i = 0; i < aprs_s.nmsg; i++) {
		v->unread = (uint8_t)(v->unread + (aprs_s.msgs[i].read ? 0u : 1u));
	}
#endif
}

void aprs_ax25_init(uint32_t now_ms)
{
	aprs_s.host_tokens = HOST_BURST;
	aprs_s.host_refill_ms = now_ms;
	aprs_s.msg_seq = (uint16_t)(now_ms % 900u);
}

void aprs_ax25_poll(uint32_t now_ms)
{
	if (codeplug_serial() != aprs_s.cp_serial || now_ms - aprs_s.ident_ms >= IDENT_CHECK_MS * 12u) {
		aprs_ax25_identity();
	}
}

void aprs_ax25_tick(uint32_t now_ms)
{
	while (aprs_s.host_tokens < HOST_BURST && now_ms - aprs_s.host_refill_ms >= HOST_REFILL_MS) {
		aprs_s.host_tokens++;
		aprs_s.host_refill_ms += HOST_REFILL_MS;
	}
	if (aprs_s.host_tokens >= HOST_BURST) {
		aprs_s.host_refill_ms = now_ms;
	}
#if CONFIG_APRS_RX
	bool expired = false;
	for (uint8_t i = 0; i < aprs_s.nst; i++) {
		if (aprs_s.st[i].has_pos && now_ms - aprs_s.st[i].pos_ms >= STATION_POS_MS) {
			aprs_s.st[i].has_pos = false;
			expired = true;
		}
	}
	if (expired) {
		aprs_changed();
	}
#endif
	uint8_t block = rf_block();
	if (block != aprs_s.block) {
		aprs_s.block = block;
		aprs_changed();
	}
}

void aprs_ax25_service(void)
{
	// This half's view fields first: the returns below leave them set
	aprs_s.v.freq_hz = aprs_cfg_freq(&aprs_s.cfg);
	aprs_s.v.block = aprs_s.block;
	aprs_s.v.msg_pending = aprs_s.out.active;
	aprs_s.v.msg_tries = aprs_s.out.tries;
	aprs_s.v.msg_acked = aprs_s.out.acked;
	if (!aprs_s.out.active || aprs_s.block != APRS_OK || (int32_t)(aprs_s.now - aprs_s.out.next_ms) < 0) {
		return;
	}
	if (aprs_s.out.acked || aprs_s.out.tries >= APRS_MSG_TRIES) {
		aprs_s.out.active = false;
		aprs_changed();
		return;
	}
	size_t n = aprs_build_message(aprs_s.out.to, aprs_s.out.text, aprs_s.out.msgno, aprs_s.info, sizeof(aprs_s.info));
	if (n != 0 && queue_frame(K_MSG, aprs_path_text(aprs_s.cfg.path), aprs_s.info, n)) {
		own_to_is(aprs_s.info, n);
		aprs_s.out.tries++;
		aprs_s.out.next_ms = aprs_s.now + (MSG_RETRY_MS << (aprs_s.out.tries - 1u));
		if (!aprs_s.out.want_ack)
			aprs_s.out.active = false;
		aprs_changed();
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
	for (uint8_t i = 0; i < aprs_s.nst; i++) {
		if (strcmp(aprs_s.st[i].call, call) == 0) {
			return &aprs_s.st[i];
		}
	}
	return NULL;
}

#if CONFIG_APRS_IGATE
// The iGate rules' view of the station list
static bool heard(const char *call, uint8_t ssid, uint32_t secs, bool direct_only)
{
	char key[AX25_CALL_LEN + 4];
	ax25_addr_t x;
	memset(&x, 0, sizeof(x));
	memcpy(x.call, call, strnlen(call, AX25_CALL_LEN));
	x.ssid = ssid;
	addr_text(&x, key);
	const aprs_station_t *s = station_find(key);
	return s != NULL && (!direct_only || s->direct) && (aprs_s.now - s->heard_ms) / 1000u < secs;
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
#endif

// The station's entry, moved to the top (a new one at the top, the oldest dropped when full)
static aprs_station_t *station_touch(const char *call)
{
	aprs_station_t *s = station_find(call);
	if (s == NULL) {
		if (aprs_s.nst < APRS_STATIONS) {
			aprs_s.nst++;
		}
		memmove(&aprs_s.st[1], &aprs_s.st[0], (size_t)(aprs_s.nst - 1u) * sizeof(aprs_station_t));
		s = &aprs_s.st[0];
		memset(s, 0, sizeof(*s));
		memcpy(s->call, call, strnlen(call, sizeof(s->call) - 1u));
	} else if (s != &aprs_s.st[0]) {
		aprs_station_t t = *s;	// most recent first
		memmove(&aprs_s.st[1], &aprs_s.st[0], (size_t)(s - &aprs_s.st[0]) * sizeof(aprs_station_t));
		aprs_s.st[0] = t;
		s = &aprs_s.st[0];
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
	s->heard_ms = aprs_s.now;
	s->direct = direct;
	s->type = p->type;
	s->count++;
	if (p->has_pos && p->type != APRS_T_OBJECT && p->type != APRS_T_ITEM) {	// an object's position isn't its sender's
		s->has_pos = true;
		s->lat = p->pos.lat;
		s->lon = p->pos.lon;
		s->pos_ms = aprs_s.now;
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
	aprs_s.now = now_ms;
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
		s->pos_ms = now_ms;
		s->sym_table = '/';
		s->sym_code = '[';
		s->course = s->speed_kn = 0xFFFFu;
		memcpy(s->text, "DMR", 4);
		aprs_s.v.stations = aprs_s.nst;
		aprs_changed();
	}
	UNLOCK();
}

static void message_in(const ax25_frame_t *f, const aprs_parsed_t *p)
{
	char from[AX25_CALL_LEN + 4], me[AX25_CALL_LEN + 4];
	addr_text(&f->src, from);
	addr_text(&aprs_s.me, me);
	if (strcmp(p->addressee, me) != 0) {
		return;
	}
	if (p->ack || p->rej) {
		if (aprs_s.out.active && strcmp(aprs_s.out.to, from) == 0 && strcmp(aprs_s.out.msgno, p->msgno) == 0) {
			aprs_s.out.acked = p->ack;
			if (p->rej)
				aprs_s.out.active = false;
			else
				aprs_s.out.next_ms = aprs_s.now;
			aprs_changed();
		}
		return;
	}
	bool dup = false;
	for (uint8_t i = 0; i < aprs_s.nmsg; i++) {
		if (strcmp(aprs_s.msgs[i].from, from) == 0 && p->msgno[0] != '\0' && strcmp(aprs_s.msgs[i].msgno, p->msgno) == 0 &&
		    aprs_s.now - aprs_s.msgs[i].ms < MSG_DUP_MS * 20u) {
			dup = true;	// a retry: acked again, not stored again
		}
	}
	if (!dup) {
		if (aprs_s.nmsg < APRS_MESSAGES) {
			aprs_s.nmsg++;
		}
		memmove(&aprs_s.msgs[1], &aprs_s.msgs[0], (size_t)(aprs_s.nmsg - 1u) * sizeof(aprs_msg_t));
		aprs_msg_t *m = &aprs_s.msgs[0];
		memset(m, 0, sizeof(*m));
		memcpy(m->from, from, sizeof(from));
		size_t k = p->text_len < sizeof(m->text) - 1u ? p->text_len : sizeof(m->text) - 1u;
		memcpy(m->text, p->text, k);
		memcpy(m->msgno, p->msgno, sizeof(m->msgno));
		m->ms = aprs_s.now;
		aprs_changed();
	}
	if (p->msgno[0] != '\0' && aprs_s.block == APRS_OK) {
		size_t n = aprs_build_ack(from, p->msgno, false, aprs_s.info, sizeof(aprs_s.info));
		if (n != 0) {
			queue_frame(K_ACK, aprs_path_text(aprs_s.cfg.path), aprs_s.info, n);
		}
	}
}

// A third-party packet ("}SRC>DST,PATH:info", APRS101 chapter 17): how an iGate puts APRS-IS traffic on RF, so
// messages from Internet stations and the acks of ours to them arrive this way. The message inside is from SRC: kept,
// matched and acked as SRC's. Only messages are taken; the station list keeps the iGate, the station actually heard,
// since an Internet station must not count as heard on RF for the iGate rules. One level, no nesting
static void third_party_in(const ax25_frame_t *f)
{
	ax25_frame_t in;
	aprs_parsed_t q;
	if (f->info_len < 2 || !ax25_from_tnc2((const char *)f->info + 1, f->info_len - 1u, &in) || in.info_len == 0 ||
	    ax25_addr_equal(&in.src, &aprs_s.me)) {
		return;
	}
	aprs_parse(&in.dst, in.info, in.info_len, &q);
	if (q.type == APRS_T_MESSAGE) {
		message_in(&in, &q);
	}
}

void aprs_rx_frame(const uint8_t *frame, size_t n, uint32_t now_ms)
{
	ax25_frame_t f;
	aprs_parsed_t p;
	bool ok = ax25_decode(frame, n, &f) && f.control == AX25_CONTROL_UI && f.pid == AX25_PID_NONE;
	LOCK();
	aprs_s.now = now_ms;
	if (!ok) {
		aprs_s.v.rx_bad++;
		UNLOCK();
		return;
	}
	aprs_s.v.rx_frames++;
	host_emit(KISS_CMD_DATA, frame, n);
	if (aprs_s.me_ok && ax25_addr_equal(&f.src, &aprs_s.me)) {
		UNLOCK();
		return;		// our own packet, digipeated back
	}
	aprs_parse(&f.dst, f.info, f.info_len, &p);
	station_update(&f, &p);
	if (p.type == APRS_T_MESSAGE && aprs_s.me_ok) {
		message_in(&f, &p);
	}
	if (p.type == APRS_T_THIRD_PARTY && aprs_s.me_ok) {
		third_party_in(&f);
	}
	if (aprs_s.cfg.igate && aprs_s.cfg.kiss && aprs_s.me_ok && CONFIG_APRS_IGATE &&
	    aprs_igate_rf_to_is(&aprs_s.dedup_gate, &f, &aprs_s.me, now_ms, aprs_s.gate, sizeof(aprs_s.gate)) == APRS_GATE_OK) {
		host_is_line(aprs_s.gate, strlen(aprs_s.gate));
		aprs_s.v.igated++;
	}
	if (aprs_s.cfg.digi != APRS_DIGI_OFF && aprs_s.me_ok && aprs_s.block == APRS_OK) {
		ax25_frame_t out;
		uint8_t buf[AX25_FRAME_MAX];
		if (aprs_digi(&aprs_s.dedup_digi, &f, &out, &aprs_s.me, aprs_s.cfg.digi, now_ms)) {
			size_t k = ax25_encode(&out, buf, sizeof(buf));
			if (k != 0 && queue_raw(K_DIGI, buf, k)) {
				aprs_s.v.digi_sent++;
			}
		}
	}
	aprs_changed();
	UNLOCK();
}

static void demod_frame(const uint8_t *frame, size_t n, void *ctx)
{
	(void)ctx;
	aprs_rx_frame(frame, n, aprs_s.now);
}

void aprs_rx_audio(const int16_t *s, size_t n, uint32_t now_ms)
{
	aprs_s.now = now_ms;
	for (size_t i = 0; i < n; i++) {
		uint16_t m = (uint16_t)(s[i] < 0 ? -(int32_t)s[i] : s[i]);
		if (m > aprs_s.rx_peak) {
			aprs_s.rx_peak = m;
		}
	}
	afsk_demod(&aprs_s.demod, s, n);
}

// The receive audio: the board's own AFSK receive path, else FM receive audio on the shared PCM stream (boards whose
// audio tap has AUDIO_TAP_FM_RX: 8 kHz, audio_mux.h)
static bool rx_by_board(void)
{
	const hal_afsk_t *h = board.afsk;
	return h != NULL && h->rx_start != NULL && h->rx_read != NULL;
}

static bool rx_by_stream(void)
{
	return board.audio != NULL && board.audio_tap != NULL && (board.audio_tap->caps & AUDIO_TAP_FM_RX) != 0;
}

// A start the board refused (no memory, its audio path busy) is tried again this much later, not on every tick: each
// try routes the receiver and back
#define RX_RETRY_MS	1000u

// The receive path: on while receiving FM with the setting on (radio task, outside the lock)
void aprs_ax25_rx_service(const aprs_radio_t *r, uint32_t now_ms)
{
	const hal_afsk_t *h = board.afsk;
	bool own = rx_by_board();
	bool want = aprs_s.cfg.rx_on && r->fm_rx && r->rx_hz != 0 && r->rx_hz == aprs_cfg_freq(&aprs_s.cfg) &&
		    (own || rx_by_stream());
	// The wait only after a refusal: an old rx_retry_ms compared 24.8 days later would read as in the future
	if (want && !aprs_s.rx_running && (!aprs_s.rx_retry_wait || (int32_t)(now_ms - aprs_s.rx_retry_ms) >= 0)) {
		aprs_s.rx_peak = 0;
		aprs_s.rx_running = afsk_demod_init(&aprs_s.demod, own ? h->rx_rate_hz : 8000u, demod_frame, NULL);
		if (aprs_s.rx_running && own) {
			aprs_s.rx_running = h->rx_start();
			aprs_s.rx_retry_wait = !aprs_s.rx_running;
			if (!aprs_s.rx_running) {
				aprs_s.rx_start_fails++;
				aprs_s.rx_retry_ms = now_ms + RX_RETRY_MS;
			}
		} else if (aprs_s.rx_running) {
			audio_mux_aprs_rx(true);
		}
	} else if (!want && aprs_s.rx_running) {
		if (own) {
			if (h->rx_stop != NULL) {
				h->rx_stop();
			}
		} else {
			audio_mux_aprs_rx(false);
		}
		aprs_s.rx_running = false;
	}
	if (!want) {
		aprs_s.rx_retry_wait = false;	// a refusal's wait is for the try it followed, not a later one
	}
	if (aprs_s.rx_running) {
		uint16_t n = (uint16_t)(sizeof(aprs_s.rxbuf) / sizeof(aprs_s.rxbuf[0]));
		uint16_t k;
		while ((k = own ? h->rx_read(aprs_s.rxbuf, n) : (uint16_t)audio_mux_aprs_read(aprs_s.rxbuf, n)) != 0) {
			aprs_rx_audio(aprs_s.rxbuf, k, now_ms);
		}
	}
}
#endif

bool aprs_can_receive(void)
{
#if CONFIG_APRS_RX
	return rx_by_board() || rx_by_stream();
#else
	return false;
#endif
}

#if CONFIG_APRS_RX
void aprs_rx_stats(aprs_rx_stats_t *out)
{
	out->peak = aprs_s.rx_peak;
	out->bits = aprs_s.demod.bits;
	out->frames = aprs_s.demod.frames;
	out->fcs_errors = afsk_demod_fcs_errors(&aprs_s.demod);
	out->aborts = afsk_demod_aborts(&aprs_s.demod);
	out->start_fails = aprs_s.rx_start_fails;
}
#endif

bool aprs_rx_listening(void)
{
#if CONFIG_APRS_RX
	return aprs_s.rx_running;
#else
	return false;
#endif
}

bool aprs_rf_pending(void)
{
	LOCK();
	bool p = aprs_s.qn > 0 && aprs_s.taken == 0 && aprs_s.block == APRS_OK && (int32_t)(aprs_s.now - aprs_s.rf_next_ms) >= 0;
	UNLOCK();
	return p;
}

bool aprs_rf_take(aprs_rf_job_t *job, uint32_t now_ms)
{
	LOCK();
	aprs_s.now = now_ms;
	if (aprs_s.qn == 0 || aprs_s.block != APRS_OK || aprs_s.taken != 0) {
		UNLOCK();
		return false;
	}
	uint16_t flags = hdlc_flags_for_ms((uint16_t)(aprs_s.cfg.txdelay * 10u), AFSK_BAUD, MIN_FLAGS);
	hdlc_enc_t e;
	uint8_t n = 0;
	for (;;) {
		hdlc_enc_init(&e, aprs_s.tones, sizeof(aprs_s.tones));
		bool ok = hdlc_enc_flags(&e, flags);
		n = 0;
		// Frames back to back, one flag between them, as many as fit with the tail. A flag leaves the NRZI level as
		// it found it and resets the stuffing count, so taking the tail's extra flags back off is exact
		while (ok && n < aprs_s.qn) {
			hdlc_enc_t keep = e;
			if (!hdlc_enc_frame(&e, aprs_s.q[n].data, aprs_s.q[n].len) || !hdlc_enc_flags(&e, TAIL_FLAGS)) {
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
		aprs_s.v.rf_failed++;
		aprs_changed();
		UNLOCK();
		return false;
	}
	hdlc_enc_flags(&e, TAIL_FLAGS - 1u);	// room checked above
	aprs_s.taken = n;
	job->hz = aprs_cfg_freq(&aprs_s.cfg);
	job->narrow = aprs_s.cfg.narrow;
	job->level = aprs_s.cfg.level;
	job->tones = aprs_s.tones;
	job->nbits = (uint16_t)e.nbits;
	aprs_s.v.rf_active = true;
	aprs_changed();
	UNLOCK();
	return true;
}

void aprs_rf_done(aprs_rf_result_t r, uint32_t now_ms)
{
	LOCK();
	aprs_s.now = now_ms;
	if (r == APRS_RF_SENT) {
		queue_drop(aprs_s.taken);
		aprs_s.v.rf_sent++;
	} else {
		if (r == APRS_RF_BUSY) {
			aprs_s.v.rf_busy++;
		} else {
			aprs_s.v.rf_failed++;
			aprs_s.v.rf_fail = (uint8_t)r;
		}
		uint8_t keep = 0;
		for (uint8_t i = 0; i < aprs_s.taken && i < aprs_s.qn; i++) {
			if (++aprs_s.q[i].tries < RF_TRIES) {
				aprs_s.q[keep++] = aprs_s.q[i];
			}
		}
		memmove(&aprs_s.q[keep], &aprs_s.q[aprs_s.taken], (size_t)(aprs_s.qn - aprs_s.taken) * sizeof(qframe_t));
		aprs_s.qn = (uint8_t)(aprs_s.qn - aprs_s.taken + keep);
		aprs_s.rf_next_ms = now_ms + RF_RETRY_MS;
	}
	aprs_s.taken = 0;
	aprs_s.v.rf_active = false;
	aprs_changed();
	UNLOCK();
}

uint8_t aprs_tx_power(uint8_t current)
{
	uint8_t p = aprs_s.cfg.power;	// one byte: no lock
	if (p == 0 || board.power_count == 0) {
		return current;
	}
	return p <= board.power_count ? (uint8_t)(p - 1u) : (uint8_t)(board.power_count - 1u);
}

bool aprs_send_status(const char *text)
{
	LOCK();
	size_t n = aprs_build_status(text, aprs_s.info, sizeof(aprs_s.info));
	bool ok = n != 0 && aprs_s.block == APRS_OK && queue_frame(K_STATUS, aprs_path_text(aprs_s.cfg.path), aprs_s.info, n);
	if (n != 0) {
		own_to_is(aprs_s.info, n);
	}
	UNLOCK();
	return ok;
}

static char *message_coord(char *p, int32_t value, bool lon)
{
	uint32_t n = (uint32_t)(value < 0 ? -(int64_t)value : value);
	uint32_t d = (n + 500u) / 1000u;
	uint32_t degrees = d / 10000u;
	uint32_t fraction = d % 10000u;
	if (lon)
		*p++ = (char)('0' + degrees / 100u % 10u);
	*p++ = (char)('0' + degrees / 10u % 10u);
	*p++ = (char)('0' + degrees % 10u);
	*p++ = '.';
	for (uint32_t div = 1000u; div != 0; div /= 10u)
		*p++ = (char)('0' + fraction / div % 10u);
	*p++ = lon ? (value < 0 ? 'W' : 'E') : (value < 0 ? 'S' : 'N');
	return p;
}

bool aprs_send_message_opts(const char *to, const char *text, uint8_t flags)
{
	if (to == NULL || text == NULL)
		return false;
	ax25_addr_t x;
	size_t tl = strlen(to), xl = strlen(text);
	if (!ax25_addr_parse(to, tl, &x) || xl == 0 || xl > APRS_MSG_TEXT_MAX ||
	    (flags & ~(APRS_MSG_ACK | APRS_MSG_POSITION)) != 0) {
		return false;
	}
	for (size_t i = 0; i < xl; i++) {
		if ((uint8_t)text[i] < 0x20u || (uint8_t)text[i] > 0x7eu)
			return false;
	}
	char composed[APRS_MSG_TEXT_MAX + 1];
	if (flags & APRS_MSG_POSITION) {
		int32_t lat, lon;
		uint32_t err_m;
		bool dmr_gps;
		if (!position_now(&lat, &lon, &err_m, &dmr_gps, aprs_s.now))
			return false;
		char coord[24];
		char *end = coord;
		*end++ = ' ';
		*end++ = '@';
		end = message_coord(end, lat, false);
		*end++ = ' ';
		end = message_coord(end, lon, true);
		size_t cn = (size_t)(end - coord);
		if (xl + cn > APRS_MSG_TEXT_MAX)
			return false;
		memcpy(composed, text, xl);
		memcpy(composed + xl, coord, cn);
		composed[xl + cn] = '\0';
		text = composed;
		xl += cn;
	}
	LOCK();
	if (rf_block() != APRS_OK) {
		UNLOCK();
		return false;
	}
	memset(&aprs_s.out, 0, sizeof(aprs_s.out));
	aprs_s.out.active = true;
	aprs_s.out.want_ack = (flags & APRS_MSG_ACK) != 0;
	ax25_addr_format(&x, false, aprs_s.out.to);
	memcpy(aprs_s.out.text, text, xl + 1u);
	if (aprs_s.out.want_ack) {
		aprs_s.msg_seq = (uint16_t)(aprs_s.msg_seq % 999u + 1u);
		uint16_t s = aprs_s.msg_seq;
		char d[4];
		uint8_t k = 0;
		do {
			d[k++] = (char)('0' + s % 10u);
			s = (uint16_t)(s / 10u);
		} while (s != 0);
		for (uint8_t i = 0; i < k; i++) {
			aprs_s.out.msgno[i] = d[k - 1u - i];
		}
		aprs_s.out.msgno[k] = '\0';
	}
	aprs_s.out.next_ms = aprs_s.now;
	aprs_changed();
	UNLOCK();
	return true;
}

bool aprs_send_message(const char *to, const char *text)
{
	return aprs_send_message_opts(to, text, APRS_MSG_ACK);
}

void aprs_set_test_call(const char *call)
{
	LOCK();
	size_t n = strnlen(call, sizeof(aprs_s.test_call) - 1u);
	memcpy(aprs_s.test_call, call, n);
	aprs_s.test_call[n] = '\0';
	UNLOCK();
	aprs_ax25_identity();
}

#if CONFIG_APRS_RX
uint8_t aprs_station_count(void)
{
	LOCK();
	uint8_t n = aprs_s.nst;
	UNLOCK();
	return n;
}

bool aprs_station(uint8_t i, aprs_station_t *out)
{
	LOCK();
	bool ok = i < aprs_s.nst;
	if (ok) {
		*out = aprs_s.st[i];
	}
	UNLOCK();
	return ok;
}

bool aprs_station_find(const char *call, aprs_station_t *out)
{
	LOCK();
	const aprs_station_t *s = station_find(call);
	bool ok = s != NULL;
	if (ok) {
		*out = *s;
	}
	UNLOCK();
	return ok;
}

uint8_t aprs_msg_count(void)
{
	LOCK();
	uint8_t n = aprs_s.nmsg;
	UNLOCK();
	return n;
}

static bool message_get(uint8_t i, aprs_msg_t *out, bool read)
{
	LOCK();
	bool ok = i < aprs_s.nmsg;
	if (ok) {
		*out = aprs_s.msgs[i];
		if (read && !aprs_s.msgs[i].read) {
			aprs_s.msgs[i].read = true;
			aprs_changed();
		}
	}
	UNLOCK();
	return ok;
}

bool aprs_msg(uint8_t i, aprs_msg_t *out) { return message_get(i, out, true); }
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out) { return message_get(i, out, false); }

bool aprs_msg_delete(uint8_t i)
{
	LOCK();
	bool ok = i < aprs_s.nmsg;
	if (ok) {
		memmove(&aprs_s.msgs[i], &aprs_s.msgs[i + 1u], (aprs_s.nmsg - i - 1u) * sizeof aprs_s.msgs[0]);
		aprs_s.nmsg--;
		aprs_changed();
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
	aprs_s.v.host_in++;
	// Station identification: the source must be our callsign (any SSID); the APRS rules (amateur frequency,
	// callsign) apply as to our own packets; a burst of HOST_BURST, then one per HOST_REFILL_MS
	ok = ok && aprs_s.cfg.kiss && aprs_s.me_ok && strcmp(f.src.call, aprs_s.me.call) == 0 && aprs_s.host_tokens > 0 &&
	     aprs_s.block == APRS_OK && queue_raw(K_HOST, frame, n);
	if (ok) {
		aprs_s.host_tokens--;
	} else {
		aprs_s.v.host_dropped++;
	}
	(void)now_ms;
	UNLOCK();
	return ok;
}

void aprs_host_is_line(const char *line, size_t n, uint32_t now_ms)
{
#if CONFIG_APRS_IGATE
	LOCK();
	aprs_s.now = now_ms;
	if (aprs_s.cfg.igate_tx && aprs_s.cfg.kiss && aprs_s.me_ok && aprs_s.block == APRS_OK) {
		aprs_igate_env_t env = { heard_direct, heard_rf, NULL };
		size_t len;
		if (aprs_igate_is_to_rf(&aprs_s.dedup_is, line, n, &aprs_s.me, &env, now_ms, aprs_s.info, sizeof(aprs_s.info), &len) &&
		    queue_frame(K_GATE, NULL, aprs_s.info, len)) {
			aprs_s.v.is_to_rf++;
			aprs_changed();
		}
	}
	UNLOCK();
#else
	(void)line;
	(void)n;
	(void)now_ms;
#endif
}

size_t aprs_host_output(uint8_t *out, size_t max)
{
	LOCK();
	size_t n = 0;
	while (n < max && aprs_s.r_len > 0) {
		out[n++] = aprs_s.ring[aprs_s.r_head];
		aprs_s.r_head = (uint16_t)((aprs_s.r_head + 1u) % OUT_RING);
		aprs_s.r_len--;
	}
	UNLOCK();
	return n;
}
#endif
