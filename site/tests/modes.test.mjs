// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The channel-mode layer of the Codeplug Editor (docs/spec/channel-modes.md): static/js/codeplug/channelmodes.js, the
// registry (modes.js) and the DMR Tier III and P25 modules, against the shared vectors (tests/vectors/channel-modes.txt)
// the C host tests read too. Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { CodeplugImage } from "../static/js/codeplug/image.js";
import { decodeArea, getValue, setValue, putArea, KV_ADDR, KV_SIZE } from "../static/js/codeplug/kv.js";
import { decode, encode, empty, newChannel, newContact, remove, validate, numberAll } from "../static/js/codeplug/model.js";
import { MODE_KEY, decodeTag, encodeTag, newSystem, setKind, syncModes, decodeSystemRecord, identityKey, countSecrets, withoutSecrets } from "../static/js/codeplug/channelmodes.js";
import { KINDS, SYSTEM_TYPES, offeredKinds, offeredTypes, kindName } from "../static/js/codeplug/modes.js";
import { nameCheck } from "../static/js/codeplug/modebytes.js";
import { decodeConv, encodeConv, decodeUnit, decodeTsys, p25System, NAC_ANY } from "../static/js/codeplug/mode_p25.js";
import { t3System, OPTS as T3_OPTS } from "../static/js/codeplug/mode_t3.js";
import { aesSystem, randomAesKey } from "../static/js/codeplug/mode_aes.js";

const VECTORS = new URL("../../tests/vectors/channel-modes.txt", import.meta.url);
const bytes = (s) => (s === "-" ? null : Uint8Array.from(s.match(/../g) ?? [], (x) => parseInt(x, 16)));
const hexOf = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase();
const vectors = (kind) => readFileSync(VECTORS, "utf8").split(/\r?\n/).filter((l) => l.startsWith(kind + " ")).map((l) => l.slice(kind.length + 1));
const area = (img) => decodeArea(img.bytes).entries;
const round = (m) => decode(encode(m));
const has = (p, code, c) => p.some((x) => x.code === code && (!c || x.item === c));

test("vectors: name check", () => {
	const v = vectors("check");
	assert.ok(v.length >= 4);
	for (const l of v) {
		const [chk, ...name] = l.split(" ");
		assert.equal(nameCheck(name.join(" ")), parseInt(chk, 16), JSON.stringify(name.join(" ")));
	}
});

test("vectors: P25 conventional block, unit ID record", () => {
	for (const l of vectors("p25conv")) {
		const [b, rx, tx, sq] = l.split(" ");
		assert.deepEqual(decodeConv(bytes(b)), { squelch: parseInt(sq, 16), rxNac: parseInt(rx, 16), txNac: parseInt(tx, 16) }, l);
	}
	for (const l of vectors("p25enc")) {
		const [rx, tx, sq, b] = l.split(" ");
		assert.equal(hexOf(encodeConv({ squelch: parseInt(sq, 16), rxNac: parseInt(rx, 16), txNac: parseInt(tx, 16) })), b, l);
	}
	for (const l of vectors("p25unit")) {
		const [b, dmr, unit] = l.split(" ");
		const u = decodeUnit(bytes(b)).unitId;
		assert.equal(u || parseInt(dmr, 16), parseInt(unit, 16), l);
	}
});

test("vectors: P25 trunked system block and identifier plan rows", () => {
	for (const l of vectors("p25tsys")) {
		const [b, wacn, sysid, nac, opts, hold, roam] = l.split(" ").map((x, i) => (i ? parseInt(x, 16) : x));
		const s = decodeTsys(bytes(b));
		assert.equal(s.wacn ?? 0xFFFFFF, wacn, l);
		assert.equal(s.sysid ?? 0xFFFF, sysid, l);
		assert.equal(s.ccNac ?? NAC_ANY, nac, l);
		assert.equal((s.skipEnc ? 1 : 0) | (s.planOnly ? 2 : 0) | (s.noReg ? 4 : 0), opts, l);
		assert.equal(s.holdS, hold, l);
		assert.equal(s.roamDbm & 0xFF, roam, l);
	}
	const plan = p25System.classes.find((c) => c.cls === 2);
	for (const l of vectors("p25iden")) {
		const [b, base, sp, off, bw, type, slots] = l.split(" ").map((x, i) => (i ? parseInt(x, 16) : x));
		const e = plan.decode(bytes(b));
		assert.deepEqual([e.baseHz, e.spacingHz, e.txOffsetHz >>> 0, e.bwHz, e.tdma ? 1 : 0, e.tdma ? e.slots : 1], [base, sp, off, bw, type, slots], l);
	}
});

test("registry: unique ids, names, a module each", () => {
	for (const list of [KINDS, SYSTEM_TYPES]) {
		assert.equal(new Set(list.map((k) => k.id)).size, list.length);
		for (const k of list) assert.ok(k.id >= 1 && k.id < 0xFF && k.module.id === k.id && k.short);
	}
	assert.equal(kindName(0x80), "?");
	for (const k of KINDS) if (k.module.systemType) assert.ok(SYSTEM_TYPES.some((s) => s.id === k.module.systemType));
});

// A codeplug with one channel of each kind, a Tier III and a P25 trunked system, a key and the unit ID
function sample() {
	const m = empty();
	const tg = newContact({ name: "TG 100", id: 100 });
	m.contacts.push(tg);
	const plain = newChannel({ name: "Plain FM" });
	const conv = newChannel({ name: "P25 Ops", rxHz: 453100000, txHz: 458100000, contact: tg });
	const t3 = newChannel({ name: "T3 Ops", contact: tg });
	const p25t = newChannel({ name: "P25 Trunk", contact: tg });
	m.channels.push(plain, conv, t3, p25t);
	const s1 = newSystem(m, 1, "Site net");
	s1.fields.net = 5;
	s1.needsKey = true;
	s1.secret = { key: "00112233445566778899AABBCCDDEEFF", psn: 0x123456 };
	s1.entries.push({ cls: 0, group: 0xFF, control: true, flags: 0, chan: 12, rxHz: 451000000, txHz: 456000000, cc: null });
	const s3 = newSystem(m, 3, "County");
	s3.fields.wacn = 0xBEE00;
	s3.fields.sysid = 0x1A2;
	s3.entries.push({ cls: 1, group: 0, control: false, flags: 0, rfss: 1, site: 7 });
	s3.entries.push({ cls: 0, group: 0, control: true, flags: 0, iden: 1, chanNo: 0x123, chan: 0, rxHz: 851012500, txHz: 0 });
	s3.entries.push({ cls: 2, group: 1, control: false, flags: 0, baseHz: 851006250, spacingHz: 6250, txOffsetHz: -45000000, bwHz: 12500, tdma: false, slots: 2 });
	m.systems.push(s1, s3);
	numberAll(m);
	setKind(m, conv, 2);
	conv.params = { ...conv.params, rxNac: 0x34, squelch: 1 };
	setKind(m, t3, 1);
	t3.system = s1;
	t3.fallback = plain;
	setKind(m, p25t, 3);
	p25t.system = s3;
	p25t.params = { followAll: true };
	m.modeSettings[2] = { unitId: 123456 };
	return { m, plain, conv, t3, p25t, s1, s3 };
}

test("model: channels of every kind, systems, entries, the key and the unit ID round-trip", () => {
	const { m } = sample();
	const img = encode(m);
	const d = decode(img);
	const by = (n) => d.channels.find((c) => c.name === n);
	assert.equal(by("Plain FM").kind, 0);
	const conv = by("P25 Ops"), t3 = by("T3 Ops"), p25t = by("P25 Trunk");
	assert.equal(conv.kind, 2);
	assert.deepEqual(conv.params, { squelch: 1, rxNac: 0x34, txNac: 0x293 });
	assert.equal(conv.tagTx, true);
	assert.equal(conv.mode, "dmr");
	assert.equal(conv.rxOnly, true);
	assert.equal(conv.rxHz, 453100000);
	assert.equal(t3.kind, 1);
	assert.equal(t3.system.name, "Site net");
	assert.equal(t3.fallback, by("Plain FM"));
	assert.equal(t3.tagTx, true);
	assert.equal(t3.rxHz, 451000000, "the record holds the control channel");
	assert.equal(p25t.kind, 3);
	assert.equal(p25t.tagTx, false, "P25 trunked: receive only by default");
	assert.deepEqual(p25t.params, { followAll: true });
	assert.equal(p25t.rxHz, 851012500);
	assert.equal(p25t.txHz, 851012500);
	const s1 = d.systems.find((s) => s.type === 1), s3 = d.systems.find((s) => s.type === 3);
	assert.equal(s1.fields.net, 5);
	assert.equal(s1.tx, true);
	assert.equal(s1.needsKey, true);
	assert.deepEqual(s1.secret, { key: "00112233445566778899AABBCCDDEEFF", psn: 0x123456 });
	assert.equal(s3.tx, false, "P25 trunked: listen only by default");
	assert.equal(s3.fields.wacn, 0xBEE00);
	assert.deepEqual(s3.entries.map((e) => e.cls), [1, 0, 2]);
	assert.equal(s3.entries[1].iden, 1);
	assert.equal(s3.entries[1].chanNo, 0x123);
	assert.equal(s3.entries[2].txOffsetHz, -45000000);
	assert.deepEqual(d.modeSettings[2], { unitId: 123456 });
	// unedited: byte for byte
	assert.deepEqual(encode(d).bytes, img.bytes);
	// the stored bytes: the system header of 7.1, the key, the tag
	const e = area(img);
	const sys = decodeSystemRecord(getValue(e, MODE_KEY.system + s1.number - 1));
	assert.equal(sys.type, 1);
	assert.equal(sys.idLen, 4);
	assert.equal(sys.flags, 3);
	assert.equal(getValue(e, MODE_KEY.secret + s1.number - 1).length, 19);
	const tag = decodeTag(getValue(e, MODE_KEY.tag + t3.number - 1));
	assert.deepEqual([tag.kind, tag.system, tag.check, tag.flags & 0x0F, tag.fbChan], [1, s1.number - 1, nameCheck("T3 Ops"), 0x05, by("Plain FM").number]);
	assert.equal(hexOf(getValue(e, MODE_KEY.setting + 2)), "40E20100");
});

test("tags: follow a rename, go when the channel is made plain or deleted, a stale one is kept", () => {
	const { m } = sample();
	const d = round(m);
	const conv = d.channels.find((c) => c.name === "P25 Ops"), n = conv.number;
	conv.name = "P25 Two";
	let e = area(encode(d));
	assert.equal(decodeTag(getValue(e, MODE_KEY.tag + n - 1)).check, nameCheck("P25 Two"));
	// a stale tag on a plain channel (a check of neither name) stays
	const d2 = round(m);
	const plain = d2.channels.find((c) => c.name === "Plain FM");
	const stale = encodeTag({ kind: 2, system: 0xFF, check: 0x1234, flags: 1, params: new Uint8Array(0) });
	putArea(d2.base.bytes, setValue(area(d2.base), MODE_KEY.tag + plain.number - 1, stale));
	const d3 = decode(d2.base.clone());
	assert.equal(d3.channels.find((c) => c.name === "Plain FM").kind, 0);
	d3.channels.find((c) => c.name === "Plain FM").name = "Plain 2";
	assert.deepEqual(getValue(area(encode(d3)), MODE_KEY.tag + plain.number - 1), stale);
	// made plain: the tag goes
	const d4 = round(m);
	const c4 = d4.channels.find((c) => c.name === "P25 Ops");
	setKind(d4, c4, 0, "fm");
	e = area(encode(d4));
	assert.equal(getValue(e, MODE_KEY.tag + c4.number - 1), null);
	assert.equal(c4.rxOnly, false, "transmit allowed comes back as the record's");
	// a kind whose record holds its system's frequencies: made plain, receive only
	const t3p = d4.channels.find((c) => c.name === "T3 Ops");
	t3p.tagTx = true;
	setKind(d4, t3p, 0, "dmr");
	assert.equal(t3p.rxOnly, true, "never transmit on a system's control channel");
	// deleted: the tag goes, and a fallback naming it is none
	const d5 = round(m);
	remove(d5, "channels", [d5.channels.find((c) => c.name === "Plain FM")]);
	const t3 = d5.channels.find((c) => c.name === "T3 Ops");
	assert.equal(t3.fallback, null);
	e = area(encode(d5));
	assert.equal(decodeTag(getValue(e, MODE_KEY.tag + t3.number - 1)).flags & 0x04, 0, "no fallback of its own");
	const d6 = round(m);
	const c6 = d6.channels.find((c) => c.name === "P25 Ops");
	remove(d6, "channels", [c6]);
	assert.equal(getValue(area(encode(d6)), MODE_KEY.tag + c6.number - 1), null);
});

test("a tag, a system and an entry of ids this editor doesn't know are kept byte for byte", () => {
	const img = CodeplugImage.blank();
	const m = decode(img);
	const c = newChannel({ name: "Future", mode: "dmr", rxOnly: true });
	const other = newChannel({ name: "Other" });
	m.channels.push(c, other);
	numberAll(m);
	const base = encode(m);
	const tag = encodeTag({ kind: 0x80, system: 0, check: nameCheck("Future"), flags: 0x31, params: Uint8Array.of(9, 8, 7) });
	const sys = new Uint8Array(28 + 3);
	sys.set([0x80, 0x06, 1, 2]);
	sys.set([1, 2, 3], 28);
	const ent = Uint8Array.of(0, 0x81, 0xFF, 0, 5, 5);
	let e = setValue(area(base), MODE_KEY.tag, tag);
	e = setValue(e, MODE_KEY.system, sys);
	e = setValue(e, MODE_KEY.entry, ent);
	e = setValue(e, MODE_KEY.setting + 0x80, Uint8Array.of(1));
	putArea(base.bytes, e);
	const d = decode(base);
	const f = d.channels.find((x) => x.name === "Future");
	assert.equal(f.kind, 0x80);
	assert.equal(f.params, null);
	assert.equal(d.systems[0].type, 0x80);
	assert.ok(validate(d).some((p) => p.code === "mode_kind_unknown"));
	assert.ok(validate(d).some((p) => p.code === "mode_type_unknown"));
	d.channels.find((x) => x.name === "Other").name = "Other 2";
	f.name = "Future 2";
	const out = area(encode(d));
	assert.deepEqual(getValue(out, MODE_KEY.system), sys);
	assert.deepEqual(getValue(out, MODE_KEY.entry), ent);
	assert.deepEqual(getValue(out, MODE_KEY.setting + 0x80), Uint8Array.of(1));
	const t = getValue(out, MODE_KEY.tag);
	assert.deepEqual([...t.subarray(4)], [0x31, 9, 8, 7], "flags and parameters kept");
	assert.equal(decodeTag(t).check, nameCheck("Future 2"), "the check follows the rename");
});

test("bytes a module doesn't cover are kept: a longer parameter block, a longer system block", () => {
	const { m } = sample();
	const img = encode(m);
	const d = decode(img);
	const conv = d.channels.find((c) => c.name === "P25 Ops");
	const key = MODE_KEY.tag + conv.number - 1;
	const long = Uint8Array.from([...getValue(area(img), key), 0xAB, 0xCD]);
	putArea(img.bytes, setValue(area(img), key, long));
	const d2 = decode(img);
	d2.channels.find((c) => c.name === "P25 Ops").params.txNac = 0x55;
	const t = getValue(area(encode(d2)), key);
	assert.deepEqual([...t.subarray(-2)], [0xAB, 0xCD]);
	assert.equal(decodeConv(decodeTag(t).params).txNac, 0x55);
});

test("deleting a system drops its key, its entries, and turns its channels plain and receive only", () => {
	const { m } = sample();
	const d = round(m);
	const s1 = d.systems.find((s) => s.type === 1);
	remove(d, "systems", [s1]);
	const t3 = d.channels.find((c) => c.name === "T3 Ops");
	assert.equal(t3.kind, 0);
	assert.equal(t3.rxOnly, true);
	const e = area(encode(d));
	assert.equal(getValue(e, MODE_KEY.system + s1.number - 1), null);
	assert.equal(getValue(e, MODE_KEY.secret + s1.number - 1), null);
	const d2 = decode(encode(d));
	assert.equal(d2.systems.length, 1);
	assert.equal(d2.systems[0].entries.length, 3);
});

test("offered kinds: the radio's features and what the codeplug holds", () => {
	const m = empty();
	assert.deepEqual(offeredKinds(m, "FM,DMR").map((k) => k.id), []);
	assert.deepEqual(offeredKinds(m, "FM,DMR,P25").map((k) => k.id), [2]);
	const { m: s } = sample();
	assert.deepEqual(offeredKinds(s, "FM,DMR").map((k) => k.id), [1, 2, 3]);
	assert.deepEqual(offeredKinds(m, null).map((k) => k.id), KINDS.map((k) => k.id), "no radio: every kind");
	assert.deepEqual(offeredTypes(m, null).map((k) => k.id), SYSTEM_TYPES.map((k) => k.id), "no radio: every type");
});

test("validation: mode not in the radio, no system, unit ID, a P25 talk group", () => {
	const { m, t3, conv } = sample();
	t3.system = null;
	m.modeSettings[2] = { unitId: 10000000 };
	conv.contact.id = 70000;
	const p = validate(m, { features: "FM,DMR,P25" });
	const has = (code, c) => p.some((x) => x.code === code && (!c || x.item === c));
	assert.ok(has("mode_not_in_radio", t3));
	assert.ok(!has("mode_not_in_radio", conv));
	assert.ok(has("mode_no_system", t3));
	assert.ok(has("p25_unit_long"));
	assert.ok(has("p25_tg_range", conv));
	assert.ok(!validate(m).some((x) => x.code === "mode_not_in_radio"), "no radio: nothing to check against");
});

test("identity: two systems of the same type and identity are the same system", () => {
	const m = empty();
	const a = newSystem(m, 3, "A"), b = newSystem(m, 3, "B");
	a.fields.wacn = b.fields.wacn = 0x12345;
	a.fields.sysid = b.fields.sysid = 0x10;
	b.fields.holdS = 9;
	assert.equal(identityKey(a), identityKey(b));
	m.systems.push(a, b);
	assert.ok(validate(m).some((x) => x.code === "mode_same_identity"));
});

test("a system of either type with no control channel is flagged", () => {
	const m = empty();
	for (const type of [1, 3]) m.systems.push(newSystem(m, type, `S${type}`));
	const flagged = validate(m).filter((x) => x.code === "mode_no_control").map((x) => x.item.type);
	assert.deepEqual(flagged.sort(), [1, 3]);
});

test("an area of a newer version is never written", () => {
	const { m } = sample();
	const img = encode(m);
	img.bytes[KV_ADDR + 4] = 9;
	const d = decode(img);
	assert.equal(d.systems.length, 0);
	const out = encode(d);
	assert.deepEqual(out.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE), img.bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE));
	syncModes(d);
});

// --- The editor's forms (views.js, modefields.js), without a page ---

import { channelForm, columns } from "../static/js/editor/views.js";
import { parseSpec, formatSpec, specField } from "../static/js/editor/modefields.js";

const t = (k, p) => (p ? `${k} ${JSON.stringify(p)}` : k);

test("channel form: the mode list offers the radio's kinds; picking one makes the channel that kind", () => {
	const m = empty();
	const c = newChannel({ name: "Ch", rxOnly: false });
	m.channels.push(c);
	numberAll(m);
	const mode = (f) => f.find((x) => x.key === "mode");
	assert.deepEqual(mode(channelForm(m, t, { features: "FM,DMR" })).options(c).map(([v]) => v), ["fm", "dmr"]);
	const form = channelForm(m, t, { features: "FM,DMR,P25" });
	const f = mode(form);
	assert.deepEqual(f.options(c).map(([v]) => v), ["fm", "dmr", "k2"]);
	const work = { ...c };
	f.set(work, "k2");
	assert.equal(work.kind, 2);
	assert.equal(work.mode, "dmr");
	assert.equal(work.rxOnly, true, "the record is the stand-in");
	assert.equal(work.tagTx, true, "transmit allowed carries over");
	assert.equal(f.get(work), "k2");
	// the RX-only box shows the tag's transmit allowed
	const rxOnly = form.find((x) => x.key === "rxOnly");
	assert.equal(rxOnly.get(work), false);
	rxOnly.set(work, true);
	assert.equal(work.tagTx, false);
	// the P25 fields: shown, parsed into the parameters; DMR's own fields hidden, the talk target kept
	const nac = form.find((x) => x.key === "k2.rxNac");
	assert.ok(nac.show(work));
	assert.equal(nac.parse(work, nac.key, "f7e"), undefined);
	assert.equal(work.params.rxNac, 0xF7E);
	assert.ok(nac.parse(work, nac.key, "1000"));
	const tx = form.find((x) => x.key === "k2.txNac");
	tx.parse(work, tx.key, "");
	assert.equal(work.params.txNac, 0x293, "blank TX NAC with RX any: the default");
	assert.equal(form.find((x) => x.key === "timeslot").show(work), false);
	assert.equal(form.find((x) => x.key === "contact").show(work), true);
	assert.equal(form.find((x) => x.key === "dmrId").show(work), true);
	assert.equal(form.find((x) => x.key === "squelch").show(work), false, "FM's fields hidden");
	// back to FM: plain, TX as it was in the tag
	f.set(work, "fm");
	assert.equal(work.kind, 0);
	assert.equal(work.rxOnly, true);
});

test("channel table: a channel of a kind shows its mode, offers every mode, and locks the fields of other modes", () => {
	const { m, conv, p25t } = sample();
	const cols = columns("channels", m, t);
	const col = (k) => cols.find((c) => c.key === k);
	assert.equal(col("mode").format(conv.mode, conv), "P25");
	assert.equal(col("mode").readOnly, false, "the kind can be changed in the table too");
	const offered = col("mode").options(conv).map(([v]) => v);
	assert.ok(["fm", "dmr", "k2", "k3"].every((v) => offered.includes(v)), "FM, DMR and the kinds, with no radio connected");
	assert.equal(col("mode").get(conv), "k2");
	assert.equal(col("timeslot").readOnly(conv), true);
	assert.equal(col("contact").readOnly(conv), false);
	assert.equal(col("rxHz").readOnly(conv), false);
	assert.equal(col("rxHz").readOnly(p25t), true, "a trunked channel's frequencies are its system's");
	assert.equal(col("rxOnly").get(p25t), true);
});

test("declared fields: hex with any, lists, keys, frequencies", () => {
	const hexAny = { type: "hex", min: 0, max: 0xFFFFF, any: true };
	assert.deepEqual(parseSpec(hexAny, "", t), { value: null });
	assert.deepEqual(parseSpec(hexAny, "bee00", t), { value: 0xBEE00 });
	assert.ok(parseSpec(hexAny, "100000", t).error);
	assert.equal(formatSpec({ ...hexAny, format: (v) => v.toString(16) }, null, t), "");
	const list = { type: "list", min: 0, max: 7, count: 3 };
	assert.deepEqual(parseSpec(list, "1, 2 7", t), { value: [1, 2, 7] });
	assert.ok(parseSpec(list, "1 2 3 4", t).error);
	assert.ok(parseSpec(list, "8", t).error);
	const key = { type: "hexbytes", bytes: 16 };
	assert.deepEqual(parseSpec(key, "00112233-44556677 8899aabb:ccddeeff", t), { value: "00112233445566778899AABBCCDDEEFF" });
	assert.ok(parseSpec(key, "0011", t).error);
	assert.deepEqual(parseSpec({ type: "freq", zero: true }, "", t), { value: 0 });
	assert.deepEqual(parseSpec({ type: "freq" }, "851.0125", t), { value: 851012500 });
	// a field of another mode neither sets nor complains
	const f = specField({ key: "x", label: "editor.x", type: "int", min: 0, max: 1 }, t, { on: () => false, get: (r) => r.p, put: () => assert.fail() }, "k");
	assert.equal(f.parse({}, f.key, "9"), undefined);
	// a select that reads other fields rebuilds its list in the form (#1042); a fixed one doesn't
	const sel = (x) => specField({ key: "s", label: "editor.s", type: "select", options: () => [], ...x }, t, { get: () => ({}), put() {} }, "k");
	assert.equal(sel({ dynamic: true }).dynamicOptions, true);
	assert.equal(sel({}).dynamicOptions, false);
});

// --- NXDN conventional (docs/spec/nxdn-codeplug.md) ---

import * as nxdn from "../static/js/codeplug/mode_nxdn.js";
import { systemOwnIdField, systemsTab } from "../static/js/editor/modesview.js";

test("Systems table leaves control channels and authentication key cells blank for NXDN", () => {
	const m = empty();
	const n = newSystem(m, 4, "Conventional");
	const t3 = newSystem(m, 1, "Trunked");
	n.number = 1;
	t3.number = 2;
	t3.entries.push({ control: true });
	t3.secret = { key: "00000000000000000000000000000000" };
	m.systems.push(n, t3);
	const h = (tag, attrs, ...children) => ({ tag, children });
	const page = systemsTab({ model: m, t: (key) => key, h, button: () => h("button") }, null);
	const rows = page.children.find((part) => part?.children[0]?.tag === "table").children[0].children[1].children;
	const cells = (row) => row.children.slice(6, 8).map((cell) => cell.children[0]);
	assert.deepEqual(cells(rows[0]), ["", ""]);
	assert.deepEqual(cells(rows[1]), ["1", "editor.modes.secret_set"]);
});

test("NXDN system Radio ID accepts blank or a whole number from 1 to 65519", () => {
	const m = empty();
	const system = newSystem(m, 4, "Operations");
	m.systems.push(system);
	const field = systemOwnIdField(nxdn.nxdnSystem, (key, { min, max } = {}) =>
		key === "editor.err.range" ? `${min}–${max}` : key);
	for (const [input, value] of [["", 0], ["  ", 0], ["1", 1], ["65519", 65519]]) {
		assert.equal(field.parse(system, "ownId", input), undefined);
		assert.equal(system.ownId, value);
	}
	for (const input of ["0", "-1", "1.5", "65520", "abc"]) {
		assert.equal(field.parse(system, "ownId", input), "1–65519");
		assert.equal(system.ownId, 65519);
	}
	system.ownId = 65520;
	assert.ok(has(validate(m), "range", system));
});

test("vectors: NXDN conventional block, unit ID record", () => {
	const conv = (rate, rx, tx, sq) => ({ rate9600: rate === "1", rxRan: parseInt(rx, 16), txRan: parseInt(tx, 16), squelch: parseInt(sq, 16) });
	assert.ok(vectors("nxdnconv").length >= 10);
	for (const l of vectors("nxdnconv")) {
		const [b, ...p] = l.split(" ");
		assert.deepEqual(nxdn.decodeConv(bytes(b) ?? new Uint8Array(0)), conv(...p), l);
	}
	for (const l of vectors("nxdnenc")) {
		const [rate, rx, tx, sq, b] = l.split(" ");
		assert.equal(hexOf(nxdn.encodeConv(conv(rate, rx, tx, sq))), b, l);
	}
	for (const l of vectors("nxdnunit")) {
		const [b, dmr, unit] = l.split(" ");
		const u = nxdn.decodeUnit(bytes(b)).unitId;
		assert.equal(u || nxdn.unitFallback(parseInt(dmr, 16)), parseInt(unit, 16), l);
	}
});

// A codeplug with an NXDN channel on a group, one on a unit, and the radio-wide unit ID
function nxdnSample() {
	const m = empty();
	const grp = newContact({ name: "Group 7", id: 7 });
	const unit = newContact({ name: "Unit 42", id: 42, type: "private" });
	m.contacts.push(grp, unit);
	const a = newChannel({ name: "NXDN Ops", rxHz: 451250000, txHz: 456250000, contact: grp });
	const b = newChannel({ name: "NXDN Unit", rxHz: 451250000, txHz: 451250000, contact: unit });
	m.channels.push(a, b);
	numberAll(m);
	setKind(m, a, 4);
	a.params = { ...a.params, rxRan: 9, txRan: 9, squelch: 1 };
	setKind(m, b, 4);
	b.params = { ...b.params, rate9600: false, rxRan: 0, txRan: 2 };
	m.modeSettings[4] = { unitId: 1234 };
	return { m, a, b };
}

test("NXDN: channels and the unit ID round-trip; unknown bytes are kept", () => {
	const { m } = nxdnSample();
	const img = encode(m);
	const d = decode(img);
	const a = d.channels.find((c) => c.name === "NXDN Ops"), b = d.channels.find((c) => c.name === "NXDN Unit");
	assert.equal(a.kind, 4);
	assert.deepEqual(a.params, { rate9600: true, squelch: 1, rxRan: 9, txRan: 9 });
	assert.equal(a.tagTx, true);
	assert.equal(a.mode, "dmr");
	assert.equal(a.rxOnly, true, "the record is the safe stand-in");
	assert.deepEqual(b.params, { rate9600: false, squelch: 0, rxRan: 0, txRan: 2 });
	assert.equal(b.contact.type, "private");
	assert.deepEqual(d.modeSettings[4], { unitId: 1234 });
	assert.deepEqual(encode(d).bytes, img.bytes);
	const e = area(img);
	assert.equal(hexOf(decodeTag(getValue(e, MODE_KEY.tag + a.number - 1)).params), "010900");
	assert.equal(hexOf(decodeTag(getValue(e, MODE_KEY.tag + b.number - 1)).params), "040082");
	assert.equal(hexOf(getValue(e, MODE_KEY.setting + 4)), "D204");
	// a longer block keeps its tail through an edit
	const key = MODE_KEY.tag + a.number - 1;
	putArea(img.bytes, setValue(area(img), key, Uint8Array.from([...getValue(area(img), key), 0xAB])));
	const d2 = decode(img);
	d2.channels.find((c) => c.name === "NXDN Ops").params.txRan = 5;
	const t = decodeTag(getValue(area(encode(d2)), key)).params;
	assert.equal(hexOf(t), "010985AB");
	// a blank unit ID writes no record
	d2.modeSettings[4] = { unitId: 0 };
	assert.equal(getValue(area(encode(d2)), MODE_KEY.setting + 4), null);
});

test("NXDN: offered with the radio's feature; group, unit and channel ID ranges", () => {
	assert.deepEqual(offeredKinds(empty(), "FM,DMR,NXDN").map((k) => k.id), [4]);
	assert.equal(kindName(4), "NXDN");
	const { m, a, b } = nxdnSample();
	assert.deepEqual(offeredKinds(m, "FM,DMR").map((k) => k.id), [4], "the codeplug holds it");
	let p = validate(m, { features: "FM,DMR,NXDN" });
	assert.ok(!has(p, "mode_not_in_radio", a));
	assert.ok(!has(p, "nxdn_group_range") && !has(p, "nxdn_unit_range"));
	assert.ok(has(validate(m, { features: "FM,DMR,P25" }), "mode_not_in_radio", a));
	a.contact.id = 70000;
	b.contact.id = 65520;
	p = validate(m, { features: "FM,DMR,NXDN" });
	assert.ok(has(p, "nxdn_group_range", a));
	assert.ok(has(p, "nxdn_unit_range", b));
	b.contact.id = 42;
	a.contact.id = 7;
	a.dmrId = 3101276;
	assert.ok(has(validate(m, { features: "FM,DMR,NXDN" }), "nxdn_unit_range", a));
});

test("AES keys are traffic keys: round-trip by system and ID, stripped exports retain the protected channel", () => {
	const m = empty(), c = newChannel({ name: "Voice" });
	m.channels.push(c);
	for (const name of ["North", "South"]) m.systems.push(newSystem(m, 5, name));
	numberAll(m);
	m.systems[0].trafficKeys = [{ id: 0, key: "12".repeat(32) }, { id: 255, key: "34".repeat(32) }];
	m.systems[1].trafficKeys = [{ id: 0, key: "56".repeat(32) }];
	setKind(m, c, 5);
	c.params.id = 0;
	const keys = (x) => x.systems.map((s) => s.trafficKeys.map(({ id, key }) => ({ id, key })));
	const img = encode(m), again = decode(img);
	// system, algorithm 0x25, key ID (u16), flags, the 32 key bytes
	assert.equal(hexOf(getValue(area(img), MODE_KEY.traffic)), "0025000000" + "12".repeat(32));
	assert.deepEqual(keys(again), keys(m));
	assert.equal(again.channels[0].params.id, 0);
	assert.equal(again.channels[0].rxOnly, true);
	again.systems[0].trafficKeys.shift();
	const updated = round(again);
	assert.deepEqual(keys(updated), [keys(m)[0].slice(1), keys(m)[1]]);
	assert.equal(countSecrets(img.bytes), 3);
	withoutSecrets(img.bytes);
	assert.equal(countSecrets(img.bytes), 0);
	const stripped = decode(img);
	assert.equal(stripped.channels[0].kind, 5);
	assert.equal(stripped.channels[0].params.id, 0);
	assert.ok(stripped.systems.every((s) => s.trafficKeys.length === 0));
});

test("AES: Generate fills a key from the CSPRNG", () => {
	const bytes = Uint8Array.from({ length: 32 }, (_, i) => i);
	const rng = { getRandomValues: (a) => (a.set(bytes.subarray(0, a.length)), a) };
	assert.equal(randomAesKey(rng), hexOf(bytes));
	const a = randomAesKey(), b = randomAesKey();
	assert.match(a, /^[0-9A-F]{64}$/);
	assert.notEqual(a, b);
	const col = aesSystem.traffic.columns.find((x) => x.key === "key");
	assert.ok(col.secret && col.generate);
	assert.match(col.generate(), /^[0-9A-F]{64}$/);
});

test("NXDN: traffic keys round-trip, resolve per system, and preserve opaque records", () => {
	const { m, a, b } = nxdnSample();
	const system = newSystem(m, 4, "Operations");
	system.trafficKeys = [{ id: 0, seed: 32767 }, { id: 63, seed: 1 }];
	m.systems.push(system);
	numberAll(m);
	a.system = system;
	a.params.txKeyId = 0;
	const form = channelForm(m, (key) => key);
	assert.deepEqual(form.find((field) => field.key === "k4.system").options(a).map(([value]) => value), [null, system]);
	const txKey = form.find((field) => field.key === "k4.txKeyId");
	assert.deepEqual(txKey.options(a).map(([value]) => value), [null, 0, 63]);
	assert.deepEqual(txKey.options(b).map(([value]) => value), [null]);
	assert.ok(!has(validate(m), "nxdn_missing_tx_key", a));
	const img = encode(m);
	assert.equal(hexOf(getValue(area(img), MODE_KEY.traffic)), "0001000000FF7F");
	const opaqueSlot = MODE_KEY.traffic + 9;
	const opaque = Uint8Array.of(0, 2, 0, 0, 0, 42);
	putArea(img.bytes, setValue(area(img), opaqueSlot, opaque));
	const decoded = decode(img);
	const [first, second] = decoded.channels;
	assert.equal(first.system, decoded.systems[0]);
	assert.equal(first.params.txKeyId, 0);
	assert.equal(second.name, b.name);
	assert.equal(second.system, null);
	assert.deepEqual(encode(decoded).bytes, img.bytes);

	decoded.systems[0].trafficKeys = decoded.systems[0].trafficKeys.filter((key) => key.id !== 0);
	assert.ok(has(validate(decoded), "nxdn_missing_tx_key", first));
	first.params.txKeyId = 63;
	const saved = area(encode(decoded));
	assert.equal(getValue(saved, MODE_KEY.traffic), null);
	assert.equal(hexOf(getValue(saved, MODE_KEY.traffic + 1)), "00013F00000100");
	assert.deepEqual(getValue(saved, opaqueSlot), opaque);

	const redacted = encode(decoded);
	assert.equal(countSecrets(redacted.bytes), 2);
	withoutSecrets(redacted.bytes);
	assert.equal(countSecrets(redacted.bytes), 0);

	remove(decoded, "systems", [decoded.systems[0]]);
	assert.equal(first.kind, 4);
	assert.equal(first.system, null);
	assert.ok(has(validate(decoded), "nxdn_missing_tx_key", first));
	assert.equal(countSecrets(encode(decoded).bytes), 0);
});

// --- Encrypted TX on amateur frequencies (docs/user/04-programming.md) ---

import { AMATEUR_HZ } from "../static/js/codeplug/channelmodes.js";

test("encrypted TX on amateur frequencies: AES and NXDN scrambler channels warn in 144-148, 222-225 and 420-450 MHz, edges included", () => {
	assert.deepEqual(AMATEUR_HZ, [[144000000, 148000000], [222000000, 225000000], [420000000, 450000000]]);
	const m = empty();
	const aes = newSystem(m, 5, "AES");
	aes.trafficKeys = [{ id: 1, key: "11".repeat(32) }];
	const nx = newSystem(m, 4, "NXDN");
	nx.trafficKeys = [{ id: 2, seed: 100 }];
	m.systems.push(aes, nx);
	numberAll(m);
	const chan = (kind, hz, params, extra = {}) => {
		const c = newChannel({ name: `C${m.channels.length}`, rxHz: hz, txHz: hz, contact: null, ...extra });
		m.channels.push(c);
		setKind(m, c, kind);
		c.system = kind === 5 ? aes : nx;
		Object.assign(c.params, params);
		return c;
	};
	const inBand = [144000000, 146520000, 148000000, 222000000, 223500000, 225000000, 420000000, 426250000, 446006250, 450000000];
	const outBand = [143999999, 148000001, 221999999, 225000001, 419999999, 450000001, 461462500, 902000000];
	const aesIn = inBand.map((hz) => chan(5, hz, { id: 1 }));
	const aesOut = outBand.map((hz) => chan(5, hz, { id: 1 }));
	const nxIn = inBand.map((hz) => chan(4, hz, { txKeyId: 2 }));
	const nxOut = outBand.map((hz) => chan(4, hz, { txKeyId: 2 }));
	const aesNone = chan(5, 146520000, { id: null });
	const nxClear = chan(4, 446006250, {});
	const rxOnly = chan(5, 146520000, { id: 1 });
	rxOnly.tagTx = false;
	const nxRxOnly = chan(4, 446006250, { txKeyId: 2 });
	nxRxOnly.tagTx = false;
	// RX in the band, TX outside it: only the TX frequency counts
	const splitOut = chan(5, 146520000, { id: 1 }, {});
	splitOut.txHz = 461462500;
	const splitIn = chan(4, 461462500, { txKeyId: 2 });
	splitIn.txHz = 446006250;
	const plain = newChannel({ name: "Plain", mode: "dmr", rxHz: 146520000, txHz: 146520000 });
	m.channels.push(plain);
	numberAll(m);
	const p = validate(m);
	const warned = (c) => p.some((x) => x.code === "crypto_amateur" && x.item === c && x.level === "warning" && x.field === "txHz");
	for (const c of [...aesIn, ...nxIn, splitIn]) assert.ok(warned(c), `${c.kind === 5 ? "AES" : "NXDN"} at ${c.txHz} warns`);
	for (const c of [...aesOut, ...nxOut, splitOut]) assert.ok(!warned(c), `${c.txHz} is outside the bands`);
	assert.ok(!warned(aesNone), "AES channel with no key: clear");
	assert.ok(!warned(nxClear), "NXDN channel with a Clear transmit key");
	assert.ok(!warned(rxOnly) && !warned(nxRxOnly), "receive only: no warning");
	assert.ok(!warned(plain), "a plain DMR channel");
	assert.ok(!p.some((x) => x.code === "crypto_amateur" && x.level !== "warning"), "a warning, never an error");
	// a system that may not transmit makes its channels receive only too
	aes.tx = false;
	assert.ok(!validate(m).some((x) => x.code === "crypto_amateur" && x.item.kind === 5));
	// nothing is blocked: the codeplug still encodes, keys and all
	aes.tx = true;
	const back = decode(encode(m));
	assert.equal(back.channels.find((c) => c.name === aesIn[1].name).params.id, 1);
	assert.ok(validate(back).some((x) => x.code === "crypto_amateur"));
});

test("Tier III: follow every talk group and learning the system are option bits 0x0008 and 0x0010; others kept", () => {
	assert.equal(T3_OPTS.followAll, 0x0008);
	assert.equal(T3_OPTS.learnSystem, 0x0010);
	const prev = new Uint8Array(39);
	prev[6] = 0x08;		// 0x0800, reserved: kept as stored
	const s = { ...t3System.newFields(), followAll: true, learnSystem: true };
	const v = t3System.encode(s, prev);
	assert.equal(v[5] | (v[6] << 8), 0x0800 | 0x0008 | 0x0010 | 0x0020);
	const d = t3System.decode(v);
	assert.ok(d.followAll && d.learnSystem && d.privateCalls && !d.allCalls);
	const off = t3System.encode({ ...d, followAll: false, learnSystem: false }, v);
	assert.equal(off[5] | (off[6] << 8), 0x0800 | 0x0020);
	const fields = t3System.fields().map((f) => [f.key, f.section]);
	assert.ok(fields.some(([k, sec]) => k === "followAll" && sec === "calls"));
	assert.ok(fields.some(([k, sec]) => k === "learnSystem" && sec === "identity"));
});

// --- The recommended ham systems (docs/spec/ham-trunking.md) ---

import { newHamSystem } from "../static/js/codeplug/channelmodes.js";
import { HAM_T3 } from "../static/js/codeplug/mode_t3.js";
import { HAM_P25 } from "../static/js/codeplug/mode_p25.js";

test("recommended ham systems: Tier III Huge NET 1 and P25 WACN 32350, transmit on, no key, one control channel", () => {
	const m = empty();
	const t3 = newHamSystem(m, 1, { name: "", rxHz: 442012500, txHz: 447012500, site: 5 });
	assert.equal(t3.name, "Ham Tier III");
	assert.deepEqual([t3.tx, t3.needsKey, t3.secret, t3.ownId], [true, false, null, 0]);
	assert.deepEqual([t3.fields.model, t3.fields.net, t3.fields.category, t3.fields.colorCode, t3.fields.registerTalkgroups, t3.fields.learnSystem, t3.fields.sites],
		[3, 1, "a", 1, true, false, []]);
	assert.deepEqual(t3.entries.map((e) => [e.cls, e.control, e.chan, e.rxHz, e.txHz, e.cc]), [[0, true, 21, 442012500, 447012500, null]]);
	// four carriers a site, every site's numbers its own, all within the LCN range
	assert.equal(HAM_T3.lcn(HAM_T3.siteMax, 4), 4092);
	const p25 = newHamSystem(m, 3, { name: "Club P25", rxHz: 442037500, txHz: 0 });
	assert.equal(p25.name, "Club P25");
	assert.deepEqual([p25.tx, p25.fields.wacn, p25.fields.sysid, p25.fields.ccNac, p25.fields.skipEnc], [true, HAM_P25.wacn, null, 0x293, true]);
	assert.notEqual(p25.fields.wacn, 0xBEE00, "never the common commercial default");
	assert.deepEqual(p25.entries.map((e) => [e.cls, e.group, e.control, e.rxHz, e.txHz]), [[0, 0, true, 442037500, 442037500]]);
	assert.equal(newHamSystem(m, 4, { rxHz: 1 }), null, "NXDN has none");
	// added and written: both validate without errors and read back the same
	for (const s of [t3, p25]) m.systems.push(s);
	numberAll(m);
	assert.deepEqual(validate(m).filter((p) => p.level === "error" && p.table === "systems"), []);
	const d = decode(encode(m));
	assert.deepEqual(d.systems.map((s) => s.fields), [t3.fields, p25.fields]);
});

test("Systems tab: the recommended ham system's button is there for a type that has one", () => {
	const m = empty();
	const h = (tag, attrs, ...children) => ({ tag, attrs, children });
	const labels = [];
	const page = systemsTab({ model: m, t: (key) => key, h, button: (text, fn, opts) => (labels.push([text, !!opts?.disabled]), h("button")) }, null);
	assert.ok(page);
	// the type list starts on Tier III, which has one
	assert.deepEqual(labels.find(([x]) => x === "editor.modes.add_ham"), ["editor.modes.add_ham", false]);
});
