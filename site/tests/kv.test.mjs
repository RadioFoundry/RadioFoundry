// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md) in the Codeplug Editor: static/js/codeplug/kv.js, the
// general settings' arsServer and ipGateway (records.js, model.js) and the region the editor moves to and from a radio.
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { CodeplugImage, REGIONS, EDITOR_REGIONS, SETTINGS_AREA } from "../static/js/codeplug/image.js";
import {
	KV_ADDR, KV_SIZE, KEY, ID_MAX, decodeArea, encodeArea, getValue, setValue, deleteValue, getKey, keyValue, keyValid, idValue, putArea, putKeys, areaForWrite, KEYS,
} from "../static/js/codeplug/kv.js";
import { decode, encode, empty, validate } from "../static/js/codeplug/model.js";
import { CpsSerial, writeRanges } from "../static/js/codeplug/transport.js";
import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";
import { crc32 } from "../static/js/flasher/bytes.js";

const area = (img) => img.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
const e = (key, ...bytes) => ({ key, value: Uint8Array.from(bytes) });
const [ARS, GW] = KEYS;	// the rows of the two ID keys

// An image whose area holds entries, written by kv.js
function withArea(entries) {
	const img = CodeplugImage.blank();
	area(img).set(encodeArea(entries));
	return img;
}

test("settings area: header, entries, CRC and erased bytes after the data", () => {
	const s = encodeArea([e(KEY.arsServer, 0x39, 0x30, 0, 0), e(0x7001, 0xAA)]);
	assert.equal(s.length, KV_SIZE);
	assert.deepEqual([...s.subarray(0, 8)], [0x52, 0x46, 0x4B, 0x56, 1, 16, 13, 0]);
	const data = s.subarray(16, 16 + 13);
	assert.deepEqual([...data], [1, 0, 4, 0, 0x39, 0x30, 0, 0, 0x01, 0x70, 1, 0, 0xAA]);
	const crc = crc32(data);
	assert.deepEqual([...s.subarray(8, 12)], [crc & 0xFF, (crc >>> 8) & 0xFF, (crc >>> 16) & 0xFF, crc >>> 24]);
	assert.deepEqual([...s.subarray(12, 16)], [0, 0, 0, 0]);
	assert.ok(s.subarray(29).every((x) => x === 0xFF));

	const img = withArea([e(KEY.arsServer, 0x39, 0x30, 0, 0), e(0x7001, 0xAA)]);
	const d = decodeArea(img.bytes);
	assert.equal(d.state, "valid");
	assert.deepEqual(d.entries, [e(1, 0x39, 0x30, 0, 0), e(0x7001, 0xAA)]);
	assert.equal(getKey(d.entries, ARS), 12345);
	assert.equal(getKey(d.entries, GW), null);

	// an empty valid area: a header with no data
	assert.deepEqual(decodeArea(withArea([]).bytes), { state: "valid", entries: [] });
	// a sector without room for the entries is refused
	assert.throws(() => encodeArea([{ key: 9, value: new Uint8Array(KV_SIZE - 16 - 3) }]), RangeError);
	assert.equal(encodeArea([{ key: 9, value: new Uint8Array(KV_SIZE - 16 - 4) }]).length, KV_SIZE);
});

test("settings area: erased, invalid and newer areas", () => {
	assert.deepEqual(decodeArea(CodeplugImage.blank().bytes), { state: "empty", entries: [] });
	const good = encodeArea([e(KEY.ipGateway, 1, 2, 3, 0)]);
	const state = (edit) => {
		const img = CodeplugImage.blank();
		const s = good.slice();
		edit(s);
		area(img).set(s);
		return decodeArea(img.bytes);
	};
	assert.equal(state(() => {}).state, "valid");
	assert.deepEqual(state((s) => (s[0] = 0x51)), { state: "bad", entries: [] }, "another tool's bytes");
	assert.equal(state((s) => s.fill(0)).state, "bad");
	assert.equal(state((s) => (s[16 + 4] ^= 1)).state, "bad", "CRC");
	assert.equal(state((s) => (s[5] = 12)).state, "bad", "header length under 16");
	assert.equal(state((s) => (s[6] = 0xF8, s[7] = 0x0F)).state, "bad", "data past the sector");
	// an entry longer than the data (CRC made to match, so only the framing is wrong)
	const overrun = state((s) => {
		s[16 + 2] = 5;
		const c = crc32(s.subarray(16, 24));
		s.set([c & 0xFF, (c >>> 8) & 0xFF, (c >>> 16) & 0xFF, c >>> 24], 8);
	});
	assert.equal(overrun.state, "bad");
	// another version: read as empty, never written
	const newer = state((s) => (s[4] = 2));
	assert.deepEqual(newer, { state: "newer", entries: [] });

	// a later header with more fields: the entries start at its length
	const img = CodeplugImage.blank();
	const s = good.slice();
	s.copyWithin(24, 16, 24);
	s.fill(0x77, 16, 24);
	s[5] = 24;
	area(img).set(s);
	assert.equal(getKey(decodeArea(img.bytes).entries, GW), 0x030201);
});

test("settings area: a newer area is never written", () => {
	const img = withArea([e(KEY.arsServer, 1, 0, 0, 0)]);
	area(img)[4] = 2;
	const before = img.bytes.slice();
	assert.equal(decodeArea(img.bytes).state, "newer");
	assert.equal(putArea(img.bytes, [e(KEY.arsServer, 2, 0, 0, 0)]), false);
	assert.equal(putKeys(img.bytes, { arsServer: 5 }), false);
	assert.deepEqual(img.bytes, before);

	// through the model: the value is kept out and the Problems tab says so
	const m = decode(img);
	assert.equal(m.general.arsServer, null);
	assert.deepEqual(validate(m).filter((p) => p.table === "general" && p.code === "settings_area_newer"), []);
	m.general = { ...m.general, arsServer: 3100000 };
	const p = validate(m).find((x) => x.code === "settings_area_newer");
	assert.equal(p?.level, "warning");
	assert.deepEqual(encode(m).bytes, before);
});

test("settings area: unknown keys kept in order, duplicates, removal", () => {
	const img = withArea([e(0x7001, 1, 2, 3), e(KEY.ipGateway, 9, 0, 0, 0), e(0x8000), e(KEY.ipGateway, 7, 0, 0, 0), e(0x7002, 4)]);
	const d = decodeArea(img.bytes);
	// one entry per key: the first wins
	assert.equal(getKey(d.entries, GW), 9);
	assert.equal(putKeys(img.bytes, { ipGateway: 3100500, arsServer: 0 }), true);
	// replaced in place, the later duplicate dropped, the new key appended, unknown keys byte for byte
	assert.deepEqual(decodeArea(img.bytes).entries, [
		e(0x7001, 1, 2, 3), { key: KEY.ipGateway, value: idValue(3100500) }, e(0x8000), e(0x7002, 4), e(KEY.arsServer, 0, 0, 0, 0)]);
	assert.equal(putKeys(img.bytes, { ipGateway: null }), true);
	assert.deepEqual(decodeArea(img.bytes).entries, [e(0x7001, 1, 2, 3), e(0x8000), e(0x7002, 4), e(KEY.arsServer, 0, 0, 0, 0)]);

	// the list helpers
	const list = [e(1, 1), e(2, 2), e(1, 3)];
	assert.deepEqual(getValue(list, 1), Uint8Array.of(1));
	assert.equal(getValue(list, 5), null);
	assert.deepEqual(setValue(list, 1, [9]), [e(1, 9), e(2, 2)]);
	assert.deepEqual(setValue(list, 5, [8]), [...list, e(5, 8)]);
	assert.deepEqual(deleteValue(list, 1), [e(2, 2)]);
	// writers drop duplicates
	assert.deepEqual(decodeArea(withArea(list).bytes).entries, [e(1, 1), e(2, 2)]);
});

test("settings area: an ID of any width; a value the setting can't take reads absent; a set replaces the value whole", () => {
	const img = withArea([e(KEY.arsServer, 0x10, 0, 0, 0, 0, 0, 0, 0), e(KEY.ipGateway, 1, 2)]);
	const d = decodeArea(img.bytes).entries;
	assert.equal(getKey(d, ARS), 0x10, "stored in 8 bytes: a later version's width");
	assert.equal(getKey(d, GW), 0x0201, "shorter than a u32: zero-extended");
	assert.equal(getKey([e(1, 0x40, 0x42, 0x0F)], ARS), 1000000, "a 3-byte ID");
	assert.equal(getKey([e(1)], ARS), null, "empty");
	assert.equal(getKey([e(1, 0x10, 0, 0, 0, 1)], ARS), null, "past 32 bits");
	assert.equal(getKey([e(1, 0x10, 0, 0, 0, 0, 0, 0, 0, 0)], ARS), null, "longer than a scalar");
	// a scalar is written whole, in the width this writer stores (4)
	putKeys(img.bytes, { arsServer: 0x20 });
	assert.deepEqual(getValue(decodeArea(img.bytes).entries, KEY.arsServer), Uint8Array.of(0x20, 0, 0, 0));

	assert.equal(getKey([{ key: 1, value: idValue(ID_MAX) }], ARS), ID_MAX);
	assert.equal(getKey([{ key: 1, value: idValue(ID_MAX + 1) }], ARS), null);
	assert.equal(getKey([{ key: 1, value: idValue(0xFFFFFFFF) }], ARS), null);
});

test("settings area: every scalar type follows its row (3.1)", () => {
	const uint = { name: "tries", key: 0x7010, type: "uint", size: 1, min: 1, max: 10 };
	const bool = { name: "tone", key: 0x7011, type: "bool", size: 1 };
	const en = { name: "mode", key: 0x7012, type: "enum", size: 1, min: 0, max: 2 };
	const int = { name: "trim", key: 0x7013, type: "int", size: 2, min: -1000, max: 1000 };
	const at = (row, ...bytes) => getKey([e(row.key, ...bytes)], row);
	assert.equal(at(uint, 10), 10);
	assert.equal(at(uint, 7, 0, 0, 0), 7, "a wider value that fits");
	assert.equal(at(uint, 0), null, "below the range");
	assert.equal(at(uint, 11), null, "above the range");
	assert.equal(getKey([], uint), null, "absent");
	assert.equal(at(bool, 0), false);
	assert.equal(at(bool, 0, 0, 4, 0), true, "anything but 0 is on");
	assert.equal(at(bool), null, "empty");
	assert.equal(at(en, 2), 2);
	assert.equal(at(en, 3), null, "a value this editor doesn't know");
	assert.equal(at(int, 0xFB), -5, "sign-extended from one byte");
	assert.equal(at(int, 0xFE, 0), 254);
	assert.equal(at(int, 0x18, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF), -1000);
	assert.equal(at(int, 0xE9, 0x03), null, "1001");
	assert.equal(at(int, 0, 0, 0, 0x80, 0), null, "past 32 bits");
	// what a writer stores
	assert.deepEqual(keyValue(uint, 9), Uint8Array.of(9));
	assert.deepEqual(keyValue(bool, true), Uint8Array.of(1));
	assert.deepEqual(keyValue(int, -300), Uint8Array.of(0xD4, 0xFE));
	assert.deepEqual(keyValue(ARS, 3100444), idValue(3100444));
	for (const [row, v] of [[uint, 9], [bool, false], [en, 1], [int, -300], [ARS, ID_MAX]]) {
		assert.equal(getKey([{ key: row.key, value: keyValue(row, v) }], row), v);
		assert.ok(keyValid(row, v));
	}
	assert.ok(!keyValid(uint, 0) && !keyValid(bool, 1) && !keyValid(int, 1.5) && !keyValid(ARS, ID_MAX + 1));
	// the rows the editor ships: unique names and keys in the assigned range, a size and a range each
	assert.equal(new Set(KEYS.map((k) => k.key)).size, KEYS.length);
	assert.equal(new Set(KEYS.map((k) => k.name)).size, KEYS.length);
	for (const k of KEYS) assert.ok(k.key >= 1 && k.key <= 0x7FFF && k.size >= 1 && k.size <= 8 && (k.type === "bool" || k.min <= k.max), k.name);
});

test("settings area: writing over an invalid area starts a new one", () => {
	const img = CodeplugImage.blank();
	area(img).fill(0x5A, 0, 64);
	const g = decode(img).general;
	assert.equal(g.arsServer, null);
	putKeys(img.bytes, { arsServer: 42 });
	const d = decodeArea(img.bytes);
	assert.equal(d.state, "valid");
	assert.deepEqual(d.entries, [{ key: KEY.arsServer, value: idValue(42) }]);
	assert.ok(area(img).subarray(16 + 8).every((x) => x === 0xFF));
});

test("general settings: arsServer and ipGateway, absent apart from 0", () => {
	const m = empty();
	assert.equal(m.general.arsServer, null);
	assert.equal(m.general.ipGateway, null);
	// a blank codeplug's area is erased and an untouched one stays so
	assert.ok(area(m.base).every((x) => x === 0xFF));
	assert.ok(area(encode(m)).every((x) => x === 0xFF));

	m.general = { ...m.general, arsServer: 0, ipGateway: 3100500 };
	const img = encode(m);
	assert.equal(decodeArea(img.bytes).state, "valid");
	const back = decode(img).general;
	assert.equal(back.arsServer, 0, "0 is a key that turns ARS off, not an absent key");
	assert.equal(back.ipGateway, 3100500);

	const m2 = decode(img);
	m2.general = { ...m2.general, arsServer: null };
	const g2 = decode(encode(m2)).general;
	assert.equal(g2.arsServer, null);
	assert.equal(g2.ipGateway, 3100500);
});

test("general settings: an unchanged codeplug keeps its area byte for byte", () => {
	// unknown keys and a value stored wider than the editor writes: a write of anything else leaves them
	const img = withArea([e(0x7001, 1, 2, 3), e(KEY.arsServer, 5, 0, 0, 0, 0)]);
	area(img).set([0x01, 0x70, 0, 0], 16 + 7 + 9 + 4);	// bytes after the data: outside the CRC, kept all the same
	const m = decode(img);
	m.general = { ...m.general, radioName: "N0CALL" };
	assert.deepEqual(area(encode(m)), area(img));

	// a bad area stays as it is too
	const bad = CodeplugImage.blank();
	area(bad).fill(0x5A, 0, 300);
	const mb = decode(bad);
	mb.general = { ...mb.general, dmrId: 3100123 };
	assert.deepEqual(area(encode(mb)), area(bad));
	// and a value set to what the area already holds writes nothing
	const m3 = decode(img);
	m3.general = { ...m3.general, arsServer: 5 };
	assert.deepEqual(area(encode(m3)), area(img));
});

test("general settings: validation of the area's IDs", () => {
	const m = empty();
	const codes = () => validate(m).filter((p) => p.field === "arsServer" || p.field === "ipGateway").map((p) => [p.field, p.code, p.level]);
	assert.deepEqual(codes(), []);
	m.general = { ...m.general, arsServer: 0, ipGateway: ID_MAX };
	assert.deepEqual(codes(), []);
	m.general = { ...m.general, arsServer: ID_MAX + 1, ipGateway: -1 };
	assert.deepEqual(codes(), [["arsServer", "range", "error"], ["ipGateway", "range", "error"]]);
	m.general = { ...m.general, arsServer: 1.5, ipGateway: null };
	assert.deepEqual(codes(), [["arsServer", "range", "error"]]);
	// an out-of-range value is never written
	assert.ok(area(encode(m)).every((x) => x === 0xFF));
});

test("the editor reads and writes the settings area with a radio; qdmr's ranges stay qdmr's", async () => {
	assert.deepEqual(SETTINGS_AREA, [0xE000, 0xF000]);
	assert.ok(!REGIONS.some(([a, b]) => a < 0xF000 && b > 0xE000));
	assert.ok(EDITOR_REGIONS.some(([a, b]) => a === 0xE000 && b === 0xF000));
	assert.ok(EDITOR_REGIONS.every(([a], i) => i === 0 || EDITOR_REGIONS[i - 1][1] <= a), "in address order");
	assert.ok(writeRanges().some(([a, b]) => a === 0xE000 && b === 0xF000));

	const start = withArea([e(0x7001, 1), e(KEY.ipGateway, 7, 0, 0, 0)]);
	const radio = new FakeCpsRadio({ board: "TEST-RADIO", image: start.bytes });
	const tr = new CpsSerial(radio.stream());
	await tr.identify([{ id: "test", codeplug: { base: 0, end: 0xE00000, names: ["TEST-RADIO"] } }]);
	const read = await tr.read({ iddb: false });
	assert.deepEqual(area(read), area(start));
	const m = decode(read);
	assert.equal(m.general.ipGateway, 7);
	m.general = { ...m.general, arsServer: 3100001 };
	const want = encode(m);
	const res = await tr.write(want, read);
	await tr.verify(want, res.plan);
	assert.deepEqual(radio.spi.subarray(0xE000, 0xF000), area(want));
	assert.deepEqual(decodeArea(radio.spi).entries, [e(0x7001, 1), e(KEY.ipGateway, 7, 0, 0, 0), { key: KEY.arsServer, value: idValue(3100001) }]);
});

test("settings area: a rewrite keeps the header length and the reserved bytes (2.4)", () => {
	const img = CodeplugImage.blank();
	const s = encodeArea([e(KEY.arsServer, 1, 0, 0, 0)]);
	// a later version's header: 24 bytes, a continuation named in 12-15 and a field in 16-23
	const later = new Uint8Array(KV_SIZE).fill(0xFF);
	later.set(s.subarray(0, 16));
	later[5] = 24;
	later.set([1, 2, 3, 4], 12);
	later.fill(0x5A, 16, 24);
	later.set(s.subarray(16, 16 + 8), 24);
	area(img).set(later);
	assert.equal(decodeArea(img.bytes).state, "valid");
	assert.equal(putKeys(img.bytes, { ipGateway: 7 }), true);
	const a = area(img);
	assert.equal(a[5], 24);
	assert.deepEqual([...a.subarray(12, 24)], [1, 2, 3, 4, ...new Array(8).fill(0x5A)]);
	assert.equal(getKey(decodeArea(img.bytes).entries, ARS), 1);
	assert.equal(getKey(decodeArea(img.bytes).entries, GW), 7);
	// a new area: 16 bytes, reserved 0
	const fresh = CodeplugImage.blank();
	putKeys(fresh.bytes, { arsServer: 3 });
	assert.equal(area(fresh)[5], 16);
	assert.deepEqual([...area(fresh).subarray(12, 16)], [0, 0, 0, 0]);
});

test("settings area: a write sends the file's area only when valid and the radio's isn't newer (6)", () => {
	const radio = withArea([e(KEY.arsServer, 9, 0, 0, 0)]);
	// a file without an area (another tool's): the radio's stays
	const none = CodeplugImage.blank();
	areaForWrite(none.bytes, radio.bytes);
	assert.deepEqual(area(none), area(radio));
	// a valid file area goes
	const file = withArea([e(KEY.arsServer, 5, 0, 0, 0)]);
	areaForWrite(file.bytes, radio.bytes);
	assert.equal(getKey(decodeArea(file.bytes).entries, ARS), 5);
	// over a newer area, never
	const newer = withArea([e(KEY.arsServer, 9, 0, 0, 0)]);
	area(newer)[4] = 2;
	const file2 = withArea([e(KEY.arsServer, 5, 0, 0, 0)]);
	areaForWrite(file2.bytes, newer.bytes);
	assert.deepEqual(area(file2), area(newer));
});

test("settings area: the editor sends the file's area only when the edit changed it", () => {
	// opened from a file with ARS 100; the radio's menu has since set 200
	const base = withArea([e(KEY.arsServer, 100, 0, 0, 0)]);
	const radio = withArea([e(KEY.arsServer, 200, 0, 0, 0)]);
	// a channel edit leaves the area as opened: the radio's own stays
	const same = base.clone();
	areaForWrite(same.bytes, radio.bytes, base.bytes);
	assert.equal(getKey(decodeArea(same.bytes).entries, ARS), 200);
	// an ARS edit goes
	const edited = base.clone();
	assert.ok(putKeys(edited.bytes, { arsServer: 300 }));
	areaForWrite(edited.bytes, radio.bytes, base.bytes);
	assert.equal(getKey(decodeArea(edited.bytes).entries, ARS), 300);
});

test("settings area: an edit sends only the keys it changed, onto the radio's area", () => {
	// opened with ARS 100, gateway 5 and a key the editor doesn't know
	const base = withArea([e(KEY.arsServer, 100, 0, 0, 0), e(KEY.ipGateway, 5, 0, 0, 0), e(0x7001, 1)]);
	// since then the radio's menu set the gateway, a later firmware changed its key and added one
	const radio = withArea([e(0x7002, 9, 9), e(KEY.arsServer, 100, 0, 0, 0), e(KEY.ipGateway, 6, 0, 0, 0), e(0x7001, 2)]);
	radio.bytes.subarray(KV_ADDR + 12, KV_ADDR + 16).set([0, 0xF0, 0, 0]);	// and named a continuation
	const edited = base.clone();
	assert.ok(putKeys(edited.bytes, { arsServer: 300 }));
	areaForWrite(edited.bytes, radio.bytes, base.bytes);
	// only the ARS server is the edit's: the rest is the radio's, in the radio's order, its header kept
	assert.deepEqual(decodeArea(edited.bytes).entries, [
		e(0x7002, 9, 9), { key: KEY.arsServer, value: idValue(300) }, e(KEY.ipGateway, 6, 0, 0, 0), e(0x7001, 2)]);
	assert.deepEqual([...area(edited).subarray(12, 16)], [0, 0xF0, 0, 0]);
	// a key the edit removed is removed from the radio's; one it added is added
	const cleared = base.clone();
	assert.ok(putKeys(cleared.bytes, { ipGateway: null }));
	areaForWrite(cleared.bytes, radio.bytes, base.bytes);
	assert.deepEqual(decodeArea(cleared.bytes).entries, [e(0x7002, 9, 9), e(KEY.arsServer, 100, 0, 0, 0), e(0x7001, 2)]);
	const none = CodeplugImage.blank();	// opened without an area
	const added = none.clone();
	assert.ok(putKeys(added.bytes, { ipGateway: 8 }));
	areaForWrite(added.bytes, radio.bytes, none.bytes);
	assert.deepEqual(decodeArea(added.bytes).entries, [
		e(0x7002, 9, 9), e(KEY.arsServer, 100, 0, 0, 0), { key: KEY.ipGateway, value: idValue(8) }, e(0x7001, 2)]);
	// a radio without a valid area takes the file's as edited
	const blank = CodeplugImage.blank();
	const whole = base.clone();
	assert.ok(putKeys(whole.bytes, { arsServer: 300 }));
	const sent = whole.clone();
	areaForWrite(sent.bytes, blank.bytes, base.bytes);
	assert.deepEqual(area(sent), area(whole));
	// the radio's keys and the edit's don't fit together: refused, not cut
	const full = withArea([e(0x7003, ...new Uint8Array(KV_SIZE - 16 - 4 - 8))]);
	assert.throws(() => areaForWrite(added.bytes, full.bytes, none.bytes), RangeError);
});
