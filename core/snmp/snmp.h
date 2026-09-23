// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// SNMPv2c message processing (RFC 3416 operations, RFC 3417 / RFC 1901 community-based messages): one request
// datagram in, one response datagram out, against a MIB given as a sorted table of scalar instances with get / test /
// commit callbacks. Get, GetNext, GetBulk and Set; SNMPv2-Trap messages for notifications. No hardware dependencies
// and no state of its own (counters live in the caller's snmpStats_t), so the host tests run it as it is
// (tests/host/snmpTest.c). docs/spec/snmp.md.
#ifndef ODR_SNMP_H
#define ODR_SNMP_H

#include <stdbool.h>
#include <stdint.h>
#include "snmp/snmpBer.h"

#define SNMP_PORT		161
#define SNMP_TRAP_PORT		162
#define SNMP_VERSION_2C		1	// msgVersion of a community-based SNMPv2 message (RFC 1901)

// PDU tags (RFC 3416 3)
#define SNMP_PDU_GET		0xA0u
#define SNMP_PDU_GETNEXT	0xA1u
#define SNMP_PDU_RESPONSE	0xA2u
#define SNMP_PDU_SET		0xA3u
#define SNMP_PDU_GETBULK	0xA5u
#define SNMP_PDU_INFORM		0xA6u
#define SNMP_PDU_TRAP2		0xA7u
#define SNMP_PDU_REPORT		0xA8u

// error-status (RFC 3416 3)
enum {
	SNMP_ERR_NONE = 0, SNMP_ERR_TOO_BIG = 1, SNMP_ERR_GEN = 5, SNMP_ERR_NO_ACCESS = 6, SNMP_ERR_WRONG_TYPE = 7,
	SNMP_ERR_WRONG_LENGTH = 8, SNMP_ERR_WRONG_ENCODING = 9, SNMP_ERR_WRONG_VALUE = 10, SNMP_ERR_NO_CREATION = 11,
	SNMP_ERR_INCONSISTENT_VALUE = 12, SNMP_ERR_NOT_WRITABLE = 17,
};

// A value: type is its BER tag (snmpBer.h). num for INTEGER (two's complement), Counter32, Gauge32 / Unsigned32 and
// TimeTicks; ptr / len for OCTET STRING, IpAddress (4 octets) and OBJECT IDENTIFIER (its contents)
typedef struct {
	uint8_t type;
	uint32_t num;
	const uint8_t *ptr;
	uint16_t len;
} snmpValue_t;

#define SNMP_OBJ_WRITE		0x01u	// read-write (SET goes to test / commit)
#define SNMP_SUB_MAX		4	// subidentifiers of an instance after its prefix

// One scalar instance (or one table cell): a prefix from the MIB's list and up to SNMP_SUB_MAX subidentifiers below
// 128 after it, the last one the instance (0 for a scalar, the row for a table). The table is sorted by OID, and an
// instance's index in it is the id the callbacks get
typedef struct {
	uint8_t prefix;
	uint8_t sub[SNMP_SUB_MAX];
	uint8_t subLen;
	uint8_t type;		// BER tag of its value
	uint8_t flags;		// SNMP_OBJ_*
} snmpObject_t;

typedef struct snmpMib snmpMib_t;
struct snmpMib {
	const snmpObject_t *objects;
	uint16_t count;
	const uint8_t *const *prefixes;	// OID contents, each led by its length
	// The value now; false when the instance has none at the moment (noSuchInstance, skipped by GetNext)
	bool (*get)(const snmpMib_t *mib, uint8_t id, snmpValue_t *v);
	// SET, phase one: SNMP_ERR_NONE, or the error the whole request fails with (wrongValue, wrongLength, ...). The type
	// is already checked. Phase two (every binding passed): commit, which must not fail
	uint8_t (*test)(const snmpMib_t *mib, uint8_t id, const snmpValue_t *v);
	void (*commit)(const snmpMib_t *mib, uint8_t id, const snmpValue_t *v);
	void *ctx;
};

#define SNMP_OID_MAX		24	// the longest instance OID (contents) a MIB may have

// The snmp group's counters (RFC 3418) and a few of our own
typedef struct {
	uint32_t inPkts, outPkts, inBadVersions, inBadCommunityNames, inBadCommunityUses, inASNParseErrs, silentDrops;
	uint32_t gets, getNexts, getBulks, sets, setsDone, outTraps;
} snmpStats_t;

// The community hash the settings store keeps instead of the text (FNV-1a, 32 bits)
uint32_t snmpCommunityHash(const uint8_t *s, int len);
#define SNMP_HASH_PUBLIC	0xCC909380u	// snmpCommunityHash("public")

typedef struct {
	const snmpMib_t *mib;
	uint32_t readHash;	// the read community's hash, 0 = "public"
	uint32_t writeHash;	// the write community's hash, 0 = none (SET refused for every community)
	bool writeAllowed;	// this transport takes SETs (the radio: USB yes, RF only with the setting)
	uint16_t maxRepetitions;	// GetBulk's max-repetitions is capped to this
	snmpStats_t *stats;
} snmpRequestCfg_t;

// One request message: the response's length in resp (at most cap octets), or 0 for none (dropped: malformed, a
// version other than 2c, an unknown community, not a request, or no room even for a tooBig answer). authFail is set
// when the community was unknown (the authenticationFailure notification)
int snmpProcess(const snmpRequestCfg_t *cfg, const uint8_t *req, int reqLen, uint8_t *resp, int cap, bool *authFail);

// A variable binding of a notification
typedef struct {
	const uint8_t *oid;
	uint8_t oidLen;
	snmpValue_t v;
} snmpVarbind_t;

// An SNMPv2-Trap message: sysUpTime.0 (uptime, hundredths of a second), snmpTrapOID.0 (trapOid contents), then the
// bindings. Its length, 0 if it doesn't fit cap
int snmpBuildTrap(uint8_t *out, int cap, const char *community, int32_t requestId, uint32_t uptime,
		  const uint8_t *trapOid, int trapOidLen, const snmpVarbind_t *vb, int n);

// The index of the instance with exactly this OID, or -1
int snmpFind(const snmpMib_t *mib, const uint8_t *oid, int len);
// Instance i's OID contents into buf (SNMP_OID_MAX octets): their length
int snmpObjectOid(const snmpMib_t *mib, int i, uint8_t *buf);

#endif
