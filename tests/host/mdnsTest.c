// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mDNS responder (core/net/mdns.c) behind the real IP gateway (core/dmr/ipGateway.c): host frames in, the
// gateway's frames out. Probing, announcing, queries (multicast, QU, legacy unicast, known answers, negative answers,
// enumeration), the rate limit, conflicts and the probe tie-break, goodbyes, link down, malformed messages, a busy
// endpoint; and the gateway's local port table (224.0.0.0/24 never to the air, TTL 255, the multicast MAC). Every
// frame is written to mdnsTest.pcap for tshark's DNS dissector (Makefile).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "dmr/ipGateway.h"
#include "net/mdns.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RADIO_ID	10005u
#define HOST_IP		(IPGW_INDIVIDUAL_NET | RADIO_ID)	// 12.0.39.21
#define RADIO_IP	(HOST_IP ^ 1u)
#define OTHER_IP	0x0C000063u

static const uint8_t HOST_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x04 };
static const uint8_t RADIO_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x05 };

static FILE *pcap;
static uint32_t now = 1000;

// ---- what the gateway needs ----
static uint8_t out[16][700];
static int out_len[16], out_count;
static int busy;
static int to_air;

bool ipGatewaySendFrame(const uint8_t *frame, int length)
{
	if (busy > 0) {
		busy--;
		return false;
	}
	uint32_t rec[4] = { now / 1000u, (now % 1000u) * 1000u, (uint32_t)length, (uint32_t)length };
	fwrite(rec, sizeof(rec), 1, pcap);
	fwrite(frame, (size_t)length, 1, pcap);
	if (out_count < 16) {
		memcpy(out[out_count], frame, (size_t)length);
		out_len[out_count] = length;
	}
	out_count++;
	return true;
}
uint32_t ipGatewayRadioId(void) { return RADIO_ID; }
void ipGatewaySerialIn(const uint8_t *data, int length) { (void)data; (void)length; }
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length) { to_air++; return true; }
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length) { to_air++; return true; }

// ---- the responder's io ----
static mdns_identity_t ident = { .ip = RADIO_IP, .dmr_id = RADIO_ID, .callsign = "N0CALL", .board = "DM-1701",
				 .firmware = "abc1234" };
static void io_identity(mdns_identity_t *id) { *id = ident; }
static uint8_t *io_buffer(int *max) { *max = IPGW_UDP_PAYLOAD_MAX; return ipGatewayUdpPayload(); }
static bool io_send(uint32_t ip, uint16_t port, const uint8_t *msg, int len) { return ipGatewayUdpSend(ip, MDNS_PORT, port, msg, len); }
static uint32_t io_random(void) { return 100; }
static const mdns_io_t io = { io_identity, io_buffer, io_send, io_random };

static const mdns_service_t services[] = {
	{ "_rfoundry-debug._udp", 3334 }, { "_mmdvm._udp", 3334 }, { "_tftp._udp", 69 },
};

static void handler(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	mdnsReceive(src, sport, data, length, now);
}

// ---- frames from the host ----
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }

static uint16_t csum(const uint8_t *p, int len, uint32_t sum)
{
	for (; len > 1; p += 2, len -= 2) {
		sum += get16(p);
	}
	if (len) {
		sum += (uint32_t)p[0] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static void from_host(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, int len)
{
	uint8_t f[700];
	memset(f, 0, sizeof(f));
	if ((dst >> 28) == 0xE) {
		uint8_t m[6] = { 0x01, 0x00, 0x5E, (uint8_t)((dst >> 16) & 0x7F), (uint8_t)(dst >> 8), (uint8_t)dst };
		memcpy(f, m, 6);
	} else {
		memcpy(f, RADIO_MAC, 6);
	}
	memcpy(f + 6, HOST_MAC, 6);
	put16(f + 12, 0x0800);
	uint8_t *ip = f + 14, *udp = f + 34;
	ip[0] = 0x45;
	put16(ip + 2, (uint16_t)(28 + len));
	ip[8] = 255;
	ip[9] = 17;
	put32(ip + 12, src);
	put32(ip + 16, dst);
	put16(ip + 10, csum(ip, 20, 0));
	put16(udp, sport);
	put16(udp + 2, dport);
	put16(udp + 4, (uint16_t)(8 + len));
	memcpy(udp + 8, data, (size_t)len);
	ipGatewayEthernetIn(f, 42 + len);
}

// ---- a small DNS reader for the checks ----
typedef struct {
	char name[256];
	uint16_t type, cls;
	uint32_t ttl;
	char target[256];	// PTR / SRV target, NSEC next name
	uint16_t port;
	uint32_t a;
	char txt[256];		// TXT strings joined with ';'
	int section;		// 1 answer, 2 authority, 3 additional
} rr_t;

typedef struct {
	uint16_t id, flags;
	int qd, an, ns, ar;
	char qname[256];
	uint16_t qtype, qclass;
	rr_t rr[32];
	int nrr;
	int length;
	uint32_t dst_ip, src_ip;
	uint16_t dst_port, src_port;
	uint8_t ttl;
	bool mcast_mac;
} dns_t;

static int read_name(const uint8_t *m, int len, int off, char *name)
{
	int end = -1, jumps = 0, n = 0;
	name[0] = '\0';
	while (off < len) {
		uint8_t c = m[off];
		if ((c & 0xC0) == 0xC0) {
			if (end < 0) {
				end = off + 2;
			}
			off = ((c & 0x3F) << 8) | m[off + 1];
			if (++jumps > 20) {
				return -1;
			}
			continue;
		}
		if (c == 0) {
			if (n > 0) {
				name[n - 1] = '\0';
			}
			return end >= 0 ? end : off + 1;
		}
		memcpy(name + n, m + off + 1, c);
		n += c;
		name[n++] = '.';
		name[n] = '\0';
		off += 1 + c;
	}
	return -1;
}

static bool parse(int index, dns_t *d)
{
	const uint8_t *f = out[index];
	memset(d, 0, sizeof(*d));
	d->mcast_mac = f[0] == 0x01 && f[1] == 0x00 && f[2] == 0x5E;
	d->ttl = f[14 + 8];
	d->src_ip = get32(f + 26);
	d->dst_ip = get32(f + 30);
	d->src_port = get16(f + 34);
	d->dst_port = get16(f + 36);
	const uint8_t *m = f + 42;
	int len = out_len[index] - 42;
	d->length = len;
	d->id = get16(m);
	d->flags = get16(m + 2);
	d->qd = get16(m + 4);
	d->an = get16(m + 6);
	d->ns = get16(m + 8);
	d->ar = get16(m + 10);
	int off = 12;
	for (int i = 0; i < d->qd; i++) {
		char tmp[256];
		off = read_name(m, len, off, i == 0 ? d->qname : tmp);
		if (off < 0) {
			return false;
		}
		if (i == 0) {
			d->qtype = get16(m + off);
			d->qclass = get16(m + off + 2);
		}
		off += 4;
	}
	for (int i = 0; i < d->an + d->ns + d->ar && i < 32; i++) {
		rr_t *r = &d->rr[d->nrr++];
		off = read_name(m, len, off, r->name);
		if (off < 0) {
			return false;
		}
		r->type = get16(m + off);
		r->cls = get16(m + off + 2);
		r->ttl = get32(m + off + 4);
		int rdlen = get16(m + off + 8), rd = off + 10;
		r->section = i < d->an ? 1 : i < d->an + d->ns ? 2 : 3;
		if (r->type == 1) {
			r->a = get32(m + rd);
		} else if (r->type == 12 || r->type == 47) {
			read_name(m, len, rd, r->target);
		} else if (r->type == 33) {
			r->port = get16(m + rd + 4);
			read_name(m, len, rd + 6, r->target);
		} else if (r->type == 16) {
			int n = 0;
			for (int k = rd; k < rd + rdlen;) {
				memcpy(r->txt + n, m + k + 1, m[k]);
				n += m[k];
				r->txt[n++] = ';';
				k += 1 + m[k];
			}
			r->txt[n] = '\0';
		}
		off = rd + rdlen;
	}
	return off == len;
}

static const rr_t *find(const dns_t *d, const char *name, uint16_t type, int section)
{
	for (int i = 0; i < d->nrr; i++) {
		if (strcmp(d->rr[i].name, name) == 0 && d->rr[i].type == type && (section == 0 || d->rr[i].section == section)) {
			return &d->rr[i];
		}
	}
	return NULL;
}

// ---- building queries ----
typedef struct {
	uint8_t b[512];
	int len;
	int qd, an, ns;
} q_t;

static void q_begin(q_t *q, uint16_t id)
{
	memset(q, 0, sizeof(*q));
	put16(q->b, id);
	q->len = 12;
}

static void q_name(q_t *q, const char *name)
{
	while (*name != '\0') {
		const char *dot = strchr(name, '.');
		int n = dot != NULL ? (int)(dot - name) : (int)strlen(name);
		q->b[q->len++] = (uint8_t)n;
		memcpy(q->b + q->len, name, (size_t)n);
		q->len += n;
		name += n + (dot != NULL);
	}
	q->b[q->len++] = 0;
}

static void q_question(q_t *q, const char *name, uint16_t type, uint16_t cls)
{
	q_name(q, name);
	put16(q->b + q->len, type);
	put16(q->b + q->len + 2, cls);
	q->len += 4;
	q->qd++;
}

// A record with a name-valued rdata (PTR) or a raw one
static void q_rr(q_t *q, const char *name, uint16_t type, uint16_t cls, uint32_t ttl, const uint8_t *rd, int rdlen,
		 const char *rdname)
{
	q_name(q, name);
	put16(q->b + q->len, type);
	put16(q->b + q->len + 2, cls);
	put32(q->b + q->len + 4, ttl);
	int at = q->len + 8;
	q->len += 10;
	int start = q->len;
	if (rdname != NULL) {
		q_name(q, rdname);
	} else {
		memcpy(q->b + q->len, rd, (size_t)rdlen);
		q->len += rdlen;
	}
	put16(q->b + at, (uint16_t)(q->len - start));
}

static void q_send(q_t *q, uint16_t flags, uint32_t src, uint16_t sport, uint32_t dst)
{
	put16(q->b + 2, flags);
	put16(q->b + 4, (uint16_t)q->qd);
	put16(q->b + 6, (uint16_t)q->an);
	put16(q->b + 8, (uint16_t)q->ns);
	from_host(src, dst, sport, MDNS_PORT, q->b, q->len);
}

static void tick_link(uint32_t ms, bool up)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		mdnsTick(now, up);
	}
}

static void tick_for(uint32_t ms)
{
	tick_link(ms, true);
}

static void query(const char *name, uint16_t type, uint16_t cls, uint16_t sport, uint16_t id)
{
	q_t q;
	q_begin(&q, id);
	q_question(&q, name, type, cls);
	q_send(&q, 0, HOST_IP, sport, MDNS_GROUP);
}

#define HOST	"rfoundry-10005.local"
#define INST	"N0CALL DM-1701 (10005)"

// Up: three probe rounds, then two announcements
static void bring_up(void)
{
	mdnsInit(&io, services, 3);
	out_count = 0;
	mdnsTick(now, true);
	tick_for(2000);
}

// The host's responder has spoken once (that announces again, as it may have missed the first ones)
static void hear_host(void)
{
	query("other.local", 1, 1, MDNS_PORT, 0);
	tick_for(2100);
}

static void test_probe_announce(void)
{
	dns_t d;
	mdnsInit(&io, services, 3);
	out_count = 0;
	mdnsTick(now, false);
	tick_link(500, false);
	CHECK(out_count == 0);			// link down: quiet
	mdnsTick(now, true);
	CHECK(out_count == 0);			// the first probe waits 0-250 ms
	tick_for(100);
	CHECK(out_count == 4);			// host + three instances
	CHECK(parse(0, &d));
	CHECK(d.flags == 0 && d.qd == 1 && d.ns == 1 && strcmp(d.qname, HOST) == 0);
	CHECK(d.qtype == 255 && d.qclass == 0x8001);	// ANY, unicast response wanted
	CHECK(d.rr[0].type == 1 && d.rr[0].a == RADIO_IP && d.rr[0].cls == 1 && d.rr[0].section == 2);
	CHECK(d.dst_ip == MDNS_GROUP && d.dst_port == MDNS_PORT && d.src_ip == RADIO_IP && d.ttl == 255 && d.mcast_mac);
	CHECK(parse(3, &d));
	CHECK(strcmp(d.qname, INST "._tftp._udp.local") == 0 && d.ns == 2);
	CHECK(find(&d, INST "._tftp._udp.local", 33, 2) != NULL && find(&d, INST "._tftp._udp.local", 33, 2)->port == 69);
	tick_for(250);
	CHECK(out_count == 8);
	tick_for(250);
	CHECK(out_count == 12);
	out_count = 0;
	tick_for(250);
	CHECK(out_count == 4);			// the first announcement: host + three services
	CHECK(parse(0, &d));
	CHECK(d.flags == 0x8400 && d.qd == 0 && d.an == 2);
	const rr_t *a = find(&d, HOST, 1, 1);
	CHECK(a != NULL && a->a == RADIO_IP && a->ttl == 120 && a->cls == 0x8001);
	const rr_t *nsec = find(&d, HOST, 47, 1);
	CHECK(nsec != NULL && strcmp(nsec->target, HOST) == 0);
	CHECK(parse(3, &d));
	const rr_t *ptr = find(&d, "_tftp._udp.local", 12, 1);
	CHECK(ptr != NULL && strcmp(ptr->target, INST "._tftp._udp.local") == 0 && ptr->cls == 1 && ptr->ttl == 4500);
	const rr_t *srv = find(&d, INST "._tftp._udp.local", 33, 1);
	CHECK(srv != NULL && srv->port == 69 && strcmp(srv->target, HOST) == 0 && srv->cls == 0x8001);
	const rr_t *txt = find(&d, INST "._tftp._udp.local", 16, 1);
	CHECK(txt != NULL && strcmp(txt->txt, "board=DM-1701;id=10005;call=N0CALL;fw=abc1234;") == 0);
	const rr_t *en = find(&d, "_services._dns-sd._udp.local", 12, 1);
	CHECK(en != NULL && strcmp(en->target, "_tftp._udp.local") == 0);
	CHECK(d.length < 260);			// names compressed
	tick_for(1000);
	CHECK(out_count == 8);			// the second, a second later
	tick_for(3000);
	CHECK(out_count == 8);
	mdns_stats_t st;
	mdnsStats(&st);
	CHECK(st.state == MDNS_RUNNING && strcmp(mdnsHostName(), "rfoundry-10005") == 0);
}

static void test_queries(void)
{
	dns_t d;
	bring_up();
	hear_host();

	// A, multicast
	out_count = 0;
	query(HOST, 1, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d));
	CHECK(d.dst_ip == MDNS_GROUP && d.an == 1 && find(&d, HOST, 1, 1) != NULL);
	// the same within a second: not again; after it: again
	query(HOST, 1, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1);
	tick_for(1000);
	query("RFOUNDRY-10005.LOCAL", 1, 1, MDNS_PORT, 0);	// names are case insensitive
	tick_for(5);
	CHECK(out_count == 2);

	// QU: unicast to the querier
	out_count = 0;
	query(HOST, 1, 0x8001, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d) && d.dst_ip == HOST_IP && d.dst_port == MDNS_PORT && !d.mcast_mac);
	CHECK(memcmp(out[0], HOST_MAC, 6) == 0);

	// Legacy unicast: the ID and the question echoed, TTL at most 10, no cache flush bit
	out_count = 0;
	query(INST "._tftp._udp.local", 33, 1, 40000, 0x1234);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d));
	CHECK(d.dst_ip == HOST_IP && d.dst_port == 40000 && d.id == 0x1234 && d.qd == 1);
	CHECK(strcmp(d.qname, INST "._tftp._udp.local") == 0 && d.qtype == 33);
	const rr_t *srv = find(&d, INST "._tftp._udp.local", 33, 1);
	CHECK(srv != NULL && srv->ttl == 10 && srv->cls == 1 && srv->port == 69);
	CHECK(find(&d, HOST, 1, 3) != NULL);	// the address as an additional record

	// Browse: the PTR, with SRV, TXT and A as additional records
	tick_for(1100);
	out_count = 0;
	query("_tftp._udp.local", 12, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d));
	CHECK(d.an == 1 && find(&d, "_tftp._udp.local", 12, 1) != NULL);
	CHECK(find(&d, INST "._tftp._udp.local", 33, 3) != NULL && find(&d, INST "._tftp._udp.local", 16, 3) != NULL);
	CHECK(find(&d, HOST, 1, 3) != NULL);

	// Known answer suppression: the browser already holds the PTR with most of its TTL
	tick_for(1100);
	out_count = 0;
	q_t q;
	q_begin(&q, 0);
	q_question(&q, "_tftp._udp.local", 12, 1);
	q_rr(&q, "_tftp._udp.local", 12, 1, 4000, NULL, 0, INST "._tftp._udp.local");
	q.an = 1;
	q_send(&q, 0, HOST_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	CHECK(out_count == 0);
	// ... but not with less than half the TTL left
	q_begin(&q, 0);
	q_question(&q, "_tftp._udp.local", 12, 1);
	q_rr(&q, "_tftp._udp.local", 12, 1, 100, NULL, 0, INST "._tftp._udp.local");
	q.an = 1;
	q_send(&q, 0, HOST_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	CHECK(out_count == 1);

	// Service type enumeration
	out_count = 0;
	query("_services._dns-sd._udp.local", 12, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d) && d.an == 3);
	CHECK(find(&d, "_services._dns-sd._udp.local", 12, 1) != NULL);

	// AAAA: no answer, the NSEC and the A record as additional ones
	tick_for(1100);
	out_count = 0;
	query(HOST, 28, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d) && d.an == 0 && d.ar == 2);
	CHECK(find(&d, HOST, 47, 3) != NULL && find(&d, HOST, 1, 3) != NULL);

	// Names that aren't ours, other classes, responses: nothing
	out_count = 0;
	query("other.local", 1, 1, MDNS_PORT, 0);
	query(HOST, 1, 3, MDNS_PORT, 0);
	tick_for(1100);
	CHECK(out_count == 0);

	// Two questions in one query: one response with both
	out_count = 0;
	q_begin(&q, 0);
	q_question(&q, HOST, 1, 1);
	q_question(&q, INST "._mmdvm._udp.local", 16, 1);
	q_send(&q, 0, HOST_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	CHECK(out_count == 1 && parse(0, &d) && d.an == 2 && find(&d, INST "._mmdvm._udp.local", 16, 1) != NULL);

	// A busy endpoint: the response goes on a later tick
	tick_for(1100);
	out_count = 0;
	busy = 1;
	query(HOST, 1, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 0);
	tick_for(5);
	CHECK(out_count == 1);
	mdns_stats_t st;
	mdnsStats(&st);
	CHECK(st.busy >= 1 && st.bad == 0);
}

static void test_conflict(void)
{
	dns_t d;
	bring_up();
	hear_host();
	// Someone else answers for our host name with another address
	q_t q;
	uint8_t other[4] = { 12, 0, 0, 99 };
	q_begin(&q, 0);
	q_rr(&q, HOST, 1, 0x8001, 120, other, 4, NULL);
	q.an = 1;
	out_count = 0;
	q_send(&q, 0x8400, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	CHECK(out_count == 4 && parse(0, &d));
	CHECK(strcmp(d.qname, "rfoundry-10005-2.local") == 0);	// renamed, probing again
	CHECK(parse(1, &d) && strcmp(d.qname, "N0CALL DM-1701 (10005) (2)._rfoundry-debug._udp.local") == 0);
	mdns_stats_t st;
	mdnsStats(&st);
	CHECK(st.conflicts == 1 && st.state == MDNS_PROBING);
	// The same record with our own data is no conflict
	tick_for(3000);
	uint8_t ours[4] = { (uint8_t)(RADIO_IP >> 24), (uint8_t)(RADIO_IP >> 16), (uint8_t)(RADIO_IP >> 8), (uint8_t)RADIO_IP };
	q_begin(&q, 0);
	q_rr(&q, "rfoundry-10005-2.local", 1, 0x8001, 120, ours, 4, NULL);
	q.an = 1;
	q_send(&q, 0x8400, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	mdnsStats(&st);
	CHECK(st.conflicts == 1 && st.state == MDNS_RUNNING && strcmp(mdnsHostName(), "rfoundry-10005-2") == 0);

	// Tie-break: a simultaneous probe with a greater address wins, we probe again a second later
	mdnsInit(&io, services, 3);
	mdnsTick(now, true);
	tick_for(120);				// first probe out
	uint8_t greater[4] = { 200, 0, 0, 1 };
	q_begin(&q, 0);
	q_question(&q, HOST, 255, 0x8001);
	q_rr(&q, HOST, 1, 1, 120, greater, 4, NULL);
	q.ns = 1;
	q_send(&q, 0, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	out_count = 0;
	tick_for(900);
	CHECK(out_count == 0);			// deferred
	tick_for(200);
	CHECK(out_count == 4);			// probing again, same name
	CHECK(parse(0, &d) && strcmp(d.qname, HOST) == 0);
	// a smaller one loses against us: probing goes on
	uint8_t smaller[4] = { 1, 0, 0, 1 };
	q_begin(&q, 0);
	q_question(&q, HOST, 255, 0x8001);
	q_rr(&q, HOST, 1, 1, 120, smaller, 4, NULL);
	q.ns = 1;
	q_send(&q, 0, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(250);
	CHECK(out_count == 8);

	// An instance: their SRV is passed over, their TXT compared with ours (it sorts first). Ours starts with a 13-byte
	// string ("board=DM-1701"): a 3-byte first string loses against it, a 32-byte one wins
	uint8_t srv[] = { 0, 0, 0, 0, 0x0D, 0x05, 3, 'x', 'y', 'z', 0 };
	uint8_t txt_small[] = { 3, 'a', 'b', 'c' };
	uint8_t txt_big[33] = { 32 };
	memset(txt_big + 1, 'z', 32);
	mdnsInit(&io, services, 3);
	mdnsTick(now, true);
	tick_for(120);
	q_begin(&q, 0);
	q_question(&q, INST "._tftp._udp.local", 255, 0x8001);
	q_rr(&q, INST "._tftp._udp.local", 33, 1, 120, srv, sizeof(srv), NULL);
	q_rr(&q, INST "._tftp._udp.local", 16, 1, 4500, txt_small, sizeof(txt_small), NULL);
	q.ns = 2;
	q_send(&q, 0, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	out_count = 0;
	tick_for(250);
	CHECK(out_count == 4);			// we won: probing goes on
	q_begin(&q, 0);
	q_question(&q, INST "._tftp._udp.local", 255, 0x8001);
	q_rr(&q, INST "._tftp._udp.local", 33, 1, 120, srv, sizeof(srv), NULL);
	q_rr(&q, INST "._tftp._udp.local", 16, 1, 4500, txt_big, sizeof(txt_big), NULL);
	q.ns = 2;
	q_send(&q, 0, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	out_count = 0;
	tick_for(900);
	CHECK(out_count == 0);			// we lost: deferred a second
	tick_for(200);
	CHECK(out_count == 4);
}

static void test_identity_and_link(void)
{
	dns_t d;
	bring_up();
	tick_for(1000);
	// A new call sign: goodbyes (TTL 0) for the old records, then probes for the new instance names
	strcpy(ident.callsign, "N1NEW");
	out_count = 0;
	tick_for(1000);
	CHECK(out_count >= 4 && parse(0, &d));
	const rr_t *a = find(&d, HOST, 1, 1);
	CHECK(a != NULL && a->ttl == 0);
	CHECK(parse(1, &d) && find(&d, INST "._rfoundry-debug._udp.local", 33, 1) != NULL &&
	      find(&d, INST "._rfoundry-debug._udp.local", 33, 1)->ttl == 0);
	tick_for(300);
	CHECK(parse(out_count - 1, &d) && strcmp(d.qname, "N1NEW DM-1701 (10005)._tftp._udp.local") == 0);
	strcpy(ident.callsign, "N0CALL");
	// Link down: quiet; up again: probes
	tick_for(3000);
	out_count = 0;
	tick_link(500, false);
	query(HOST, 1, 1, MDNS_PORT, 0);
	CHECK(out_count == 0);
	mdns_stats_t st;
	mdnsStats(&st);
	CHECK(st.state == MDNS_OFF && mdnsHostName()[0] == '\0');
	tick_for(300);
	CHECK(out_count == 4);
	// No address (no DMR ID): quiet
	ident.ip = 0;
	tick_for(1100);
	mdnsStats(&st);
	CHECK(st.state == MDNS_OFF);
	ident.ip = RADIO_IP;
	tick_for(2500);
	// The host's first message after the link came up: announced again
	bring_up();
	tick_for(3000);
	out_count = 0;
	query("other.local", 1, 1, MDNS_PORT, 0);
	tick_for(5);
	CHECK(out_count == 4);
}

static void test_malformed(void)
{
	bring_up();
	hear_host();
	mdns_stats_t st0, st;
	mdnsStats(&st0);
	out_count = 0;
	uint8_t shortmsg[6] = { 0 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, shortmsg, sizeof(shortmsg));
	// a compression pointer to itself
	uint8_t loop[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, loop, sizeof(loop));
	// a label running off the end
	uint8_t over[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 40, 'a', 'b' };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, over, sizeof(over));
	// counts claiming more records than there are
	uint8_t many[] = { 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, many, sizeof(many));
	// opcode not 0: ignored
	q_t q;
	q_begin(&q, 0);
	q_question(&q, HOST, 1, 1);
	q_send(&q, 0x2800, HOST_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	mdnsStats(&st);
	CHECK(st.bad == st0.bad + 4 && out_count == 0);
}

// The gateway's local port table, with a bare handler
static int hits;
static uint32_t hit_dst;
static uint16_t hit_port;
static void count_handler(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	hits++;
	hit_dst = dst;
	hit_port = dport;
}

static void test_gateway_ports(void)
{
	uint8_t data[4] = { 1, 2, 3, 4 };
	hits = 0;
	to_air = 0;
	CHECK(ipGatewayUdpBind(69, count_handler));
	from_host(HOST_IP, RADIO_IP, 5000, 69, data, 4);
	CHECK(hits == 1 && hit_dst == RADIO_IP && hit_port == 69 && to_air == 0);
	from_host(OTHER_IP, RADIO_IP, 5000, 69, data, 4);	// any source on the link
	CHECK(hits == 2);
	from_host(HOST_IP, MDNS_GROUP, 5000, 9999, data, 4);	// link local, unbound: dropped, never to the air
	CHECK(hits == 2 && to_air == 0);
	from_host(HOST_IP, RADIO_IP, 5000, 70, data, 4);	// unbound on the radio's address: over the air as before
	CHECK(to_air == 1);
	CHECK(ipGatewayUdpBind(69, NULL));
	from_host(HOST_IP, RADIO_IP, 5000, 69, data, 4);
	CHECK(hits == 2 && to_air == 2);
	for (int i = 0; i < IPGW_UDP_PORTS - 1; i++) {	// 5353 is bound already
		CHECK(ipGatewayUdpBind((uint16_t)(1000 + i), count_handler));
	}
	CHECK(!ipGatewayUdpBind(2000, count_handler));		// full
	for (int i = 0; i < IPGW_UDP_PORTS - 1; i++) {
		ipGatewayUdpBind((uint16_t)(1000 + i), NULL);
	}
	// A unicast send: to the host's MAC, TTL 255, a good UDP checksum
	out_count = 0;
	uint8_t *p = ipGatewayUdpPayload();
	memcpy(p, "hello", 5);
	CHECK(ipGatewayUdpSend(HOST_IP, 69, 5000, p, 5));
	CHECK(out_count == 1 && memcmp(out[0], HOST_MAC, 6) == 0 && out[0][22] == 255);
	uint8_t *ip = out[0] + 14;
	uint32_t pseudo = (RADIO_IP >> 16) + (RADIO_IP & 0xFFFF) + (HOST_IP >> 16) + (HOST_IP & 0xFFFF) + 17 + 13;
	CHECK(csum(ip + 20, 13, pseudo) == 0 && csum(ip, 20, 0) == 0);
}

// Compressed names from the network (RFC 1035 4.1.4): pointer loops over two and three names, pointers past the end,
// into the header, cut off after their first octet, the reserved label types 01 and 10, 17 pointers in a row (more
// than the 16 followed), and a forward pointer to our name that is fine. Records whose rdata runs past the end; a
// response holding our instance's SRV with rdata too short to hold a target.
static void test_names(void)
{
	bring_up();
	hear_host();
	mdns_stats_t st0, st;
	uint8_t m[300];
	q_t q;

	mdnsStats(&st0);
	out_count = 0;
	const uint8_t loop2[] = { 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0xC0, 18, 0, 1, 0, 1, 0xC0, 12, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, loop2, sizeof(loop2));
	const uint8_t loop3[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0xC0, 17, 1, 'b', 0xC0, 20, 1, 'c', 0xC0, 12, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, loop3, sizeof(loop3));
	const uint8_t past[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC1, 0x00, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, past, sizeof(past));
	const uint8_t header[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 2, 0, 1, 0, 1 };	// into the header
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, header, sizeof(header));
	const uint8_t cut[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, cut, sizeof(cut));
	const uint8_t type01[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x41, 'a', 0, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, type01, sizeof(type01));
	const uint8_t type10[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x81, 'a', 0, 0, 1, 0, 1 };
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, type10, sizeof(type10));
	// 17 pointers, each to the next, then the end of a name: refused (more than 16 followed)
	memset(m, 0, sizeof(m));
	m[5] = 1;
	for (int i = 0; i < 17; i++) {
		m[12 + 2 * i] = 0xC0;
		m[13 + 2 * i] = (uint8_t)(14 + 2 * i);
	}
	m[12 + 34] = 0;
	put16(m + 47, 1);
	put16(m + 49, 1);
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, m, 51);
	// a record whose rdlen runs past the end
	q_begin(&q, 0);
	q_question(&q, HOST, 1, 1);
	q_rr(&q, HOST, 1, 1, 120, (const uint8_t *)"\x0C\x00\x27\x14", 4, NULL);
	q.an = 1;
	q.b[q.len - 5] = 0x20;	// rdlen 0x2004
	q_send(&q, 0, HOST_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	mdnsStats(&st);
	CHECK(st.bad == st0.bad + 8 && out_count == 0);	// all but the pointer into the header (a root name there)
	CHECK(st.state == MDNS_RUNNING);

	// A forward pointer to our host name later in the message: a good question, answered
	memset(m, 0, sizeof(m));
	m[5] = 1;
	m[12] = 0xC0;
	m[13] = 18;
	put16(m + 14, 1);
	put16(m + 16, 1);
	int n = 18;
	m[n++] = 14;
	memcpy(m + n, "rfoundry-10005", 14);
	n += 14;
	m[n++] = 5;
	memcpy(m + n, "local", 5);
	n += 5;
	m[n++] = 0;
	out_count = 0;
	from_host(HOST_IP, MDNS_GROUP, MDNS_PORT, MDNS_PORT, m, n);
	tick_for(300);
	dns_t d;
	CHECK(out_count == 1 && parse(0, &d) && find(&d, HOST, 1, 1) != NULL);

	// Someone's response with our instance's SRV, 3 bytes of rdata: a conflict, never a read past the rdata
	q_begin(&q, 0);
	q_rr(&q, INST "._rfoundry-debug._udp.local", 33, 0x8001, 120, (const uint8_t *)"\0\0\0", 3, NULL);
	q.an = 1;
	q_send(&q, 0x8400, OTHER_IP, MDNS_PORT, MDNS_GROUP);
	tick_for(5);
	mdnsStats(&st);
	CHECK(st.state == MDNS_PROBING);	// renamed, probing the new name
	tick_for(3000);
}

// Mutated queries, probes and responses straight into mdnsReceive in exact-size buffers, with the timers running:
// every message the responder sends parses, and it never stops (no state it can't leave)
static void test_fuzz(void)
{
	q_t seeds[5];
	q_begin(&seeds[0], 0);
	q_question(&seeds[0], HOST, 1, 1);
	q_question(&seeds[0], "_services._dns-sd._udp.local", 12, 0x8001);
	q_begin(&seeds[1], 0);
	q_question(&seeds[1], "_rfoundry-debug._udp.local", 12, 1);
	q_rr(&seeds[1], "_rfoundry-debug._udp.local", 12, 1, 4500, NULL, 0, INST "._rfoundry-debug._udp.local");
	seeds[1].an = 1;
	q_begin(&seeds[2], 0);	// a probe for our host name with an authority record
	q_question(&seeds[2], HOST, 255, 1);
	q_rr(&seeds[2], HOST, 1, 1, 120, (const uint8_t *)"\x0C\x00\x27\x99", 4, NULL);
	seeds[2].ns = 1;
	q_begin(&seeds[3], 0);	// someone's response
	q_rr(&seeds[3], INST "._tftp._udp.local", 16, 0x8001, 4500, (const uint8_t *)"\x05x=abc", 6, NULL);
	seeds[3].an = 1;
	q_begin(&seeds[4], 0x1234);	// a legacy unicast query
	q_question(&seeds[4], INST "._tftp._udp.local", 33, 1);
	const uint16_t flags[5] = { 0, 0, 0, 0x8400, 0 };
	for (int i = 0; i < 5; i++) {
		put16(seeds[i].b + 2, flags[i]);
		put16(seeds[i].b + 4, (uint16_t)seeds[i].qd);
		put16(seeds[i].b + 6, (uint16_t)seeds[i].an);
		put16(seeds[i].b + 8, (uint16_t)seeds[i].ns);
	}

	bring_up();
	hear_host();
	fuzz_t f;
	fuzz_init(&f, 0x4D444E53u);
	long iters = fuzz_iters(20000), bad = 0, sent = 0;
	uint8_t buf[600];
	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 5);
		memcpy(buf, seeds[s].b, (size_t)seeds[s].len);
		int len = fuzz_mutate(&f, buf, seeds[s].len, (int)sizeof(buf));
		uint8_t *p = fuzz_exact(buf, len);
		out_count = 0;
		mdnsReceive(HOST_IP, (s == 4) ? 40000 : MDNS_PORT, p, len, now);
		free(p);
		uint32_t dt = (fuzz_below(&f, 16) == 0) ? 1000u : 50u;
		for (uint32_t t = 0; t < dt; t += 25) {
			now += 25;
			mdnsTick(now, true);
		}
		for (int k = 0; k < out_count && k < 16; k++) {
			dns_t d;
			if (!parse(k, &d) || out_len[k] > 42 + IPGW_UDP_PAYLOAD_MAX) {
				if (bad++ < 3) {
					printf("  fuzz #%ld: a message that doesn't parse (%d bytes)\n", i, out_len[k]);
				}
			}
		}
		sent += out_count;
	}
	tick_for(5000);
	mdns_stats_t st;
	mdnsStats(&st);
	CHECK(bad == 0 && sent > 0);
	CHECK(st.state == MDNS_RUNNING || st.state == MDNS_PROBING || st.state == MDNS_ANNOUNCING);
	printf("# mdns fuzz: %ld messages, %ld sent\n", iters, sent);
}

int main(void)
{
	uint32_t header[6] = { 0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1 };
	pcap = fopen("mdnsTest.pcap", "wb");
	fwrite(header, sizeof(header), 1, pcap);
	ipGatewayInit(RADIO_MAC, HOST_MAC);
	ipGatewayUdpBind(MDNS_PORT, handler);

	test_probe_announce();
	test_queries();
	test_conflict();
	test_identity_and_link();
	test_malformed();
	test_names();
	test_fuzz();
	test_gateway_ports();

	fclose(pcap);
	printf("# mdnsTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
