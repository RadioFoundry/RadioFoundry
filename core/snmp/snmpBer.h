// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The subset of the ASN.1 Basic Encoding Rules (ITU-T X.690) that SNMP messages use (RFC 3416, RFC 3417): one-octet
// tags, definite lengths up to 0xFFFF, INTEGER, OCTET STRING, NULL, OBJECT IDENTIFIER, SEQUENCE and the application
// and context types SNMP defines. No hardware dependencies (tests/host/snmpTest.c).
//
// Reading: a berReader_t over one TLV's contents; every call checks the remaining length and sets err on anything
// malformed (later calls then fail too). Writing: forward into a buffer, a constructed value's length patched by
// berEnd (shortest form: the contents move up when the length needs more than one octet); err on overflow.
#ifndef ODR_SNMP_BER_H
#define ODR_SNMP_BER_H

#include <stdbool.h>
#include <stdint.h>

// Universal and SNMP application tags (RFC 2578 / RFC 3416)
#define BER_INTEGER		0x02u
#define BER_OCTETS		0x04u
#define BER_NULL		0x05u
#define BER_OID			0x06u
#define BER_SEQUENCE		0x30u
#define BER_IPADDRESS		0x40u
#define BER_COUNTER32		0x41u
#define BER_GAUGE32		0x42u	// also Unsigned32
#define BER_TIMETICKS		0x43u
#define BER_OPAQUE		0x44u
#define BER_COUNTER64		0x46u
// The exceptions in a response's variable binding (RFC 3416 3)
#define BER_NO_SUCH_OBJECT	0x80u
#define BER_NO_SUCH_INSTANCE	0x81u
#define BER_END_OF_MIB_VIEW	0x82u

#define BER_OID_MAX		64	// content octets of an OID we accept (deeper names are refused as malformed)

typedef struct {
	const uint8_t *p;
	int len;
	int pos;
	bool err;
} berReader_t;

typedef struct {
	uint8_t *p;
	int cap;
	int pos;
	bool err;
} berWriter_t;

void berReaderInit(berReader_t *r, const uint8_t *p, int len);
bool berAtEnd(const berReader_t *r);
// The next TLV: its tag, contents and their length
bool berGet(berReader_t *r, uint8_t *tag, const uint8_t **val, int *len);
// The next TLV must have this tag; a reader over its contents
bool berEnter(berReader_t *r, uint8_t tag, berReader_t *inner);
// INTEGER (tag BER_INTEGER) that fits 32 bits signed
bool berGetInt(berReader_t *r, int32_t *v);
// Decoding of an integer's contents: signed (fits int32) or unsigned (fits uint32, a leading 0 octet allowed)
bool berDecodeInt(const uint8_t *p, int len, int32_t *v);
bool berDecodeUnsigned(const uint8_t *p, int len, uint32_t *v);
// OBJECT IDENTIFIER contents well formed: at least one octet, no subidentifier with a leading 0x80 octet or over 32
// bits, the last octet ends a subidentifier, at most BER_OID_MAX octets
bool berOidValid(const uint8_t *p, int len);
// The order of two OIDs given as their contents (shortest form): subidentifier by subidentifier, a prefix first
int berOidCompare(const uint8_t *a, int alen, const uint8_t *b, int blen);

void berWriterInit(berWriter_t *w, uint8_t *p, int cap);
// A constructed value: berBegin writes its tag and returns where its contents start, berEnd sets its length
int berBegin(berWriter_t *w, uint8_t tag);
void berEnd(berWriter_t *w, int mark);
void berPutInt(berWriter_t *w, uint8_t tag, int32_t v);
void berPutUnsigned(berWriter_t *w, uint8_t tag, uint32_t v);
void berPutOctets(berWriter_t *w, uint8_t tag, const void *p, int len);
void berPutNull(berWriter_t *w, uint8_t tag);

#endif
