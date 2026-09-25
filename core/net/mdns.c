// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Multicast DNS responder and DNS-SD advertiser (mdns.h, docs/spec/mdns.md), written from RFC 1035, RFC 6762 and
// RFC 6763. The records it owns:
//   <host>.local                        A      the radio's address                     unique, TTL 120
//   <host>.local                        NSEC   "only an A record here"                 unique, TTL 120
// and for each service s of the table:
//   <type>.local                        PTR    <instance>.<type>.local                 shared, TTL 4500
//   <instance>.<type>.local             SRV    0 0 <port> <host>.local                 unique, TTL 120
//   <instance>.<type>.local             TXT    board=, id=, call=, fw=                 unique, TTL 4500
//   _services._dns-sd._udp.local        PTR    <type>.local                            shared, TTL 4500
//   <instance>.<type>.local             NSEC   "only SRV and TXT here"                 unique, TTL 120
// The host is "odr-<DMR ID>", the instance "<callsign> <board> (<DMR ID>)". Names are compared against the message
// in place (no decode buffers: the host link task's stack is small). Replies wait in record sets and go out from
// mdnsTick, so a busy USB endpoint only delays them.
#include <string.h>
#include "net/mdns.h"

#define TYPE_A		1
#define TYPE_PTR	12
#define TYPE_TXT	16
#define TYPE_AAAA	28
#define TYPE_SRV	33
#define TYPE_NSEC	47
#define TYPE_ANY	255
#define CLASS_IN	1
#define CLASS_ANY	255
#define CACHE_FLUSH	0x8000u		// rrclass top bit in a response: this record set replaces what caches hold
#define QU_BIT		0x8000u		// qclass top bit in a question: unicast response wanted

#define TTL_HOST	120u		// records with a host name in them (RFC 6762 10)
#define TTL_OTHER	4500u
#define TTL_LEGACY	10u		// legacy unicast answers (RFC 6762 6.7)

#define PROBE_GAP_MS	250u
#define PROBES		3
#define ANNOUNCES	2
#define ANNOUNCE_GAP_MS	1000u
#define DEFER_MS	1000u		// lost a simultaneous probe tie-break: probe again after this
#define RATE_MS		1000u		// a record set is multicast at most once a second
#define RATE_PROBE_MS	250u		// except when defending against a probe
#define IDENTITY_MS	1000u
#define SEND_BUDGET	4		// messages per tick at most

// Records: two for the host, then K_COUNT per service
enum { REC_A, REC_HNSEC, REC_SVC0 };
enum { K_PTR, K_SRV, K_TXT, K_ENUM, K_NSEC, K_COUNT };
#define REC(s, k)	(REC_SVC0 + (s) * K_COUNT + (k))
#define BIT(r)		((recset_t)1 << (r))
typedef uint64_t recset_t;
_Static_assert(REC_SVC0 + MDNS_SERVICES_MAX * K_COUNT <= 64, "record sets are 64 bits");

// Names, as label lists
enum { N_HOST, N_TYPE, N_INST, N_ENUM };
#define LABELS_MAX	6
typedef struct {
	const char *p[LABELS_MAX];
	uint8_t len[LABELS_MAX];
	uint8_t n;
} name_t;

// A message being built
#define OFFSETS_MAX	24
typedef struct {
	uint8_t *b;
	int len, max;
	bool over;
	uint16_t offs[OFFSETS_MAX];	// where names (and their suffixes) start, for compression
	uint8_t noffs;
	uint16_t count[4];		// questions, answers, authority, additional
} msg_t;

// How a record is written
enum { HOW_NORMAL, HOW_LEGACY, HOW_GOODBYE, HOW_PROBE };

static const mdns_io_t *io;
static const mdns_service_t *svc;
static uint8_t nsvc;
static mdns_stats_t stats;

static mdns_identity_t id;
static char host_label[24];
static char inst_label[MDNS_LABEL_MAX + 1];
static uint8_t rename_n;		// conflicts since the identity was set: suffix n + 1
static uint8_t state;			// mdns_state_t
static uint8_t step;			// probes / announcements sent in this state
static uint32_t next_ms;
static uint32_t identity_ms;
static bool identity_read;
static bool was_up;
static uint32_t now_ms;			// the time of the current mdnsTick / mdnsReceive
static bool host_heard;			// an mDNS message from the host since the link came up
// When each record was last multicast, in 4 ms units (wraps after 262 s; only gaps under a second matter), and
// which of them have been at all
#define RATE_UNIT_MS	4u
static uint16_t last_mcast[REC_SVC0 + MDNS_SERVICES_MAX * K_COUNT];
static recset_t mcast_once;

// What waits to be sent
static uint16_t pend_probe, pend_announce;	// groups: bit 0 the host, bit s + 1 service s
static recset_t pend_ans, pend_add;		// a multicast response
static struct {
	bool on;
	bool legacy;
	uint32_t ip;
	uint16_t port;
	uint16_t qid;
	uint8_t qname, qsvc;			// the question to echo (legacy): one of our names
	uint16_t qtype;
	recset_t ans, add;
} uni;

// ---- small helpers ----

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int utoa10(char *out, uint32_t v)
{
	char t[10];
	int n = 0, i = 0;
	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 && n < 10);
	while (n > 0) {
		out[i++] = t[--n];
	}
	out[i] = '\0';
	return i;
}

// Append s to buf (size bytes), keeping the NUL
static void cat(char *buf, size_t size, const char *s)
{
	size_t n = strlen(buf);
	while (*s != '\0' && n + 1 < size) {
		buf[n++] = *s++;
	}
	buf[n] = '\0';
}

static void stamp_mcast(recset_t sent, uint32_t now)
{
	for (int r = 0; r < 64; r++) {
		if ((sent & BIT(r)) != 0) {
			last_mcast[r] = (uint16_t)(now / RATE_UNIT_MS);
		}
	}
	mcast_once |= sent;
}

static bool mcast_recently(int r, uint32_t now, uint32_t gap)
{
	return (mcast_once & BIT(r)) != 0 && (uint16_t)(now / RATE_UNIT_MS - last_mcast[r]) < gap / RATE_UNIT_MS;
}

// ---- names ----

static void add_label(name_t *nm, const char *p, size_t len)
{
	if (nm->n < LABELS_MAX) {
		nm->p[nm->n] = p;
		nm->len[nm->n] = (uint8_t)(len > MDNS_LABEL_MAX ? MDNS_LABEL_MAX : len);
		nm->n++;
	}
}

// "_tftp._udp" -> two labels
static void add_type(name_t *nm, const char *type)
{
	const char *dot = strchr(type, '.');
	if (dot == NULL) {
		add_label(nm, type, strlen(type));
		return;
	}
	add_label(nm, type, (size_t)(dot - type));
	add_label(nm, dot + 1, strlen(dot + 1));
}

static void make_name(name_t *nm, int which, int s)
{
	nm->n = 0;
	switch (which) {
	case N_HOST:
		add_label(nm, host_label, strlen(host_label));
		break;
	case N_INST:
		add_label(nm, inst_label, strlen(inst_label));
		add_type(nm, svc[s].type);
		break;
	case N_TYPE:
		add_type(nm, svc[s].type);
		break;
	default:
		add_label(nm, "_services", 9);
		add_label(nm, "_dns-sd", 7);
		add_label(nm, "_udp", 4);
		break;
	}
	add_label(nm, "local", 5);
}

// The name at msg[off] equals nm (ASCII case insensitive, compression followed)? *end: the offset after the name in
// place, -1 if malformed
static bool name_match(const uint8_t *msg, int length, int off, const name_t *nm, int *end)
{
	int i = 0, jumps = 0, after = -1;
	bool same = true;

	for (;;) {
		if (off >= length) {
			*end = -1;
			return false;
		}
		uint8_t c = msg[off];
		if ((c & 0xC0) == 0xC0) {
			if (off + 1 >= length || ++jumps > 16) {
				*end = -1;
				return false;
			}
			if (after < 0) {
				after = off + 2;
			}
			off = ((c & 0x3F) << 8) | msg[off + 1];
			continue;
		}
		if ((c & 0xC0) != 0 || off + 1 + c > length) {
			*end = -1;
			return false;
		}
		if (c == 0) {
			*end = after >= 0 ? after : off + 1;
			return same && i == nm->n;
		}
		if (same) {
			if (i >= nm->n || nm->len[i] != c) {
				same = false;
			} else {
				for (int k = 0; k < c; k++) {
					if (lower((char)msg[off + 1 + k]) != lower(nm->p[i][k])) {
						same = false;
						break;
					}
				}
			}
			i++;
		}
		off += 1 + c;
	}
}

// Which of our names is at msg[off]: N_* and the service in *s, -1 if none; *end as name_match
static int our_name(const uint8_t *msg, int length, int off, int *s, int *end)
{
	name_t nm;
	*s = 0;
	make_name(&nm, N_HOST, 0);
	if (name_match(msg, length, off, &nm, end)) {
		return N_HOST;
	}
	if (*end < 0) {
		return -1;
	}
	make_name(&nm, N_ENUM, 0);
	if (name_match(msg, length, off, &nm, end)) {
		return N_ENUM;
	}
	for (int i = 0; i < nsvc; i++) {
		*s = i;
		make_name(&nm, N_TYPE, i);
		if (name_match(msg, length, off, &nm, end)) {
			return N_TYPE;
		}
		make_name(&nm, N_INST, i);
		if (name_match(msg, length, off, &nm, end)) {
			return N_INST;
		}
	}
	return -1;
}

// ---- writing ----

static void put(msg_t *m, const void *p, int n)
{
	if (m->len + n > m->max) {
		m->over = true;
		return;
	}
	memcpy(m->b + m->len, p, (size_t)n);
	m->len += n;
}

static void put8(msg_t *m, uint8_t v)
{
	put(m, &v, 1);
}

static void put16(msg_t *m, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
	put(m, b, 2);
}

static void put32(msg_t *m, uint32_t v)
{
	put16(m, (uint16_t)(v >> 16));
	put16(m, (uint16_t)v);
}

// Labels i.. of nm are written at m->b[off] (written by us: exact compare)?
static bool suffix_at(const msg_t *m, int off, const name_t *nm, int i)
{
	for (int jumps = 0; jumps < 16;) {
		uint8_t c = m->b[off];
		if ((c & 0xC0) == 0xC0) {
			off = ((c & 0x3F) << 8) | m->b[off + 1];
			jumps++;
			continue;
		}
		if (c == 0) {
			return i == nm->n;
		}
		if (i >= nm->n || c != nm->len[i] || memcmp(&m->b[off + 1], nm->p[i], c) != 0) {
			return false;
		}
		off += 1 + c;
		i++;
	}
	return false;
}

static void put_name(msg_t *m, const name_t *nm, bool compress)
{
	for (int i = 0; i < nm->n; i++) {
		if (compress) {
			for (int k = 0; k < m->noffs; k++) {
				if (suffix_at(m, m->offs[k], nm, i)) {
					put16(m, (uint16_t)(0xC000u | m->offs[k]));
					return;
				}
			}
			if (m->noffs < OFFSETS_MAX && m->len < 0x3FFF && !m->over) {
				m->offs[m->noffs++] = (uint16_t)m->len;
			}
		}
		put8(m, nm->len[i]);
		put(m, nm->p[i], nm->len[i]);
	}
	put8(m, 0);
}

static void put_kv(msg_t *m, const char *k, const char *v)
{
	size_t kl = strlen(k), vl = strlen(v);
	if (kl + vl > 255) {
		vl = 255 - kl;
	}
	put8(m, (uint8_t)(kl + vl));
	put(m, k, (int)kl);
	put(m, v, (int)vl);
}

static void put_txt(msg_t *m)
{
	char num[11];
	put_kv(m, "board=", id.board != NULL ? id.board : "");
	utoa10(num, id.dmr_id);
	put_kv(m, "id=", num);
	if (id.callsign[0] != '\0') {
		put_kv(m, "call=", id.callsign);
	}
	put_kv(m, "fw=", id.firmware != NULL ? id.firmware : "");
}

static void make_owner(name_t *nm, int rec)
{
	if (rec < REC_SVC0) {
		make_name(nm, N_HOST, 0);
		return;
	}
	int s = (rec - REC_SVC0) / K_COUNT, k = (rec - REC_SVC0) % K_COUNT;
	make_name(nm, k == K_PTR ? N_TYPE : k == K_ENUM ? N_ENUM : N_INST, s);
}

// One resource record; false (and nothing written) when it doesn't fit
static bool put_record(msg_t *m, int rec, int how)
{
	int len0 = m->len;
	uint8_t noffs0 = m->noffs;
	int s = rec < REC_SVC0 ? 0 : (rec - REC_SVC0) / K_COUNT;
	int k = rec < REC_SVC0 ? -1 : (rec - REC_SVC0) % K_COUNT;
	uint16_t type;
	uint32_t ttl;
	bool unique;
	name_t nm;

	if (rec == REC_A) {
		type = TYPE_A, ttl = TTL_HOST, unique = true;
	} else if (rec == REC_HNSEC) {
		type = TYPE_NSEC, ttl = TTL_HOST, unique = true;
	} else if (k == K_PTR || k == K_ENUM) {
		type = TYPE_PTR, ttl = TTL_OTHER, unique = false;
	} else if (k == K_SRV) {
		type = TYPE_SRV, ttl = TTL_HOST, unique = true;
	} else if (k == K_TXT) {
		type = TYPE_TXT, ttl = TTL_OTHER, unique = true;
	} else {
		type = TYPE_NSEC, ttl = TTL_HOST, unique = true;
	}
	if (how == HOW_GOODBYE) {
		ttl = 0;
	} else if (how == HOW_LEGACY && ttl > TTL_LEGACY) {
		ttl = TTL_LEGACY;
	}

	make_owner(&nm, rec);
	put_name(m, &nm, true);
	put16(m, type);
	put16(m, (uint16_t)(CLASS_IN | ((unique && how != HOW_LEGACY && how != HOW_PROBE) ? CACHE_FLUSH : 0)));
	put32(m, ttl);
	int rdlen_at = m->len;
	put16(m, 0);
	int rd0 = m->len;
	switch (type) {
	case TYPE_A:
		put32(m, id.ip);
		break;
	case TYPE_PTR:
		make_name(&nm, k == K_PTR ? N_INST : N_TYPE, s);
		put_name(m, &nm, true);
		break;
	case TYPE_SRV:
		put16(m, 0);
		put16(m, 0);
		put16(m, svc[s].port);
		make_name(&nm, N_HOST, 0);
		put_name(m, &nm, true);
		break;
	case TYPE_TXT:
		put_txt(m);
		break;
	default:	// NSEC: the next name is the owner itself, uncompressed, then the type bitmap of window 0
		make_owner(&nm, rec);
		put_name(m, &nm, false);
		put8(m, 0);
		if (rec == REC_HNSEC) {
			put8(m, 1);
			put8(m, 0x40);		// A (1)
		} else {
			static const uint8_t bits[5] = { 0, 0, 0x80, 0, 0x40 };	// TXT (16), SRV (33)
			put8(m, 5);
			put(m, bits, 5);
		}
		break;
	}
	if (m->over) {
		m->len = len0;
		m->noffs = noffs0;
		m->over = false;
		return false;
	}
	m->b[rdlen_at] = (uint8_t)((m->len - rd0) >> 8);
	m->b[rdlen_at + 1] = (uint8_t)(m->len - rd0);
	return true;
}

static bool msg_begin(msg_t *m)
{
	memset(m, 0, sizeof(*m));
	m->b = io->buffer(&m->max);
	if (m->b == NULL || m->max < 12) {
		return false;
	}
	m->len = 12;
	return true;
}

static bool msg_send(msg_t *m, uint16_t qid, uint16_t flags, uint32_t ip, uint16_t port)
{
	uint8_t *h = m->b;
	h[0] = (uint8_t)(qid >> 8);
	h[1] = (uint8_t)qid;
	h[2] = (uint8_t)(flags >> 8);
	h[3] = (uint8_t)flags;
	for (int i = 0; i < 4; i++) {
		h[4 + 2 * i] = (uint8_t)(m->count[i] >> 8);
		h[5 + 2 * i] = (uint8_t)m->count[i];
	}
	if (!io->send(ip, port, m->b, m->len)) {
		stats.busy++;
		return false;
	}
	stats.sent++;
	return true;
}

// The records of one group: the host's A and NSEC, a service's PTR, SRV, TXT, enumeration PTR and NSEC
static recset_t group_records(int g)
{
	if (g == 0) {
		return BIT(REC_A) | BIT(REC_HNSEC);
	}
	recset_t r = 0;
	for (int k = 0; k < K_COUNT; k++) {
		r |= BIT(REC(g - 1, k));
	}
	return r;
}

// A response from the record sets: answers first, then as many additional records as fit. Sets the bits it wrote
// in *sent. False: nothing to send or busy
static bool send_response(recset_t ans, recset_t add, int how, recset_t *sent, uint32_t ip, uint16_t port,
			  uint16_t qid, int qname, int qs, uint16_t qtype)
{
	msg_t m;
	*sent = 0;
	if (!msg_begin(&m)) {
		return false;
	}
	if (how == HOW_LEGACY) {
		name_t nm;
		make_name(&nm, qname, qs);
		put_name(&m, &nm, true);
		put16(&m, qtype);
		put16(&m, CLASS_IN);
		m.count[0] = 1;
	}
	for (int r = 0; r < 64; r++) {
		if ((ans & BIT(r)) != 0) {
			if (!put_record(&m, r, how)) {
				break;
			}
			m.count[1]++;
			*sent |= BIT(r);
		}
	}
	if ((ans & ~*sent) == 0) {	// additional records only when every answer fitted
		for (int r = 0; r < 64; r++) {
			if ((add & ~ans & BIT(r)) != 0 && put_record(&m, r, how)) {
				m.count[3]++;
			}
		}
		*sent |= add;
	}
	if (m.count[1] == 0 && m.count[3] == 0) {
		*sent = ans | add;	// nothing fits (can't happen with sane names): dropped, not retried forever
		return false;
	}
	if (!msg_send(&m, qid, 0x8400, ip, port)) {	// response, authoritative
		*sent = 0;
		return false;
	}
	if (ip == MDNS_GROUP) {
		stamp_mcast(*sent & ans, now_ms);
	}
	return true;
}

// A probe for one group: the question "ANY <name>" (unicast response wanted) and the proposed records as authority
static bool send_probe(int g)
{
	msg_t m;
	name_t nm;
	if (!msg_begin(&m)) {
		return false;
	}
	make_name(&nm, g == 0 ? N_HOST : N_INST, g - 1);
	put_name(&m, &nm, true);
	put16(&m, TYPE_ANY);
	put16(&m, (uint16_t)(CLASS_IN | QU_BIT));
	m.count[0] = 1;
	if (g == 0) {
		m.count[2] += put_record(&m, REC_A, HOW_PROBE);
	} else {
		m.count[2] += put_record(&m, REC(g - 1, K_SRV), HOW_PROBE);
		m.count[2] += put_record(&m, REC(g - 1, K_TXT), HOW_PROBE);
	}
	return msg_send(&m, 0, 0, MDNS_GROUP, MDNS_PORT);
}

static bool send_group(int g, int how)
{
	recset_t r = group_records(g), sent;
	if (how == HOW_GOODBYE) {
		r &= ~(BIT(REC_HNSEC) | (g > 0 ? BIT(REC(g - 1, K_NSEC)) : 0));
	}
	return send_response(r, 0, how, &sent, MDNS_GROUP, MDNS_PORT, 0, 0, 0, 0);
}

// ---- identity ----

static void make_labels(void)
{
	char num[11];

	strcpy(host_label, "odr-");
	utoa10(num, id.dmr_id);
	cat(host_label, sizeof(host_label), num);
	inst_label[0] = '\0';
	if (id.callsign[0] != '\0') {
		cat(inst_label, sizeof(inst_label), id.callsign);
		cat(inst_label, sizeof(inst_label), " ");
	}
	cat(inst_label, sizeof(inst_label), id.board != NULL ? id.board : "ODR");
	cat(inst_label, sizeof(inst_label), " (");
	cat(inst_label, sizeof(inst_label), num);
	cat(inst_label, sizeof(inst_label), ")");
	if (rename_n > 0) {
		utoa10(num, rename_n + 1u);
		cat(host_label, sizeof(host_label), "-");
		cat(host_label, sizeof(host_label), num);
		cat(inst_label, sizeof(inst_label), " (");
		cat(inst_label, sizeof(inst_label), num);
		cat(inst_label, sizeof(inst_label), ")");
	}
}

static void start_probing(uint32_t now, uint32_t delay)
{
	state = MDNS_PROBING;
	step = 0;
	next_ms = now + delay;
	pend_probe = pend_announce = 0;
	pend_ans = pend_add = 0;
	uni.on = false;
}

static void all_groups(uint16_t *set)
{
	*set = (uint16_t)((1u << (1 + nsvc)) - 1u);
}

static void go_off(void)
{
	state = MDNS_OFF;
	host_heard = false;
	pend_probe = pend_announce = 0;
	pend_ans = pend_add = 0;
	uni.on = false;
}

static void conflict(uint32_t now)
{
	stats.conflicts++;
	if (rename_n < 250) {
		rename_n++;
	}
	make_labels();
	start_probing(now, 0);
}

// ---- receiving ----

// A response from someone else holding one of our unique records with other data: a conflict
static bool conflicting(const uint8_t *msg, int length, int which, int s, uint16_t type, int rd, int rdlen)
{
	if (which == N_HOST && type == TYPE_A) {
		return rdlen != 4 || get32(&msg[rd]) != id.ip;
	}
	if (which == N_INST && type == TYPE_SRV) {
		name_t nm;
		int end;
		make_name(&nm, N_HOST, 0);
		return rdlen < 7 || get16(&msg[rd + 4]) != svc[s].port || !name_match(msg, length, rd + 6, &nm, &end);
	}
	if (which == N_INST && type == TYPE_TXT) {
		uint8_t b[160];
		msg_t m = { .b = b, .max = sizeof(b) };
		put_txt(&m);
		return m.over || rdlen != m.len || memcmp(b, &msg[rd], (size_t)rdlen) != 0;
	}
	return false;
}

// Simultaneous probe tie-break (RFC 6762 8.2) on the first record of each sorted set: for the host its A record, for
// an instance its TXT (TXT 16 sorts before SRV 33). Their record of that type compared with ours, class, then rdata
// byte by byte; their other records are passed over. > 0: theirs is greater, we lose
static int tie_break(const uint8_t *msg, int which, uint16_t type, uint16_t cls, int rd, int rdlen)
{
	uint8_t ours[160];
	int n;
	uint16_t our_type;
	if (which == N_HOST) {
		our_type = TYPE_A;
		ours[0] = (uint8_t)(id.ip >> 24);
		ours[1] = (uint8_t)(id.ip >> 16);
		ours[2] = (uint8_t)(id.ip >> 8);
		ours[3] = (uint8_t)id.ip;
		n = 4;
	} else {
		msg_t m = { .b = ours, .max = sizeof(ours) };
		our_type = TYPE_TXT;
		put_txt(&m);
		n = m.len;
	}
	if (type != our_type) {
		return 0;
	}
	if ((cls & 0x7FFF) != CLASS_IN) {
		return (cls & 0x7FFF) > CLASS_IN ? 1 : -1;
	}
	for (int i = 0; i < n && i < rdlen; i++) {
		if (msg[rd + i] != ours[i]) {
			return msg[rd + i] > ours[i] ? 1 : -1;
		}
	}
	return rdlen > n ? 1 : rdlen < n ? -1 : 0;
}

// The records a question asks for: answers, additional ones, and for a type we don't have the NSEC that says so
// (with what there is, in the additional section: RFC 6762 6.1)
static void question(int which, int s, uint16_t qtype, recset_t *ans, recset_t *add, recset_t *neg)
{
	bool any = qtype == TYPE_ANY;
	switch (which) {
	case N_HOST:
		if (any || qtype == TYPE_A) {
			*ans |= BIT(REC_A);
		} else {
			*neg |= BIT(REC_HNSEC) | BIT(REC_A);
		}
		break;
	case N_ENUM:
		if (any || qtype == TYPE_PTR) {
			for (int i = 0; i < nsvc; i++) {
				*ans |= BIT(REC(i, K_ENUM));
			}
		}
		break;
	case N_TYPE:
		if (any || qtype == TYPE_PTR) {
			*ans |= BIT(REC(s, K_PTR));
			*add |= BIT(REC(s, K_SRV)) | BIT(REC(s, K_TXT)) | BIT(REC_A);	// RFC 6763 12.1
		}
		break;
	case N_INST:
		if (any || qtype == TYPE_SRV) {
			*ans |= BIT(REC(s, K_SRV));
			*add |= BIT(REC_A);
		}
		if (any || qtype == TYPE_TXT) {
			*ans |= BIT(REC(s, K_TXT));
		}
		if (!any && qtype != TYPE_SRV && qtype != TYPE_TXT) {
			*neg |= BIT(REC(s, K_NSEC)) | BIT(REC(s, K_SRV)) | BIT(REC(s, K_TXT));
		}
		break;
	default:
		break;
	}
}

void mdnsReceive(uint32_t srcIp, uint16_t srcPort, const uint8_t *msg, int length, uint32_t now)
{
	if (state == MDNS_OFF || io == NULL) {
		return;
	}
	now_ms = now;
	if (length < 12) {
		stats.bad++;
		return;
	}
	uint16_t qid = get16(&msg[0]), flags = get16(&msg[2]);
	int qd = get16(&msg[4]), an = get16(&msg[6]), ns = get16(&msg[8]), ar = get16(&msg[10]);
	bool response = (flags & 0x8000u) != 0;
	bool legacy = srcPort != MDNS_PORT;
	int off = 12;

	if ((flags & 0x7800u) != 0 || (flags & 0x000Fu) != 0) {
		return;		// opcode and rcode must be 0 (RFC 6762 18.3, 18.11)
	}
	if (!host_heard) {
		host_heard = true;	// the host's responder is there now: tell it again, in case it missed the first
		if (state == MDNS_RUNNING) {
			state = MDNS_ANNOUNCING;
			step = 0;
			next_ms = now;
		}
	}

	recset_t ans = 0, add = 0, neg = 0, known = 0;
	bool qu = false, probe = false;
	int qname = -1, qsvc = 0;
	uint16_t qtype0 = 0;

	if (!response) {
		stats.queries++;
	}
	for (int i = 0; i < qd; i++) {
		int s, end;
		int which = our_name(msg, length, off, &s, &end);
		if (end < 0 || end + 4 > length) {
			stats.bad++;
			return;
		}
		uint16_t qtype = get16(&msg[end]), qclass = get16(&msg[end + 2]);
		off = end + 4;
		if (response || which < 0 || ((qclass & 0x7FFF) != CLASS_IN && (qclass & 0x7FFF) != CLASS_ANY)) {
			continue;
		}
		question(which, s, qtype, &ans, &add, &neg);
		qu |= (qclass & QU_BIT) != 0;
		if (qname < 0) {
			qname = which;
			qsvc = s;
			qtype0 = qtype;
		}
	}
	// Answer, authority and additional records: known answers in a query, the probed records of someone probing,
	// conflicting records in a response
	for (int i = 0; i < an + ns + ar; i++) {
		int s, end;
		int which = our_name(msg, length, off, &s, &end);
		if (end < 0 || end + 10 > length) {
			stats.bad++;
			return;
		}
		uint16_t type = get16(&msg[end]), cls = get16(&msg[end + 2]);
		uint32_t ttl = get32(&msg[end + 4]);
		int rdlen = get16(&msg[end + 8]), rd = end + 10;
		if (rd + rdlen > length) {
			stats.bad++;
			return;
		}
		off = rd + rdlen;
		if (which < 0) {
			continue;
		}
		if (response) {
			if (ttl != 0 && conflicting(msg, length, which, s, type, rd, rdlen)) {
				conflict(now);
				return;
			}
		} else if (i < an) {
			// Known answer suppression (RFC 6762 7.1): a PTR the querier holds with at least half our TTL
			if (type == TYPE_PTR && ttl >= TTL_OTHER / 2u && (which == N_TYPE || which == N_ENUM)) {
				name_t target;
				int e;
				make_name(&target, which == N_TYPE ? N_INST : N_TYPE, s);
				if (name_match(msg, length, rd, &target, &e)) {
					known |= BIT(which == N_TYPE ? REC(s, K_PTR) : REC(s, K_ENUM));
				}
			}
		} else if (i < an + ns && (which == N_HOST || which == N_INST)) {
			probe = true;	// someone probes one of our names
			if (state == MDNS_PROBING && tie_break(msg, which, type, cls, rd, rdlen) > 0) {
				start_probing(now, DEFER_MS);	// we lost: wait, then probe again (RFC 6762 8.2)
				return;
			}
		}
	}
	if (response || (state != MDNS_RUNNING && state != MDNS_ANNOUNCING)) {
		return;		// while probing nothing is ours yet
	}
	ans &= ~known;
	if (ans == 0 && neg == 0) {
		return;		// nothing asked for, or the querier knows it all
	}
	add = (add | neg) & ~known & ~ans;
	stats.answered++;
	if (legacy || qu) {
		uni.on = true;
		uni.legacy = legacy;
		uni.ip = srcIp;
		uni.port = srcPort;
		uni.qid = qid;
		uni.qname = (uint8_t)qname;
		uni.qsvc = (uint8_t)qsvc;
		uni.qtype = qtype0;
		uni.ans = ans;
		uni.add = add;
		return;
	}
	// Multicast: each record set at most once a second (a quarter second when defending against a probe)
	uint32_t gap = probe ? RATE_PROBE_MS : RATE_MS;
	bool asked = ans != 0;
	for (int r = 0; r < 64; r++) {
		if ((ans & BIT(r)) != 0 && mcast_recently(r, now, gap)) {
			ans &= ~BIT(r);
		}
	}
	if (asked && ans == 0) {
		return;		// all of it went out a moment ago
	}
	pend_ans |= ans;
	pend_add |= add;
}

// ---- timers and sending ----

static void poll_identity(uint32_t now, bool link_up)
{
	if (identity_read && now - identity_ms < IDENTITY_MS && link_up == was_up) {
		return;
	}
	identity_read = true;
	was_up = link_up;
	identity_ms = now;
	mdns_identity_t n;
	memset(&n, 0, sizeof(n));
	io->identity(&n);
	n.callsign[sizeof(n.callsign) - 1] = '\0';
	if (!link_up || n.ip == 0) {
		if (state != MDNS_OFF) {
			go_off();
		}
		id = n;
		return;
	}
	bool changed = n.ip != id.ip || n.dmr_id != id.dmr_id || strcmp(n.callsign, id.callsign) != 0 ||
		       n.board != id.board || n.firmware != id.firmware;
	if (state != MDNS_OFF && !changed) {
		return;
	}
	if (changed && (state == MDNS_RUNNING || state == MDNS_ANNOUNCING)) {
		for (int g = 0; g <= nsvc; g++) {
			if (!send_group(g, HOW_GOODBYE)) {
				break;		// best effort (RFC 6762 10.1)
			}
		}
	}
	id = n;
	rename_n = 0;
	make_labels();
	start_probing(now, io->random != NULL ? io->random() % PROBE_GAP_MS : 0);	// RFC 6762 8.1
}

void mdnsTick(uint32_t now, bool link_up)
{
	if (io == NULL) {
		return;
	}
	now_ms = now;
	poll_identity(now, link_up);
	if (state == MDNS_OFF) {
		return;
	}
	if ((int32_t)(now - next_ms) >= 0) {
		if (state == MDNS_PROBING) {
			if (step < PROBES) {
				all_groups(&pend_probe);
				step++;
				next_ms = now + PROBE_GAP_MS;
			} else {
				state = MDNS_ANNOUNCING;	// nobody objected within 250 ms of the third probe
				step = 0;
				next_ms = now;
			}
		}
		if (state == MDNS_ANNOUNCING) {
			if (step < ANNOUNCES) {
				all_groups(&pend_announce);
				step++;
				next_ms = now + ANNOUNCE_GAP_MS;
			} else {
				state = MDNS_RUNNING;
			}
		}
	}
	for (int budget = 0; budget < SEND_BUDGET; budget++) {
		recset_t sent;
		if (pend_probe != 0) {
			int g = __builtin_ctz(pend_probe);
			if (!send_probe(g)) {
				break;
			}
			pend_probe &= (uint16_t)~(1u << g);
		} else if (pend_announce != 0) {
			int g = __builtin_ctz(pend_announce);
			if (!send_group(g, HOW_NORMAL)) {
				break;
			}
			pend_announce &= (uint16_t)~(1u << g);
		} else if (uni.on) {
			bool ok = send_response(uni.ans, uni.add, uni.legacy ? HOW_LEGACY : HOW_NORMAL, &sent, uni.ip, uni.port,
						uni.legacy ? uni.qid : 0, uni.qname, uni.qsvc, uni.qtype);
			uni.ans &= ~sent;
			uni.add &= ~sent;
			uni.on = uni.ans != 0 || uni.add != 0;
			if (!ok) {
				break;
			}
		} else if (pend_ans != 0 || pend_add != 0) {
			bool ok = send_response(pend_ans, pend_add, HOW_NORMAL, &sent, MDNS_GROUP, MDNS_PORT, 0, 0, 0, 0);
			pend_ans &= ~sent;
			pend_add &= ~sent;
			if (!ok) {
				break;
			}
		} else {
			break;
		}
	}
	stats.state = state;
}

void mdnsInit(const mdns_io_t *i, const mdns_service_t *services, int count)
{
	io = i;
	svc = services;
	nsvc = (uint8_t)(count < 0 ? 0 : count > MDNS_SERVICES_MAX ? MDNS_SERVICES_MAX : count);
	memset(&stats, 0, sizeof(stats));
	memset(&id, 0, sizeof(id));
	mcast_once = 0;
	identity_read = false;
	was_up = false;
	rename_n = 0;
	host_label[0] = inst_label[0] = '\0';
	go_off();
}

void mdnsStats(mdns_stats_t *s)
{
	*s = stats;
	s->state = state;
}

const char *mdnsHostName(void)
{
	return state == MDNS_OFF ? "" : host_label;
}
