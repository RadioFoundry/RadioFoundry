// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key check values in the Codeplug Editor (static/js/codeplug/kcv.js, docs/spec/key-check.md) against the shared vectors
// the C host test reads too (tests/vectors/key-check.txt). Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { kcvPrivacy, kcvHashed, kcvAes, kcvTrafficRecord, KCV_CLASS } from "../static/js/codeplug/kcv.js";
import { nxdnSystem } from "../static/js/codeplug/mode_nxdn.js";

const VECTORS = new URL("../../tests/vectors/key-check.txt", import.meta.url);
const bytes = (s) => Uint8Array.from(s.match(/../g) ?? [], (x) => parseInt(x, 16));
const vectors = (kind) => readFileSync(VECTORS, "utf8").split(/\r?\n/).filter((l) => l.startsWith(kind + " ")).map((l) => l.split(" ").slice(1));

test("vectors: privacy key slots", async () => {
	const v = vectors("privacy");
	assert.ok(v.length >= 3);
	for (const [type, key, want] of v) assert.equal(await kcvPrivacy(parseInt(type, 16), parseInt(key, 16)), want);
});

test("vectors: traffic keys, hashed", async () => {
	const v = vectors("traffic");
	assert.ok(v.length >= 1);
	for (const [sys, alg, key, want] of v) assert.equal(await kcvHashed(KCV_CLASS.traffic, parseInt(sys, 16), parseInt(alg, 16), bytes(key)), want);
});

test("vectors: AES keys", async () => {
	const v = vectors("aes");
	assert.ok(v.length >= 1);
	for (const [key, want] of v) assert.equal(await kcvAes(bytes(key)), want);
	assert.equal(await kcvAes(new Uint8Array(16)), null);	// only AES-256 keys
});

test("an NXDN key record as the editor encodes it", async () => {
	// seed 0x1234 under key ID 5 of system 0: the vector's key octets, so the vector's value whatever the ID
	const rec = nxdnSystem.traffic.encode(0, { id: 5, seed: 0x1234 });
	assert.equal(await kcvTrafficRecord(nxdnSystem.id, rec), "51588D");
	const other = nxdnSystem.traffic.encode(3, { id: 9, seed: 0x1234 });
	assert.equal(await kcvTrafficRecord(nxdnSystem.id, other), "51588D");
	assert.notEqual(await kcvTrafficRecord(nxdnSystem.id, nxdnSystem.traffic.encode(0, { id: 5, seed: 0x1235 })), "51588D");
});
