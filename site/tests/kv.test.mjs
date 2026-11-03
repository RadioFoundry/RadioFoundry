// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md) in the Codeplug Editor: static/js/codeplug/kv.js, the
// general settings' arsServer and ipGateway (records.js, model.js) and the region the editor moves to and from a radio.
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { CodeplugImage, REGIONS, EDITOR_REGIONS, SETTINGS_AREA } from "../static/js/codeplug/image.js";
import {
	KV_ADDR, KV_SIZE, KEY, ID_MAX, decodeArea, encodeArea, getValue, setValue, deleteValue, getId, idValue, putArea, putIds,
} from "../static/js/codeplug/kv.js";
import { decode, encode, empty, validate } from "../static/js/codeplug/model.js";
import { CpsSerial, writeRanges } from "../static/js/codeplug/transport.js";
import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";
import { crc32 } from "../static/js/flasher/bytes.js";

const area = (img) => img.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
const e = (key, ...bytes) => ({ key, value: Uint8Array.from(bytes) });

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
	assert.equal(getId(d.entries, KEY.arsServer), 12345);
	assert.equal(getId(d.entries, KEY.ipGateway), null);

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
	assert.equal(getId(decodeArea(img.bytes).entries, KEY.ipGateway), 0x030201);
});

test("settings area: a newer area is never written", () => {
	const img = withArea([e(KEY.arsServer, 1, 0, 0, 0)]);
	area(img)[4] = 2;
	const before = img.bytes.slice();
	assert.equal(putArea(img.bytes, [e(KEY.arsServer, 2, 0, 0, 0)]), false);
	assert.equal(putIds(img.bytes, { [KEY.arsServer]: 5 }), false);
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
	assert.equal(getId(d.entries, KEY.ipGateway), 9);
	assert.equal(putIds(img.bytes, { [KEY.ipGateway]: 3100500, [KEY.arsServer]: 0 }), true);
	// replaced in place, the later duplicate dropped, the new key appended, unknown keys byte for byte
	assert.deepEqual(decodeArea(img.bytes).entries, [
		e(0x7001, 1, 2, 3), { key: KEY.ipGateway, value: idValue(3100500) }, e(0x8000), e(0x7002, 4), e(KEY.arsServer, 0, 0, 0, 0)]);
	assert.equal(putIds(img.bytes, { [KEY.ipGateway]: null }), true);
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

test("settings area: a value that grew keeps its later fields; short and out-of-range IDs read absent", () => {
	const img = withArea([e(KEY.arsServer, 0x10, 0, 0, 0, 0xA1, 0xA2), e(KEY.ipGateway, 1, 2)]);
	const d = decodeArea(img.bytes).entries;
	assert.equal(getId(d, KEY.arsServer), 0x10);
	assert.equal(getId(d, KEY.ipGateway), null, "shorter than a u32");
	putIds(img.bytes, { [KEY.arsServer]: 0x20 });
	assert.deepEqual(getValue(decodeArea(img.bytes).entries, KEY.arsServer), Uint8Array.of(0x20, 0, 0, 0, 0xA1, 0xA2));

	assert.equal(getId([{ key: 1, value: idValue(ID_MAX) }], 1), ID_MAX);
	assert.equal(getId([{ key: 1, value: idValue(ID_MAX + 1) }], 1), null);
	assert.equal(getId([{ key: 1, value: idValue(0xFFFFFFFF) }], 1), null);
});

test("settings area: writing over an invalid area starts a new one", () => {
	const img = CodeplugImage.blank();
	area(img).fill(0x5A, 0, 64);
	const g = decode(img).general;
	assert.equal(g.arsServer, null);
	putIds(img.bytes, { [KEY.arsServer]: 42 });
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
	// unknown keys, a duplicate and a later value: a write of anything else leaves them
	const img = withArea([e(0x7001, 1, 2, 3), e(KEY.arsServer, 5, 0, 0, 0, 0xEE)]);
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
	const radio = new FakeCpsRadio({ board: "DM-1701", image: start.bytes });
	const tr = new CpsSerial(radio.stream());
	await tr.identify([{ id: "dm1701", codeplug: { base: 0, end: 0xE00000, names: ["DM-1701"] } }]);
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
