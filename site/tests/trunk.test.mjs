// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III trunked systems in the Codeplug Editor (docs/spec/dmr-trunking.md 12): the settings area's system,
// channel, key and tag records (static/js/codeplug/trunk.js), the model's references to them (model.js), the
// Trunked Systems dialog's parser (editor/trunkview.js) and the channel table's DMR Trunked mode (editor/views.js).
// The records here are packed by hand from the spec's tables, as tools/rfoundry_debug.py writes them.
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { CodeplugImage } from "../static/js/codeplug/image.js";
import { KV_ADDR, KV_SIZE, decodeArea, getValue, putArea } from "../static/js/codeplug/kv.js";
import { decode, encode, empty, validate, remove, newChannel, setTrunk, putSystem, systemUsable } from "../static/js/codeplug/model.js";
import { nameCheck, decodeTag, decodeSystem, decodeKey, encodeSystem, newSystem, T3_KEY } from "../static/js/codeplug/trunk.js";
import { parseSystem } from "../static/js/editor/trunkview.js";
import { columns, channelForm } from "../static/js/editor/views.js";
import { History } from "../static/js/editor/undo.js";
import { importQdmr } from "../static/js/codeplug/qdmr.js";

const t = (k, p) => (p ? `${k} ${JSON.stringify(p)}` : k);

// --- Records packed from the spec's tables (12.1-12.4) ---

function pack(spec) {
	const size = spec.reduce((n, [w]) => n + w, 0);
	const v = new Uint8Array(size), d = new DataView(v.buffer);
	let o = 0;
	for (const [w, x] of spec) {
		if (x instanceof Uint8Array) v.set(x.subarray(0, w), o);
		else if (w === 1) d.setUint8(o, x & 0xFF);
		else if (w === 2) d.setUint16(o, x, true);
		else d.setUint32(o, x >>> 0, true);
		o += w;
	}
	return v;
}
const nameBytes = (s) => Uint8Array.from({ length: 16 }, (_, i) => (i < s.length ? s.charCodeAt(i) : 0));

function systemRecord({ name, model = 0, net = 0, cat = 1, cc = 1, opts = 0x20, ownId = 0, sharing = 0, hold = 0, fb = 0, fbName = "",
	base = 0, first = 0, sep = 0, planFlags = 0, split = 0, leave = 0, ps = 0, sites = [] }) {
	const s = [...sites, 0, 0, 0, 0, 0, 0, 0, 0].slice(0, 8);
	return pack([[16, nameBytes(name)], [1, model], [2, net], [1, cat], [1, cc], [2, opts], [4, ownId], [1, sharing], [1, hold],
		[2, fb], [4, base], [2, first], [1, sep], [1, planFlags], [4, split], [1, leave], [1, ps], [1, sites.length],
		...s.map((x) => [2, x]), [2, fb ? nameCheck(fbName) : 0]]);
}
const chanRecord = (sys, ctrl, lcn, rx, tx, cc = 0xFF) => pack([[1, sys], [1, ctrl ? 1 : 0], [2, lcn], [4, rx], [4, tx], [1, cc]]);
const tagRecord = (sys, name, noFb = false, fb = 0, fbName = "") => pack([[1, sys], [1, noFb ? 1 : 0], [2, nameCheck(name)], [2, fb], [2, fb ? nameCheck(fbName) : 0]]);
const KEY0 = Uint8Array.from({ length: 16 }, (_, i) => 0x10 + i);
const KEY1 = Uint8Array.from({ length: 19 }, (_, i) => 0xA0 + i);	// with a PSN: A0..AF, then B0 B1 B2
const UNKNOWN = { key: 0x7001, value: Uint8Array.of(1, 2, 3, 4, 5) };

// A codeplug with channels 1 County (trunked on system 0, its own fallback channel 3), 2 Metro DMR (trunked on system
// 1, no fallback), 3 Analog (FM), 4 Stale (a tag whose check is another name's), 5 Spare; systems 0 (authenticates,
// a key without PSN, fallback channel 3) and 1 (open, a key with a PSN); their channels, one of a system not stored; a
// tag for channel 9, which isn't stored; and a key no one knows
function fixture() {
	const m = empty();
	const ch = (number, name, f = {}) => newChannel({ number, name, mode: "dmr", rxHz: 451000000, txHz: 456000000, ...f });
	m.channels.push(ch(1, "County"), ch(2, "Metro DMR"), ch(3, "Analog", { mode: "fm", rxHz: 462562500, txHz: 462562500 }), ch(4, "Stale"), ch(5, "Spare"));
	const img = encode(m);
	putArea(img.bytes, [
		{ key: 0x0100, value: systemRecord({ name: "County", model: 1, net: 100, cat: 2, cc: 3, opts: 0x0800 | 0x0040 | 0x0020, ownId: 3100123,
			sharing: 1, hold: 45, fb: 3, fbName: "Analog", base: 451000000, first: 1, sep: 3, planFlags: 1, split: 5000000, leave: -100, ps: 3, sites: [1, 7] }) },
		{ key: 0x0101, value: systemRecord({ name: "Metro", model: 3, net: 2, sharing: 2 }) },
		UNKNOWN,
		{ key: 0x0200, value: chanRecord(0, true, 1, 451012500, 456012500) },
		{ key: 0x0201, value: chanRecord(0, false, 2, 451025000, 456025000, 3) },
		{ key: 0x0202, value: chanRecord(1, true, 10, 452000000, 457000000) },
		{ key: 0x0203, value: chanRecord(5, true, 1, 453000000, 458000000) },
		{ key: 0x0300, value: KEY0 },
		{ key: 0x0301, value: KEY1 },
		{ key: 0x0400, value: tagRecord(0, "County", false, 3, "Analog") },
		{ key: 0x0401, value: tagRecord(1, "Metro DMR", true) },
		{ key: 0x0403, value: tagRecord(0, "Not Stale") },
		{ key: 0x0408, value: tagRecord(0, "Nine") },
	]);
	return img;
}

const entries = (img) => decodeArea(img.bytes).entries;
const value = (img, key) => getValue(entries(img), key);
const byName = (m, name) => m.channels.find((c) => c.name === name);
const sameArea = (a, b) => assert.deepEqual(a.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE), b.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE));

test("name check: FNV-1a over the 16-octet NUL-padded name, folded to 16 bits", () => {
	assert.equal(nameCheck("County"), 0xb880);
	assert.equal(nameCheck("Metro DMR"), 0xa52b);
	// the name as the channel record stores it: 16 characters at most
	assert.equal(nameCheck("A name longer than sixteen"), nameCheck("A name longer th"));
});

test("systems, channels, keys and tags decode into references", () => {
	const m = decode(fixture());
	const [s0, s1] = m.trunkSystems;
	assert.equal(m.trunkSystems.length, 2);
	assert.deepEqual([s0.number, s0.name, s0.model, s0.net, s0.category, s0.colorCode], [1, "County", 1, 100, "b", 3]);
	assert.deepEqual([s0.authenticates, s0.registerTalkgroups, s0.privateCalls, s0.acceptStun, s0.listenOnly], [true, true, true, false, false]);
	assert.deepEqual([s0.ownId, s0.sharing, s0.fallbackHold, s0.fallback], [3100123, 1, 45, byName(m, "Analog")]);
	assert.deepEqual([s0.planBaseHz, s0.planFirstLcn, s0.planSep, s0.planTxLow, s0.planNoLearn, s0.planSplitHz], [451000000, 1, 3, true, false, 5000000]);
	assert.deepEqual([s0.leaveDbm, s0.powerSave, s0.sites], [-100, 3, [1, 7]]);
	assert.deepEqual([s0.key, s0.psn], ["101112131415161718191A1B1C1D1E1F", 0]);
	assert.deepEqual([s1.key, s1.psn, s1.sharing, s1.authenticates], ["A0A1A2A3A4A5A6A7A8A9AAABACADAEAF", 0xB0B1B2, 2, false]);
	// the system channels in key order; one of a system not stored keeps its number (1-based)
	assert.deepEqual(m.trunkChannels.map((c) => [c.system, c.control, c.lcn, c.cc]), [[s0, true, 1, null], [s0, false, 2, 3], [s1, true, 10, null], [6, true, 1, null]]);
	const county = byName(m, "County"), metro = byName(m, "Metro DMR");
	assert.deepEqual([county.system, county.noFallback, county.fallback], [s0, false, byName(m, "Analog")]);
	assert.deepEqual([metro.system, metro.noFallback, metro.fallback], [s1, true, null]);
	// a tag whose check is another name's is not one the radio takes: an ordinary channel
	assert.equal(byName(m, "Stale").system, undefined);
	assert.equal(byName(m, "Analog").system, undefined);
	assert.ok(systemUsable(m, s0) && systemUsable(m, s1));
});

test("an unedited codeplug with trunked systems encodes to the same bytes", () => {
	const img = fixture();
	const out = encode(decode(img));
	assert.deepEqual(out.bytes, img.bytes);
	// and twice over
	assert.deepEqual(encode(decode(out)).bytes, img.bytes);
});

test("the records the editor writes are the spec's bytes", () => {
	const s = { ...newSystem({ name: "County", model: 1, net: 100, category: "b", colorCode: 3, authenticates: true, registerTalkgroups: true, ownId: 3100123,
		sharing: 1, fallbackHold: 45, planBaseHz: 451000000, planFirstLcn: 1, planSep: 3, planTxLow: true, planSplitHz: 5000000,
		leaveDbm: -100, powerSave: 3, sites: [1, 7] }), fallback: 3, fallbackCheck: nameCheck("Analog") };
	assert.deepEqual(encodeSystem(s), systemRecord({ name: "County", model: 1, net: 100, cat: 2, cc: 3, opts: 0x0860, ownId: 3100123, sharing: 1,
		hold: 45, fb: 3, fbName: "Analog", base: 451000000, first: 1, sep: 3, planFlags: 1, split: 5000000, leave: -100, ps: 3, sites: [1, 7] }));
	// an edited system's key and channels, written whole; the unknown key as it was
	const m = decode(fixture());
	const s1 = m.trunkSystems[1];
	s1.key = "00112233445566778899aabbccddeeff";
	s1.psn = 0;
	m.trunkSystems[0].psn = 0x123456;
	const img = encode(m);
	assert.deepEqual(value(img, 0x0301), Uint8Array.from([0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]));
	assert.deepEqual(value(img, 0x0300), Uint8Array.from([...KEY0, 0x12, 0x34, 0x56]));
	assert.deepEqual(value(img, UNKNOWN.key), UNKNOWN.value);
	assert.deepEqual(decodeKey(value(img, 0x0300)), { key: "101112131415161718191A1B1C1D1E1F", psn: 0x123456 });
});

test("renaming a channel rewrites its tag's check, and a fallback's check where it is one", () => {
	const m = decode(fixture());
	byName(m, "County").name = "County North";
	byName(m, "Analog").name = "Simplex";
	const img = encode(m);
	const tag = decodeTag(value(img, 0x0400));
	assert.equal(tag.check, nameCheck("County North"));
	assert.deepEqual([tag.fallback, tag.fallbackCheck], [3, nameCheck("Simplex")]);
	assert.equal(decodeSystem(value(img, 0x0100)).fallbackCheck, nameCheck("Simplex"));
	// read back, the references are the same channels
	const back = decode(img);
	assert.equal(byName(back, "County North").system, back.trunkSystems[0]);
	assert.equal(byName(back, "County North").fallback, byName(back, "Simplex"));
	assert.equal(back.trunkSystems[0].fallback, byName(back, "Simplex"));
	// the tags and keys of other channels and the unknown key are kept byte for byte
	const was = fixture();
	for (const key of [0x0401, 0x0403, 0x0408, 0x0300, 0x0301, 0x0200, 0x0203, UNKNOWN.key]) assert.deepEqual(value(img, key), value(was, key), key.toString(16));
});

test("renumbering moves the tags and fallback numbers with their channels", () => {
	const m = decode(fixture());
	byName(m, "County").number = 10;
	byName(m, "Analog").number = 7;
	const img = encode(m);
	assert.equal(value(img, 0x0400), null);
	const tag = decodeTag(value(img, 0x0409));
	assert.deepEqual([tag.system, tag.check, tag.fallback, tag.fallbackCheck], [0, nameCheck("County"), 7, nameCheck("Analog")]);
	assert.equal(decodeSystem(value(img, 0x0100)).fallback, 7);
	const back = decode(img);
	assert.equal(byName(back, "County").number, 10);
	assert.equal(byName(back, "County").system, back.trunkSystems[0]);
	assert.equal(back.trunkSystems[0].fallback, byName(back, "Analog"));
	// a tag the radio didn't take is left as found
	assert.deepEqual(value(img, 0x0403), value(fixture(), 0x0403));
	assert.deepEqual(value(img, 0x0408), value(fixture(), 0x0408));
});

test("deleting a channel removes its tag and clears a fallback on it", () => {
	const m = decode(fixture());
	remove(m, "channels", [byName(m, "Analog"), byName(m, "Metro DMR")]);
	assert.equal(m.trunkSystems[0].fallback, null);
	assert.equal(byName(m, "County").fallback, null);
	const img = encode(m);
	assert.equal(value(img, 0x0401), null, "the deleted channel's tag goes");
	const tag = decodeTag(value(img, 0x0400));
	assert.deepEqual([tag.fallback, tag.fallbackCheck], [0, 0]);
	const s = decodeSystem(value(img, 0x0100));
	assert.deepEqual([s.fallback, s.fallbackCheck], [0, 0]);
	assert.deepEqual(value(img, UNKNOWN.key), UNKNOWN.value);
});

test("a tag left over from another tool never makes a channel trunked by a later edit", () => {
	const m = decode(fixture());
	// channel 4 takes the name its stray tag was made for: the model says it is conventional, so the tag goes
	byName(m, "Stale").name = "Not Stale";
	const img = encode(m);
	assert.equal(value(img, 0x0403), null);
	assert.equal(decode(img).channels.find((c) => c.number === 4).system, undefined);
	// a channel made conventional loses its tag; one made trunked gets one
	const m2 = decode(fixture());
	setTrunk(m2, byName(m2, "County"), null);
	setTrunk(m2, byName(m2, "Spare"), m2.trunkSystems[1]);
	const img2 = encode(m2);
	assert.equal(value(img2, 0x0400), null);
	assert.deepEqual(value(img2, 0x0404), tagRecord(1, "Spare"));
	const spare = byName(decode(img2), "Spare");
	assert.deepEqual([spare.mode, spare.rxOnly, spare.rxHz, spare.txHz], ["dmr", true, 452000000, 457000000], "the record is the system's first control channel, RX only");
});

test("deleting a system removes its record, key and channels; its trunked channels become conventional", () => {
	const m = decode(fixture());
	const s1 = m.trunkSystems[1];
	remove(m, "trunkSystems", [s1]);
	assert.deepEqual([byName(m, "Metro DMR").system, byName(m, "Metro DMR").rxOnly], [null, true]);
	const img = encode(m);
	for (const key of [0x0101, 0x0301, 0x0401]) assert.equal(value(img, key), null, key.toString(16));
	const back = decode(img);
	assert.equal(back.trunkSystems.length, 1);
	assert.deepEqual(back.trunkChannels.map((c) => [c.system?.number ?? c.system, c.lcn]), [[1, 1], [1, 2], [6, 1]]);
	assert.deepEqual(entries(img).filter((e) => e.key >= 0x0200 && e.key < 0x0300).map((e) => e.key), [0x0200, 0x0201, 0x0202]);
	assert.deepEqual(value(img, 0x0300), KEY0);
	assert.deepEqual(value(img, UNKNOWN.key), UNKNOWN.value);
});

test("a new system: the lowest free number, its channels and key, read back the same", () => {
	const m = decode(fixture());
	const s = newSystem({ name: "Third" });
	putSystem(m, s, { name: "Third", model: 2, net: 9, key: "000102030405060708090A0B0C0D0E0F", psn: 0, authenticates: true },
		[{ control: true, lcn: 3, rxHz: 460000000, txHz: 465000000, cc: 5 }]);
	assert.equal(s.number, 3);
	const img = encode(m);
	assert.equal(value(img, 0x0302).length, 16, "a key without a PSN is 16 octets");
	const back = decode(img).trunkSystems.find((x) => x.number === 3);
	assert.deepEqual([back.name, back.model, back.net, back.authenticates, back.key], ["Third", 2, 9, true, "000102030405060708090A0B0C0D0E0F"]);
	assert.deepEqual(decode(img).trunkChannels.filter((c) => c.system?.number === 3).map((c) => [c.lcn, c.rxHz, c.cc]), [[3, 460000000, 5]]);
	// editing a system puts its channels where its old ones were: the other systems keep their order
	const m2 = decode(fixture());
	const s0 = m2.trunkSystems[0];
	putSystem(m2, s0, {}, [{ control: true, lcn: 4, rxHz: 451050000, txHz: 456050000, cc: null }]);
	assert.deepEqual(m2.trunkChannels.map((c) => [c.system?.number ?? c.system, c.lcn]), [[1, 4], [2, 10], [6, 1]]);
	// the system's trunked channels follow its first control channel
	assert.deepEqual([byName(m2, "County").rxHz, byName(m2, "County").txHz], [451050000, 456050000]);
});

test("a codeplug without trunking stays without: no area written for it", () => {
	const m = empty();
	m.channels.push(newChannel({ number: 1, name: "One" }));
	const img = encode(m);
	assert.equal(decodeArea(img.bytes).state, "empty");
	const back = decode(img);
	assert.deepEqual([back.trunkSystems, back.trunkChannels], [[], []]);
});

test("undo brings back a system edit, its channels and tags", () => {
	const m = decode(fixture());
	const h = new History();
	h.record(m);
	remove(m, "trunkSystems", [m.trunkSystems[0]]);
	assert.equal(byName(m, "County").system, null);
	h.undo(m);
	assert.equal(m.trunkSystems.length, 2);
	assert.equal(byName(m, "County").system, m.trunkSystems[0]);
	assert.equal(m.trunkChannels.length, 4);
	assert.deepEqual(encode(m).bytes, fixture().bytes);
});

test("validation: systems, their channels and keys, and trunked channels", () => {
	const codes = (m) => validate(m).filter((p) => p.table === "trunkSystems" || p.code.startsWith("t3_")).map((p) => [p.level, p.code, p.field, p.item?.name ?? null]);
	const m = decode(fixture());
	// the fixture: a channel record of system 6, which isn't stored; Metro DMR has no talkgroup
	assert.deepEqual(codes(m), [
		["warning", "t3_chan_no_system", null, null],
		["warning", "t3_no_talkgroup", "contact", "County"],
		["warning", "t3_no_talkgroup", "contact", "Metro DMR"],
	]);
	const [s0, s1] = m.trunkSystems;
	s0.net = 128;		// small: NET 0-127
	s0.key = "1234";
	s0.colorCode = 16;
	s1.key = "";
	s1.psn = 5;
	s1.authenticates = true;
	m.trunkChannels[2].lcn = 0;
	m.trunkChannels = m.trunkChannels.filter((c) => c.system !== s0 || !c.control);
	byName(m, "County").system = 7;
	byName(m, "Metro DMR").fallback = byName(m, "County");
	const out = codes(m);
	for (const want of [
		["error", "t3_net_range", "net", "County"],
		["error", "range", "colorCode", "County"],
		["error", "t3_key_length", "key", "County"],
		["warning", "t3_no_control", "controlChannels", "County"],
		["warning", "t3_psn_no_key", "psn", "Metro"],
		["warning", "t3_no_key", "key", "Metro"],
		["error", "range", "lcn", "Metro"],
		["warning", "t3_tag_no_system", "system", "County"],
		["warning", "t3_fallback_trunked", "fallback", "Metro DMR"],
	]) assert.ok(out.some((x) => JSON.stringify(x) === JSON.stringify(want)), JSON.stringify(want));
	// more than eight systems
	const many = decode(fixture());
	for (let i = 0; i < 7; i++) many.trunkSystems.push(newSystem({ name: `S${i}` }));
	assert.ok(codes(many).some(([level, code]) => level === "error" && code === "too_many"));
	// a stale fallback number: the channel it named was renamed by another tool
	const stale = decode(fixture());
	stale.trunkSystems[0].fallback = 4;
	assert.ok(codes(stale).some(([, code, field]) => code === "t3_fallback_stale" && field === "fallback"));
});

test("the settings area full is an error, not a write that fails", () => {
	const m = decode(fixture());
	for (let i = 0; i < 250; i++) m.trunkChannels.push({ system: m.trunkSystems[0], control: false, lcn: 100 + i, rxHz: 451000000, txHz: 456000000, cc: null });
	const p = validate(m).filter((x) => x.code === "settings_area_full");
	assert.equal(p.length, 1);
	assert.equal(p[0].level, "error");
	assert.ok(p[0].params.used > p[0].params.max);
});

test("the system dialog's parser: blanks are 0, typed values checked", () => {
	const raw = { name: " County ", model: 1, net: "100", category: "b", colorCode: 3, authenticates: true, key: "10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f",
		psn: "0x123456", sharing: 2, fallback: null, fallbackHold: "", ownId: "", planBaseHz: "451.0125", planFirstLcn: "1", planSep: 3, planSplitHz: "5",
		leaveDbm: "-100", powerSave: 0, sites: "1, 7", acceptStun: true,
		chans: [{ control: true, lcn: "1", rxHz: "451.0125", txHz: "456.0125", cc: null }] };
	const { fields, chans, errors } = parseSystem(raw, t);
	assert.deepEqual(errors, []);
	assert.deepEqual([fields.name, fields.net, fields.key, fields.psn, fields.fallbackHold, fields.ownId], ["County", 100, "101112131415161718191A1B1C1D1E1F", 0x123456, 0, 0]);
	assert.deepEqual([fields.planBaseHz, fields.planSplitHz, fields.leaveDbm, fields.sites, fields.acceptStun, fields.noRoaming], [451012500, 5000000, -100, [1, 7], true, false]);
	assert.deepEqual(chans, [{ control: true, lcn: 1, rxHz: 451012500, txHz: 456012500, cc: null }]);
	const bad = parseSystem({ ...raw, name: "", net: "128", key: "abc", sites: "1, 99", chans: [{ control: true, lcn: "0", rxHz: "x", txHz: "1", cc: null }] }, t).errors;
	assert.equal(bad.length, 6, bad.join("\n"));
});

test("the channel table's DMR Trunked mode tags a channel with a system; FM or DMR untags it", () => {
	const m = decode(fixture());
	const cols = columns("channels", m, t);
	const mode = cols.find((c) => c.key === "mode"), sys = cols.find((c) => c.key === "system");
	const spare = byName(m, "Spare");
	assert.deepEqual(mode.options(spare).map(([v]) => v), ["fm", "dmr", "t3"]);
	assert.equal(mode.get(byName(m, "County")), "t3");
	assert.equal(mode.format("t3", byName(m, "County")), "editor.mode.t3");
	assert.equal(mode.set(spare, "t3"), undefined);
	assert.equal(spare.system, m.trunkSystems[0]);
	assert.deepEqual([spare.rxOnly, spare.rxHz, spare.txHz], [true, 451012500, 456012500]);
	assert.equal(sys.set(spare, m.trunkSystems[1]), undefined);
	assert.equal(spare.rxHz, 452000000);
	// a trunked channel's frequencies, slot and color code are its system's: locked in the table
	for (const k of ["rxHz", "txHz", "rxOnly", "colorCode", "timeslot"]) assert.equal(cols.find((c) => c.key === k).readOnly(spare), true, k);
	assert.equal(cols.find((c) => c.key === "contact").readOnly(spare), false);
	mode.set(spare, "fm");
	assert.deepEqual([spare.system, spare.mode], [null, "fm"]);
	// no system yet: no DMR Trunked to pick
	const plain = empty();
	plain.channels.push(newChannel({ number: 1, name: "One" }));
	const c2 = columns("channels", plain, t);
	assert.deepEqual(c2.find((c) => c.key === "mode").options(plain.channels[0]).map(([v]) => v), ["fm", "dmr"]);
	assert.equal(c2.find((c) => c.key === "system"), undefined);
	assert.equal(c2.find((c) => c.key === "mode").set(plain.channels[0], "t3"), "editor.err.t3_no_system");
});

test("the channel form shows the trunking part on a trunked channel and hides what the system decides", () => {
	const m = decode(fixture());
	const form = channelForm(m, t);
	const f = (key) => form.find((x) => x.key === key);
	const county = { ...byName(m, "County") }, analog = { ...byName(m, "Analog") };
	for (const k of ["system", "noFallback", "fallback"]) assert.deepEqual([f(k).show(county), f(k).show(analog)], [true, false], k);
	for (const k of ["freqs", "rxOnly", "colorCode", "timeslot", "dmo", "sfr"]) assert.equal(f(k).show(county), false, k);
	assert.equal(f("contact").show(county), true);
	// the channel's own fallback: the system's, or a conventional channel other than itself
	const opts = f("fallback").options(county).map(([v]) => v);
	assert.equal(opts[0], null);
	assert.ok(opts.includes(byName(m, "Analog")) && !opts.includes(byName(m, "County")) && !opts.includes(byName(m, "Metro DMR")));
	// the setters leave a conventional channel alone
	f("noFallback").set(analog, true);
	assert.equal(analog.noFallback, undefined);
});

test("a qdmr import replaces the channels: the systems stay, their fallback channels go", () => {
	const m = decode(fixture());
	importQdmr(m, "channels:\n  - analog: {id: ch1, name: Analog, rxFrequency: 446.0 MHz, txFrequency: 446.0 MHz}\n");
	assert.equal(m.trunkSystems.length, 2);
	assert.equal(m.trunkSystems[0].fallback, null);
	const img = encode(m);
	const s = decodeSystem(value(img, 0x0100));
	assert.deepEqual([s.fallback, s.fallbackCheck], [0, 0]);
	for (const key of [0x0400, 0x0401]) assert.equal(value(img, key), null, "no channel is trunked");
	assert.deepEqual(value(img, 0x0300), KEY0);
});
