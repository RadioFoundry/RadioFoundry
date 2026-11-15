// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
import { test } from "node:test";
import assert from "node:assert/strict";
import { empty, newChannel, encode, decode, validate } from "../static/js/codeplug/model.js";
import { decodeArea, putArea, areaForWrite, KV_ADDR } from "../static/js/codeplug/kv.js";
import { AES_KEYS_KEY, CHANNEL_PRIVACY_KEY, decodePrivacy } from "../static/js/codeplug/privacy.js";
import { exportQdmr, importQdmr, QdmrError } from "../static/js/codeplug/qdmr.js";
import { History } from "../static/js/editor/undo.js";

const key = Array.from({ length: 32 }, (_, i) => i.toString(16).padStart(2, "0")).join("");
const model = () => {
	const m = empty();
	m.general.aesKeys = [{ id: 7, key }, { id: 239, key: "00".repeat(32) }];
	m.channels.push(newChannel({ name: "Encrypted", mode: "dmr", privacyMode: 2, privacyId: 7 }));
	return m;
};

test("AES keys and per-channel modes round-trip in the binary codeplug", () => {
	const m = model();
	m.channels.push(newChannel({ name: "Clear", mode: "dmr", privacyMode: 1 }), newChannel({ name: "Inherit", mode: "dmr" }));
	const image = encode(m), d = decode(image);
	assert.deepEqual(d.general.aesKeys, m.general.aesKeys);
	assert.deepEqual(d.channels.map((c) => [c.privacyMode, c.privacyId]), [[2, 7], [1, 0], [0, 0]]);
	assert.deepEqual(encode(d).bytes, image.bytes);
	const area = decodeArea(image.bytes);
	const ring = area.entries.find((e) => e.key === AES_KEYS_KEY).value;
	assert.equal(ring.length, 68);
	assert.deepEqual([...ring.subarray(0, 4)], [1, 3, 7, 0]);
	assert.equal(ring[34], 31); assert.equal(ring[35], 239);
	assert.equal(area.entries.find((e) => e.key === CHANNEL_PRIVACY_KEY).value.length, 2049);
});

test("all 1024 channel selections fit in the existing settings sector", () => {
	const m = model(); m.channels = [];
	for (let n = 1; n <= 1024; n++) m.channels.push(newChannel({ number: n, name: `Channel ${n}`, mode: "dmr", privacyMode: 2, privacyId: 239 }));
	const d = decode(encode(m));
	assert.equal(d.channels.length, 1024);
	assert.ok(d.channels.every((c) => c.privacyMode === 2 && c.privacyId === 239));
});

test("missing, duplicate, malformed keys and unsupported modes block saving", () => {
	const m = model();
	const errors = () => validate(m).filter((p) => p.level === "error").map((p) => p.code);
	assert.deepEqual(errors(), []);
	m.channels[0].privacyId = 99;
	assert.ok(errors().includes("aes_missing"));
	m.channels[0].privacyId = 7;
	m.general.aesKeys[1].id = 7;
	assert.ok(errors().includes("aes_duplicate"));
	assert.throws(() => encode(m), RangeError);
	m.general.aesKeys[1] = null;
	m.general.aesKeys[0].key = "invalid";
	assert.ok(errors().includes("aes_key"));
	assert.throws(() => encode(m), RangeError);
	m.general.aesKeys[0].key = key;
	m.channels[0].privacyMode = 3;
	assert.ok(errors().includes("aes_mode"));
	assert.throws(() => encode(m), RangeError);
});

test("explicit empty ring removes keys without reviving local provisioning", () => {
	const d = decode(encode(model()));
	d.general.aesKeys = [null, null]; d.channels[0].privacyMode = 1;
	const image = encode(d), area = decodeArea(image.bytes);
	assert.equal(area.entries.find((e) => e.key === AES_KEYS_KEY).value[1], 0);
	assert.deepEqual(decode(image).general.aesKeys, [null, null]);
});

test("unknown records and appended fields survive encryption edits", () => {
	const image = encode(model());
	const entries = decodeArea(image.bytes).entries.map((e) => ({ key: e.key, value: new Uint8Array([...e.value, 0xA5, 0x5A]) }));
	entries.push({ key: 0x1234, value: Uint8Array.of(9, 8, 7) });
	putArea(image.bytes, entries);
	const m = decode(image);
	m.general.aesKeys[0].id = 8; m.channels[0].privacyId = 8;
	const area = decodeArea(encode(m).bytes);
	for (const id of [AES_KEYS_KEY, CHANNEL_PRIVACY_KEY]) assert.deepEqual([...area.entries.find((e) => e.key === id).value.slice(-2)], [0xA5, 0x5A]);
	assert.deepEqual([...area.entries.find((e) => e.key === 0x1234).value], [9, 8, 7]);
});

test("AES keys and channel requirements participate in undo and redo", () => {
	const m = model(), history = new History();
	history.record(m, "key and channel");
	m.general.aesKeys[0].id = 10; m.channels[0].privacyId = 10;
	history.undo(m);
	assert.equal(m.general.aesKeys[0].id, 7); assert.equal(m.channels[0].privacyId, 7);
	history.redo(m);
	assert.equal(m.general.aesKeys[0].id, 10); assert.equal(m.channels[0].privacyId, 10);
});

test("YAML refuses encryption it cannot preserve", () => {
	assert.throws(() => exportQdmr(model()), (e) => e instanceof QdmrError && e.code === "encryption");
	const m = model(), before = JSON.stringify(m.general.aesKeys);
	assert.throws(() => importQdmr(m, "channels: []\ncommercial:\n  encryptionKeys: [unknown]\n"), (e) => e instanceof QdmrError && e.code === "encryption");
	assert.equal(JSON.stringify(m.general.aesKeys), before);
});

test("corrupted RFKV metadata never decodes as clear channel policy", () => {
	const image = encode(model());
	let seed = 0x12345678;
	for (let n = 0; n < 1000; n++) {
		seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
		const bytes = image.bytes.slice();
		const at = KV_ADDR + 4 + seed % 2200;
		bytes[at] ^= 1 << (seed % 8);
		const area = decodeArea(bytes), privacy = decodePrivacy(bytes);
		if (area.state === "bad" || area.state === "newer") assert.equal(privacy.channel(1).privacyMode, 3);
	}
});


test("radio writes merge AES edits while preserving unrelated radio keys", () => {
	const base = encode(model()), edited = decode(base), radio = encode(decode(base));
	const entries = decodeArea(radio.bytes).entries;
	entries.push({ key: 1, value: Uint8Array.of(9, 0, 0, 0) });
	putArea(radio.bytes, entries);
	edited.general.aesKeys[0].id = 8; edited.channels[0].privacyId = 8;
	const file = encode(edited);
	areaForWrite(file.bytes, radio.bytes, base.bytes);
	const got = decode(file);
	assert.equal(got.general.arsServer, 9);
	assert.equal(got.general.aesKeys[0].id, 8);
	assert.equal(got.channels[0].privacyId, 8);
});
