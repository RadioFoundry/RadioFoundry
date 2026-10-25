// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SNMPv2c message processing (see snmp.h). Written from RFC 3416 4.2 (the PDU processing rules), RFC 3417 (the
// message over UDP), RFC 1901 (the community-based message) and RFC 3418 (the snmp group's counters).
#include <string.h>
#include "snmp/snmp.h"

#define BULK_REPEATERS_MAX	16	// GetBulk bindings repeated with state kept; more get one repetition

// sysUpTime.0 and snmpTrapOID.0 (RFC 3418)
static const uint8_t OID_SYS_UPTIME[] = { 0x2B, 6, 1, 2, 1, 1, 3, 0 };
static const uint8_t OID_TRAP_OID[] = { 0x2B, 6, 1, 6, 3, 1, 1, 4, 1, 0 };

uint32_t snmpCommunityHash(const uint8_t *s, int len)
{
	uint32_t h = 2166136261u;

	for (int i = 0; i < len; i++) {
		h = (h ^ s[i]) * 16777619u;
	}
	return h;
}

int snmpObjectOid(const snmpMib_t *mib, int i, uint8_t *buf)
{
	const snmpObject_t *o = &mib->objects[i];
	const uint8_t *p = mib->prefixes[o->prefix];

	memcpy(buf, p + 1, p[0]);
	memcpy(buf + p[0], o->sub, o->subLen);
	return p[0] + o->subLen;
}

int snmpFind(const snmpMib_t *mib, const uint8_t *oid, int len)
{
	uint8_t buf[SNMP_OID_MAX];

	for (int i = 0; i < mib->count; i++) {
		int n = snmpObjectOid(mib, i, buf);

		if ((n == len) && (memcmp(buf, oid, (size_t)len) == 0)) {
			return i;
		}
	}
	return -1;
}

// The first instance after oid, or count
static int findNext(const snmpMib_t *mib, const uint8_t *oid, int len)
{
	uint8_t buf[SNMP_OID_MAX];
	int i = 0;

	while ((i < mib->count) && (berOidCompare(buf, snmpObjectOid(mib, i, buf), oid, len) <= 0)) {
		i++;
	}
	return i;
}

static void putValue(berWriter_t *w, const snmpValue_t *v)
{
	switch (v->type) {
	case BER_INTEGER:
		berPutInt(w, BER_INTEGER, (int32_t)v->num);
		break;
	case BER_COUNTER32:
	case BER_GAUGE32:
	case BER_TIMETICKS:
		berPutUnsigned(w, v->type, v->num);
		break;
	case BER_OCTETS:
	case BER_OID:
	case BER_IPADDRESS:
	case BER_OPAQUE:
		berPutOctets(w, v->type, v->ptr, v->len);
		break;
	default:
		berPutNull(w, v->type);	// NULL and the exceptions
		break;
	}
}

static void putVarbind(berWriter_t *w, const uint8_t *oid, int len, const snmpValue_t *v)
{
	int m = berBegin(w, BER_SEQUENCE);

	berPutOctets(w, BER_OID, oid, len);
	putValue(w, v);
	berEnd(w, m);
}

// The instance i's binding, if it has a value now
static bool putInstance(berWriter_t *w, const snmpMib_t *mib, int i)
{
	uint8_t buf[SNMP_OID_MAX];
	snmpValue_t v = { .type = mib->objects[i].type };

	if (!mib->get(mib, (uint8_t)i, &v)) {
		return false;
	}
	v.type = mib->objects[i].type;
	putVarbind(w, buf, snmpObjectOid(mib, i, buf), &v);
	return true;
}

static void putException(berWriter_t *w, const uint8_t *oid, int len, uint8_t tag)
{
	snmpValue_t v = { .type = tag };

	putVarbind(w, oid, len, &v);
}

// GetNext from instance index i on (the first with a value now); false at the end of the MIB
static int nextWithValue(berWriter_t *w, const snmpMib_t *mib, int i)
{
	for (; i < mib->count; i++) {
		if (putInstance(w, mib, i)) {
			return i;
		}
	}
	return -1;
}

// A binding of the request: its name (validated) and the value's TLV
typedef struct {
	const uint8_t *oid;
	int oidLen;
	uint8_t tag;
	const uint8_t *val;
	int valLen;
} binding_t;

static bool nextBinding(berReader_t *list, binding_t *b)
{
	berReader_t vb;
	uint8_t t;

	if (!berEnter(list, BER_SEQUENCE, &vb) || !berGet(&vb, &t, &b->oid, &b->oidLen) || (t != BER_OID) ||
	    !berOidValid(b->oid, b->oidLen) || !berGet(&vb, &b->tag, &b->val, &b->valLen) || !berAtEnd(&vb)) {
		list->err = true;
		return false;
	}
	return true;
}

// The value of a SET binding; false when its encoding is wrong for its tag
static bool decodeValue(const binding_t *b, snmpValue_t *v)
{
	int32_t s;

	memset(v, 0, sizeof(*v));
	v->type = b->tag;
	v->ptr = b->val;
	v->len = (uint16_t)b->valLen;
	switch (b->tag) {
	case BER_INTEGER:
		if (!berDecodeInt(b->val, b->valLen, &s)) {
			return false;
		}
		v->num = (uint32_t)s;
		return true;
	case BER_COUNTER32:
	case BER_GAUGE32:
	case BER_TIMETICKS:
		return berDecodeUnsigned(b->val, b->valLen, &v->num);
	case BER_IPADDRESS:
		return b->valLen == 4;
	case BER_OID:
		return berOidValid(b->val, b->valLen);
	default:
		return true;
	}
}

typedef struct {
	const snmpRequestCfg_t *cfg;
	berWriter_t w;
	const uint8_t *community;
	int communityLen;
	int32_t requestId;
	int outer, pdu, list;	// marks of the message, the response PDU and its binding list
	int cap;		// the whole buffer (the writer stops LENGTH_GROWTH short until the lengths are set)
} response_t;

#define LENGTH_GROWTH		6	// the message, PDU and list lengths may each need two more octets

static void beginResponse(response_t *r, uint8_t *resp, int cap, uint8_t pdu, uint8_t status, int index)
{
	r->cap = cap;
	berWriterInit(&r->w, resp, cap - LENGTH_GROWTH);
	r->outer = berBegin(&r->w, BER_SEQUENCE);
	berPutInt(&r->w, BER_INTEGER, SNMP_VERSION_2C);
	berPutOctets(&r->w, BER_OCTETS, r->community, r->communityLen);
	r->pdu = berBegin(&r->w, pdu);
	berPutInt(&r->w, BER_INTEGER, r->requestId);
	berPutInt(&r->w, BER_INTEGER, status);
	berPutInt(&r->w, BER_INTEGER, index);
	r->list = berBegin(&r->w, BER_SEQUENCE);
}

static int endResponse(response_t *r, int outer)
{
	r->w.cap = r->cap;
	berEnd(&r->w, r->list);
	berEnd(&r->w, r->pdu);
	berEnd(&r->w, outer);
	if (r->w.err) {
		return 0;
	}
	if (r->cfg != NULL) {
		r->cfg->stats->outPkts++;
	}
	return r->w.pos;
}

// An error answer: the status, the index and the request's bindings as they came (RFC 3416 4.2.5); tooBig has none
static int errorResponse(response_t *r, uint8_t *resp, int cap, uint8_t status, int index, const uint8_t *list,
			 int listLen)
{
	beginResponse(r, resp, cap, SNMP_PDU_RESPONSE, status, index);
	if ((status != SNMP_ERR_TOO_BIG) && (list != NULL)) {
		if ((r->w.pos + listLen) > r->w.cap) {
			r->w.err = true;
		} else {
			memcpy(r->w.p + r->w.pos, list, (size_t)listLen);
			r->w.pos += listLen;
		}
	}
	int n = endResponse(r, r->outer);

	if (n == 0) {
		r->cfg->stats->silentDrops++;
	}
	return n;
}

static int doSet(response_t *r, uint8_t *resp, int cap, bool rw, berReader_t *list)
{
	const snmpMib_t *mib = r->cfg->mib;
	berReader_t pass = *list;
	binding_t b;
	snmpValue_t v;
	int index = 0, failed = 0;
	uint8_t status = SNMP_ERR_NONE;

	r->cfg->stats->sets++;
	// Phase one: every binding must be acceptable before anything changes; the first failure answers
	while (!berAtEnd(&pass)) {
		if (!nextBinding(&pass, &b)) {
			r->cfg->stats->inASNParseErrs++;
			return 0;
		}
		index++;
		if (status != SNMP_ERR_NONE) {
			continue;	// the rest only parsed
		}
		int i = snmpFind(mib, b.oid, b.oidLen);

		if (!rw || !r->cfg->writeAllowed) {
			status = SNMP_ERR_NO_ACCESS;
		} else if (i < 0) {
			status = SNMP_ERR_NO_CREATION;	// nothing here can be created
		} else if (!(mib->objects[i].flags & SNMP_OBJ_WRITE)) {
			status = SNMP_ERR_NOT_WRITABLE;
		} else if (b.tag != mib->objects[i].type) {
			status = SNMP_ERR_WRONG_TYPE;
		} else if (!decodeValue(&b, &v)) {
			status = SNMP_ERR_WRONG_ENCODING;
		} else {
			status = mib->test(mib, (uint8_t)i, &v);
		}
		failed = index;
	}
	if (status != SNMP_ERR_NONE) {
		if (!rw) {
			r->cfg->stats->inBadCommunityUses++;	// a SET with the read community
		}
		return errorResponse(r, resp, cap, status, failed, list->p, list->len);
	}
	// Phase two
	pass = *list;
	while (!berAtEnd(&pass) && nextBinding(&pass, &b)) {
		decodeValue(&b, &v);
		mib->commit(mib, (uint8_t)snmpFind(mib, b.oid, b.oidLen), &v);
	}
	r->cfg->stats->setsDone++;
	return errorResponse(r, resp, cap, SNMP_ERR_NONE, 0, list->p, list->len);
}

// GetBulk (RFC 3416 4.2.3): the non-repeaters once, then the repeaters round by round until max-repetitions, the end
// of the MIB in every column, or the room runs out (fewer repetitions are allowed)
static int doBulk(response_t *r, uint8_t *resp, int cap, berReader_t *list, int32_t nonRep, int32_t maxRep)
{
	const snmpMib_t *mib = r->cfg->mib;
	binding_t b;
	int16_t at[BULK_REPEATERS_MAX];		// per column: where the next GetNext starts from, mib->count at the end
	int16_t prev[BULK_REPEATERS_MAX];	// ... the instance last returned, -1 none (endOfMibView is named so)
	uint8_t buf[SNMP_OID_MAX];

	if (nonRep < 0) {
		nonRep = 0;
	}
	if (maxRep > r->cfg->maxRepetitions) {
		maxRep = r->cfg->maxRepetitions;
	}
	for (int i = 0; (i < nonRep) && !berAtEnd(list); i++) {
		if (!nextBinding(list, &b)) {
			r->cfg->stats->inASNParseErrs++;
			return 0;
		}
		if (nextWithValue(&r->w, mib, findNext(mib, b.oid, b.oidLen)) < 0) {
			putException(&r->w, b.oid, b.oidLen, BER_END_OF_MIB_VIEW);
		}
	}
	if (r->w.err) {
		return errorResponse(r, resp, cap, SNMP_ERR_TOO_BIG, 0, NULL, 0);
	}
	int columns = 0;

	for (berReader_t c = *list; !berAtEnd(&c); columns++) {
		if (!nextBinding(&c, &b)) {
			r->cfg->stats->inASNParseErrs++;
			return 0;
		}
	}
	if (columns > BULK_REPEATERS_MAX) {
		maxRep = (maxRep > 0) ? 1 : 0;	// no state for that many columns: one repetition
	}
	int last = r->w.pos;

	for (int round = 0; round < maxRep; round++) {
		berReader_t l = *list;
		bool any = false;

		for (int k = 0; k < columns; k++) {
			nextBinding(&l, &b);
			// (more than BULK_REPEATERS_MAX columns only get round 0, which needs no state)
			int start = (round == 0) ? findNext(mib, b.oid, b.oidLen) : at[k];
			int before = (round == 0) ? -1 : prev[k];
			int got = (start < mib->count) ? nextWithValue(&r->w, mib, start) : -1;

			if (got < 0) {
				// The end: named as the binding this one follows (the request's, or the column's last)
				if (before < 0) {
					putException(&r->w, b.oid, b.oidLen, BER_END_OF_MIB_VIEW);
				} else {
					putException(&r->w, buf, snmpObjectOid(mib, before, buf), BER_END_OF_MIB_VIEW);
				}
				start = mib->count;
			} else {
				any = true;
				start = got + 1;
				before = got;
			}
			if (k < BULK_REPEATERS_MAX) {
				at[k] = (int16_t)start;
				prev[k] = (int16_t)before;
			}
			if (r->w.err) {
				r->w.err = false;
				r->w.pos = last;	// up to the last whole binding
				return endResponse(r, r->outer);
			}
			last = r->w.pos;
		}
		if (!any) {
			break;
		}
	}
	return endResponse(r, r->outer);
}

int snmpProcess(const snmpRequestCfg_t *cfg, const uint8_t *req, int reqLen, uint8_t *resp, int cap, bool *authFail)
{
	snmpStats_t *st = cfg->stats;
	const snmpMib_t *mib = cfg->mib;
	berReader_t msg, top, pdu, list;
	int32_t version, requestId, f2, f3;
	uint8_t tag;
	const uint8_t *val;
	int len;
	response_t r = { .cfg = cfg };

	*authFail = false;
	st->inPkts++;
	berReaderInit(&top, req, reqLen);
	if (!berEnter(&top, BER_SEQUENCE, &msg) || !berGetInt(&msg, &version)) {
		st->inASNParseErrs++;
		return 0;
	}
	if (version != SNMP_VERSION_2C) {
		st->inBadVersions++;
		return 0;
	}
	if (!berGet(&msg, &tag, &r.community, &r.communityLen) || (tag != BER_OCTETS) || !berGet(&msg, &tag, &val, &len) ||
	    !berAtEnd(&msg)) {
		st->inASNParseErrs++;
		return 0;
	}
	berReaderInit(&pdu, val, len);
	if (!berGetInt(&pdu, &requestId) || !berGetInt(&pdu, &f2) || !berGetInt(&pdu, &f3) ||
	    !berEnter(&pdu, BER_SEQUENCE, &list) || !berAtEnd(&pdu)) {
		st->inASNParseErrs++;
		return 0;
	}
	r.requestId = requestId;

	// The community decides the access: the write community reads and writes, the read community reads
	uint32_t h = snmpCommunityHash(r.community, r.communityLen);
	uint32_t readHash = (cfg->readHash != 0) ? cfg->readHash : SNMP_HASH_PUBLIC;
	bool rw = (cfg->writeHash != 0) && (h == cfg->writeHash);

	if (!rw && (h != readHash)) {
		st->inBadCommunityNames++;
		*authFail = true;
		return 0;
	}

	binding_t b;

	switch (tag) {
	case SNMP_PDU_SET:
		return doSet(&r, resp, cap, rw, &list);
	case SNMP_PDU_GET:
		st->gets++;
		break;
	case SNMP_PDU_GETNEXT:
		st->getNexts++;
		break;
	case SNMP_PDU_GETBULK:
		st->getBulks++;
		break;
	default:
		return 0;	// a response, notification or report sent to us: not a request (RFC 3416 4.2)
	}

	beginResponse(&r, resp, cap, SNMP_PDU_RESPONSE, SNMP_ERR_NONE, 0);
	if (tag == SNMP_PDU_GETBULK) {
		return doBulk(&r, resp, cap, &list, f2, f3);
	}
	while (!berAtEnd(&list)) {
		if (!nextBinding(&list, &b)) {
			st->inASNParseErrs++;
			return 0;
		}
		if (tag == SNMP_PDU_GET) {
			int i = snmpFind(mib, b.oid, b.oidLen);

			if ((i < 0) || !putInstance(&r.w, mib, i)) {
				putException(&r.w, b.oid, b.oidLen,
					     (i >= 0) ? BER_NO_SUCH_INSTANCE : BER_NO_SUCH_OBJECT);
			}
		} else if (nextWithValue(&r.w, mib, findNext(mib, b.oid, b.oidLen)) < 0) {
			putException(&r.w, b.oid, b.oidLen, BER_END_OF_MIB_VIEW);
		}
		if (r.w.err) {
			// The rest still has to parse before the answer is tooBig
			while (!berAtEnd(&list) && nextBinding(&list, &b)) {
			}
			if (list.err) {
				st->inASNParseErrs++;
				return 0;
			}
			return errorResponse(&r, resp, cap, SNMP_ERR_TOO_BIG, 0, NULL, 0);
		}
	}
	int n = endResponse(&r, r.outer);

	return (n != 0) ? n : errorResponse(&r, resp, cap, SNMP_ERR_TOO_BIG, 0, NULL, 0);
}

int snmpBuildTrap(uint8_t *out, int cap, const char *community, int32_t requestId, uint32_t uptime,
		  const uint8_t *trapOid, int trapOidLen, const snmpVarbind_t *vb, int n)
{
	response_t r = { .community = (const uint8_t *)community, .communityLen = (int)strlen(community),
			 .requestId = requestId };
	snmpValue_t v = { .type = BER_TIMETICKS, .num = uptime };

	beginResponse(&r, out, cap, SNMP_PDU_TRAP2, 0, 0);
	putVarbind(&r.w, OID_SYS_UPTIME, sizeof(OID_SYS_UPTIME), &v);
	v.type = BER_OID;
	v.ptr = trapOid;
	v.len = (uint16_t)trapOidLen;
	putVarbind(&r.w, OID_TRAP_OID, sizeof(OID_TRAP_OID), &v);
	for (int i = 0; i < n; i++) {
		putVarbind(&r.w, vb[i].oid, vb[i].oidLen, &vb[i].v);
	}
	return endResponse(&r, r.outer);
}
