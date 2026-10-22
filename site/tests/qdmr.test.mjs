// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// qdmr YAML in the codeplug editor: the YAML subset (static/js/codeplug/yaml.js), the mapping to the editing model
// (static/js/codeplug/qdmr.js), round trips on a file written the way qdmr 0.15 writes one and on cpsSim's populated
// codeplug, the Tools page converters, and, when dmrconf is installed, qdmr itself encoding what the editor exports.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { execFileSync, spawnSync } from "node:child_process";

import { CodeplugImage, CODEPLUG_SPAN } from "../static/js/codeplug/image.js";
import { decode, encode, empty } from "../static/js/codeplug/model.js";
import { parse, stringify, flow, Tag, YamlError } from "../static/js/codeplug/yaml.js";
import { importQdmr, exportQdmr, QdmrError, parseFrequency, parseDuration, fromLocator, toLocator } from "../static/js/codeplug/qdmr.js";
import { convertersFor } from "../static/js/flasher/codeplug.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const sim = () => decode(CodeplugImage.from(new Uint8Array(readFileSync(join(DIR, "codeplug_sim.bin"))).slice(0, CODEPLUG_SPAN)));

// A codeplug as qdmr 0.15 writes one (made-up names and IDs): flow maps for contacts, block maps for channels,
// units on frequencies and times, !default tags, ~ for no tone, document markers
const SAMPLE = `---
version: 0.15.1
settings:
  power: High
  defaultID: id1
  boot:
    display: Logo
    message1: Test Radio
    message2: N0CALL
  audio:
    squelch: 3
    vox: off
  gnss:
    fixedPositionEnabled: false
    fixedPosition: ""
  dmr:
    privateCallHangTime: 5 s
    groupCallHangTime: 3 s
radioIDs:
  - dmr: {id: id1, name: N0CALL, number: 1234567}
  - dmr: {id: id2, name: Club, number: 7654321}
contacts:
  - dmr: {id: cont1, name: TG 9, ring: false, type: GroupCall, number: 9, openGD77: {timeSlotOverride: None}}
  - dmr: {id: cont2, name: Echo Test, ring: false, type: PrivateCall, number: 9990, openGD77: {timeSlotOverride: TS2}}
  - dmr: {id: cont3, name: 201, ring: false, type: GroupCall, number: 201, openGD77: {timeSlotOverride: None}}
  - dmr: {id: cont4, name: All Call, ring: false, type: AllCall, number: 16777215}
  - dtmf: {id: cont5, name: Gate, number: "123*#A"}
groupLists:
  - {id: grp1, name: Local, contacts: [cont1, cont3]}
channels:
  - dmr:
      id: ch1
      name: Repeater TS1
      rxFrequency: 439.5625 MHz
      txFrequency: 431.9625 MHz
      timeout: 3 min
      rxOnly: false
      vox: off
      openGD77:
        scanZoneSkip: true
        scanAllSkip: false
        beep: true
        powerSave: false
        location: EM73tt
        talkerAliasTS1: Text
        talkerAliasTS2: None
      admit: Always
      colorCode: 7
      timeSlot: TS2
      radioId: id2
      groupList: grp1
      contact: cont1
      roaming: !default ""
      extended:
        talkaround: true
        privateCallConfirm: false
      power: Max
  - fm:
      id: ch2
      name: "Simplex: 446"
      rxFrequency: 446.00625 MHz
      txFrequency: 446.00625 MHz
      timeout: infinite
      rxOnly: false
      vox: 3
      openGD77: {scanZoneSkip: false, scanAllSkip: true, beep: false, powerSave: true, location: "", talkerAliasTS1: None, talkerAliasTS2: None}
      squelch: 5
      admit: Always
      rxTone: {ctcss: 67 Hz}
      txTone: {dcs: -754}
      bandwidth: Wide
      aprs: aprs1
      power: Low
  - fm:
      id: ch3
      name: APRS 2m   # a comment
      rxFrequency: 144.39 MHz
      txFrequency: 144.39 MHz
      timeout: 45 s
      squelch: !default ""
      rxTone: ~
      txTone: {ctcss: 88.5}
      bandwidth: Narrow
      power: !default ""
zones:
  - id: zone1
    name: Home
    A: [ch1, ch2]
    B: []
  - id: zone2
    name: Both
    A: [ch2]
    B:
      - ch3
positioning:
  - aprs:
      id: aprs1
      name: Car
      period: 300 s
      revert: ch3
      icon: Jogger
      message: 'It''s me'
      opengd77: {transmitQSY: true, baud: 300}
      destination: APN000-0
      source: N0CALL-9
      path: [WIDE1-1, WIDE2-2, WIDE3-3]
  - dmr: {id: gps1, name: BM, period: 300, contact: cont1}
scanLists:
  - {id: scan1, name: All, channels: [ch1]}
commercial:
  encryptionKeys:
    []
...
`;

// --- YAML ---

test("yaml: block and flow collections, scalars, tags, comments, markers", () => {
	const d = parse(SAMPLE);
	assert.equal(d.version, "0.15.1");
	assert.equal(d.settings.boot.message1, "Test Radio");
	assert.equal(d.settings.audio.vox, "off");
	assert.equal(d.settings.gnss.fixedPosition, "");
	assert.deepEqual(d.radioIDs[0], { dmr: { id: "id1", name: "N0CALL", number: 1234567 } });
	assert.equal(d.contacts[2].dmr.name, 201);
	assert.deepEqual(d.contacts[1].dmr.openGD77, { timeSlotOverride: "TS2" });
	assert.equal(d.channels[1].fm.name, "Simplex: 446");
	assert.equal(d.channels[2].fm.name, "APRS 2m");
	assert.equal(d.channels[2].fm.rxTone, null);
	assert.ok(d.channels[0].dmr.roaming instanceof Tag);
	assert.deepEqual([d.channels[0].dmr.roaming.tag, d.channels[0].dmr.roaming.value], ["!default", ""]);
	assert.deepEqual(d.zones[1].B, ["ch3"]);
	assert.deepEqual(d.zones[0].B, []);
	assert.equal(d.positioning[0].aprs.message, "It's me");
	assert.deepEqual(d.commercial.encryptionKeys, []);
});

test("yaml: scalars resolve as the core schema says; quotes keep strings", () => {
	const d = parse('a: 1\nb: -2.5\nc: true\nd: ~\ne: null\nf: "1"\ng: \'x # y\'\nh: x # y\ni: "tab\\there \\u00e9"\nj: 0x1F\nk: 1e3\n');
	assert.deepEqual(d, { a: 1, b: -2.5, c: true, d: null, e: null, f: "1", g: "x # y", h: "x", i: "tab\there é", j: 31, k: 1000 });
});

test("yaml: block scalars, nested lists, multi-line flow, a list at the key's indent", () => {
	const d = parse("lit: |\n  one\n  two\nfold: >-\n  a\n  b\nl:\n- - 1\n  - 2\n- x\nf: [a,\n  b, {c: d}]\n");
	assert.deepEqual(d, { lit: "one\ntwo\n", fold: "a b", l: [[1, 2], "x"], f: ["a", "b", { c: "d" }] });
});

test("yaml: refuses what qdmr files don't use", () => {
	assert.throws(() => parse("a: &x 1\nb: *x\n"), YamlError);
	assert.throws(() => parse("a: 1\na: 2\n"), YamlError);
	assert.throws(() => parse("a: [1, 2\n"), YamlError);
	assert.throws(() => parse("a: 1\n  b: 2\n"), YamlError);
	assert.throws(() => parse("---\na: 1\n---\nb: 2\n"), YamlError);
});

test("yaml: stringify writes qdmr's layout and parses back", () => {
	const v = {
		version: "0.15.1",
		list: [{ dmr: flow({ id: "c1", name: "TG 9", n: 9 }) }, { fm: { id: "ch1", tone: null, power: new Tag("!default", "") } }],
		refs: flow(["ch1", "ch2"]), empty: flow([]), odd: ["201", "a: b", " lead", "#x", "", "yes", "-1", "line\nbreak"],
	};
	const text = stringify(v);
	assert.match(text, /^ {2}- dmr: \{id: c1, name: TG 9, n: 9\}$/m);
	assert.match(text, /^ {2}- fm:\n {6}id: ch1\n {6}tone: ~\n {6}power: !default ""$/m);
	assert.match(text, /^refs: \[ch1, ch2\]$/m);
	assert.match(text, /^empty: \[\]$/m);
	const back = parse(text);
	assert.deepEqual(back.odd, v.odd);
	assert.deepEqual(back.refs, ["ch1", "ch2"]);
	assert.equal(back.list[1].fm.power.tag, "!default");
});

// --- Values ---

test("values: frequencies, durations, locators", () => {
	assert.equal(parseFrequency("439.5625 MHz"), 439562500);
	assert.equal(parseFrequency(446.00625), 446006250);
	assert.equal(parseFrequency("145500 kHz"), 145500000);
	assert.equal(parseFrequency("x"), null);
	assert.equal(parseDuration("3 min"), 180000);
	assert.equal(parseDuration("500 ms"), 500);
	assert.equal(parseDuration(45), 45000);
	assert.equal(parseDuration("infinite"), 0);
	assert.equal(parseDuration(new Tag("!default", "")), null);
	const p = fromLocator("JO62qm");
	assert.ok(Math.abs(p.lat - 52.52) < 0.03 && Math.abs(p.lon - 13.375) < 0.05, JSON.stringify(p));
	assert.equal(toLocator(p.lat, p.lon), "JO62qm");
	assert.equal(toLocator(-33.8688, 151.2093), "QF56od");
	assert.equal(fromLocator("nope"), null);
});

// --- Import ---

test("import: every table and setting qdmr's file holds", () => {
	const m = empty();
	const r = importQdmr(m, SAMPLE);
	assert.deepEqual(r.counts, { channels: 3, contacts: 4, rxLists: 1, dtmfContacts: 1, aprs: 1, zones: 3 });
	const codes = Object.fromEntries(r.notes.map((n) => [n.code, n.n]));
	assert.deepEqual(codes, { path_cut: 1, aprs_period: 1, dmr_positioning: 1, scan_lists: 1 });

	assert.deepEqual(m.general, { radioName: "N0CALL", dmrId: 1234567, groupHang: 6, privateHang: 10, bootLine1: "Test Radio", bootLine2: "N0CALL" });
	assert.deepEqual(m.contacts.map((c) => [c.number, c.name, c.id, c.type, c.timeslot]),
		[[1, "TG 9", 9, "group", 0], [2, "Echo Test", 9990, "private", 2], [3, "201", 201, "group", 0], [4, "All Call", 0xFFFFFF, "all", 0]]);
	assert.deepEqual(m.dtmfContacts.map((d) => [d.name, d.code]), [["Gate", "123*#A"]]);
	assert.deepEqual(m.rxLists[0].contacts, [m.contacts[0], m.contacts[2]]);

	const [c1, c2, c3] = m.channels;
	assert.deepEqual([c1.number, c1.mode, c1.rxHz, c1.txHz, c1.tot, c1.power, c1.colorCode, c1.timeslot], [1, "dmr", 439562500, 431962500, 180, 10, 7, 2]);
	assert.equal(c1.contact, m.contacts[0]);
	assert.equal(c1.rxList, m.rxLists[0]);
	assert.equal(c1.dmrId, 7654321);
	assert.deepEqual([c1.zoneSkip, c1.noBeep, c1.noEco, c1.aliasTs1, c1.roam, c1.dmo, c1.useLocation], [true, false, true, 2, true, true, true]);
	assert.ok(Math.abs(c1.lat - 33.81) < 0.03 && Math.abs(c1.lon + 84.375) < 0.05);
	assert.deepEqual([c2.mode, c2.name, c2.rxTone, c2.txTone, c2.bw, c2.vox, c2.squelch, c2.power, c2.allSkip, c2.noBeep, c2.tot],
		["fm", "Simplex: 446", "67.0", "D754I", 25, true, 10, 3, true, true, 0]);
	assert.equal(c2.aprs, m.aprs[0]);
	assert.deepEqual([c3.rxTone, c3.txTone, c3.bw, c3.squelch, c3.power, c3.tot, c3.vox], ["", "88.5", 12.5, 0, 0, 45, false]);

	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.map((c) => c.number)]), [["Home", [1, 2]], ["Both A", [2]], ["Both B", [3]]]);
	const a = m.aprs[0];
	assert.deepEqual([a.name, a.ssid, a.path1, a.path2, a.symTable, a.symCode, a.comment, a.baud300, a.qsy, a.txHz],
		["Car", 9, "WIDE1-1", "WIDE2-2", "/", "[", "It's me", true, true, 144390000]);
});

test("import: encodes and decodes to the same model, as one replacement of the tables", () => {
	const m = sim();
	importQdmr(m, SAMPLE);
	const again = decode(encode(m));
	assert.deepEqual(again.channels.map((c) => c.name), ["Repeater TS1", "Simplex: 446", "APRS 2m"]);
	assert.deepEqual(again.contacts.map((c) => c.name), ["TG 9", "Echo Test", "201", "All Call"]);
	assert.equal(again.zones.length, 3);
	assert.equal(again.aprs.length, 1);
	for (const v of again.vfo) if (v) assert.equal(v.contact, null);
});

test("import: errors and odd input", () => {
	assert.throws(() => importQdmr(empty(), "a: [1"), (e) => e instanceof QdmrError && e.code === "bad_yaml");
	assert.throws(() => importQdmr(empty(), "foo: 1\n"), (e) => e instanceof QdmrError && e.code === "not_qdmr");
	assert.throws(() => importQdmr(empty(), "- 1\n"), (e) => e.code === "not_qdmr");
	const m = empty();
	const r = importQdmr(m, `channels:
  - analog: {id: a, name: A very long channel name, rxFrequency: 146.52, txFrequency: 146.52, rxTone: {ctcss: 1}}
  - fm: {id: b, name: B, rxFrequency: nowhere}
  - m17: {id: c, name: C, rxFrequency: 439}
`);
	assert.equal(m.channels.length, 1);
	assert.equal(m.channels[0].name, "A very long chan");
	assert.equal(m.channels[0].rxHz, 146520000);
	const codes = Object.fromEntries(r.notes.map((n) => [n.code, n.n]));
	assert.deepEqual(codes, { shortened: 1, bad_tone: 1, bad_frequency: 1, channel_type: 1 });
});

// --- Export ---

test("export: model to YAML to model keeps every field qdmr has", () => {
	const m = empty();
	importQdmr(m, SAMPLE);
	const { text, notes } = exportQdmr(m);
	assert.deepEqual(notes, [{ code: "location_rounded", n: 1 }]);
	assert.match(text, /^---\nversion: 0\.15\.1\n/);
	assert.match(text, /^ {6}rxTone: \{ctcss: 67 Hz\}$/m);
	assert.match(text, /^ {6}txTone: \{dcs: -754\}$/m);
	assert.match(text, /^ {6}power: !default ""$/m);
	const m2 = empty();
	importQdmr(m2, text);
	const strip = (x) => JSON.parse(JSON.stringify(x, (k, v) => (k === "base" ? undefined : v && typeof v === "object" && "number" in v && k ? v.number ?? v.name : v)));
	assert.deepEqual(strip(m2.channels), strip(m.channels));
	assert.deepEqual(strip(m2.contacts), strip(m.contacts));
	assert.deepEqual(strip(m2.rxLists), strip(m.rxLists));
	assert.deepEqual(strip(m2.dtmfContacts), strip(m.dtmfContacts));
	assert.deepEqual(strip(m2.aprs), strip(m.aprs));
	assert.deepEqual(m2.zones.map((z) => [z.name, z.channels.map((c) => c.number)]), m.zones.map((z) => [z.name, z.channels.map((c) => c.number)]));
	assert.deepEqual(m2.general, m.general);
});

test("export: cpsSim's codeplug round-trips through YAML", () => {
	const m = sim();
	const { text, notes } = exportQdmr(m);
	const m2 = empty();
	importQdmr(m2, text);
	const keys = ["name", "rxHz", "txHz", "mode", "power", "tot", "rxTone", "txTone", "bw", "rxOnly", "allSkip", "zoneSkip", "vox", "colorCode", "timeslot", "dmrId", "noBeep", "noEco", "roam", "aliasTs1", "aliasTs2", "useLocation"];
	const pick = (c) => Object.fromEntries(keys.map((k) => [k, c[k]]));
	assert.equal(m2.channels.length, m.channels.length);
	m.channels.forEach((c, i) => {
		const want = pick(c), got = pick(m2.channels[i]);
		if (c.mode === "dmr") {
			want.dmo = c.dmo;
			got.dmo = m2.channels[i].dmo;
		} else {
			// FM channels have no DMR fields in qdmr's file; squelch goes through qdmr's 1-10 scale
			for (const k of ["colorCode", "timeslot", "dmrId", "roam"]) delete want[k], delete got[k];
			want.squelch = c.squelch ? Math.min(Math.max(Math.round(c.squelch / 2), 1), 10) * 2 : 0;
			got.squelch = m2.channels[i].squelch;
		}
		if (POWER_NAMED.has(c.power) === false) want.power = 0;
		assert.deepEqual(got, want, c.name);
	});
	assert.deepEqual(m2.contacts.map((c) => [c.name, c.id, c.type, c.timeslot]), m.contacts.map((c) => [c.name, c.type === "all" ? 0xFFFFFF : c.id, c.type, c.timeslot]));
	assert.deepEqual(m2.zones.map((z) => z.name), m.zones.map((z) => z.name));
	assert.ok(Array.isArray(notes));
});
const POWER_NAMED = new Set([0, 1, 3, 6, 9, 10]);

// --- Tools page converters ---

test("converters: qdmr YAML to an image and an image to YAML", async () => {
	const [toImg] = convertersFor("yaml");
	assert.equal(toImg.status, "ready");
	const r = await toImg.convert(new TextEncoder().encode(SAMPLE), "my codeplug.yaml");
	assert.equal(r.name, "my codeplug.bin");
	assert.equal(r.data.length, CODEPLUG_SPAN);
	const m = decode(CodeplugImage.from(r.data));
	assert.deepEqual(m.channels.map((c) => c.name), ["Repeater TS1", "Simplex: 446", "APRS 2m"]);
	await assert.rejects(toImg.convert(new TextEncoder().encode("x: 1\n"), "x.yaml"), /not a qdmr codeplug/);

	const toYaml = convertersFor("openfw-image").find((c) => c.id === "odr-to-yaml");
	assert.equal(toYaml.status, "ready");
	const y = await toYaml.convert(r.data, "radio.bin");
	assert.equal(y.name, "radio.yaml");
	assert.match(new TextDecoder().decode(y.data), /name: Repeater TS1/);
});

// --- qdmr itself, when installed ---

const DMRCONF = spawnSync("dmrconf", ["--version"], { encoding: "utf8" }).status === 0;

// The codeplug in a DfuSe file dmrconf encodes: each element at its wire address (= image address)
function fromDfu(d) {
	const img = CodeplugImage.blank();
	const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
	assert.equal(new TextDecoder().decode(d.subarray(0, 5)), "DfuSe");
	let o = 11;
	while (new TextDecoder().decode(d.subarray(o, o + 6)) === "Target") {
		const n = dv.getUint32(o + 270, true);
		o += 274;
		for (let i = 0; i < n; i++) {
			const a = dv.getUint32(o, true), len = dv.getUint32(o + 4, true);
			img.bytes.set(d.subarray(o + 8, o + 8 + len), a);
			o += 8 + len;
		}
	}
	return img;
}

test("dmrconf encodes the exported YAML to the same channels, contacts, lists and zones", { skip: !DMRCONF && "dmrconf is not installed" }, () => {
	const m = empty();
	importQdmr(m, SAMPLE);
	const dir = mkdtempSync(join(tmpdir(), "odr-qdmr-"));
	try {
		writeFileSync(join(dir, "c.yaml"), exportQdmr(m).text);
		execFileSync("dmrconf", ["encode", "-y", "-R", "openuv380", join(dir, "c.yaml"), join(dir, "c.dfu")], { stdio: "pipe" });
		const q = decode(fromDfu(new Uint8Array(readFileSync(join(dir, "c.dfu")))));
		const ours = decode(encode(m));
		// qdmr's encoder doesn't write squelch, hang times or APRS symbols as characters, and writes no TX contact
		// when a channel has an RX group list: compare the rest
		const keys = ["name", "rxHz", "txHz", "mode", "power", "tot", "rxTone", "txTone", "bw", "rxOnly", "allSkip", "zoneSkip", "vox", "colorCode", "timeslot", "dmrId", "noBeep", "noEco", "aliasTs1", "aliasTs2"];
		const pick = (c) => ({ ...Object.fromEntries(keys.map((k) => [k, c[k]])), rxList: c.rxList?.name ?? null, contact: c.rxList ? null : c.contact?.name ?? null });
		assert.deepEqual(q.channels.map(pick), ours.channels.map(pick));
		assert.deepEqual(q.contacts.map((c) => [c.name, c.id, c.type, c.timeslot]), ours.contacts.map((c) => [c.name, c.id, c.type, c.timeslot]));
		assert.deepEqual(q.rxLists.map((l) => [l.name, l.contacts.map((c) => c.name)]), ours.rxLists.map((l) => [l.name, l.contacts.map((c) => c.name)]));
		assert.deepEqual(q.zones.map((z) => [z.name, z.channels.map((c) => c.number)]), ours.zones.map((z) => [z.name, z.channels.map((c) => c.number)]));
		assert.deepEqual(q.dtmfContacts.map((d) => [d.name, d.code]), ours.dtmfContacts.map((d) => [d.name, d.code]));
		assert.deepEqual([q.general.radioName, q.general.dmrId, q.general.bootLine1, q.general.bootLine2],
			[ours.general.radioName, ours.general.dmrId, ours.general.bootLine1, ours.general.bootLine2]);
		assert.deepEqual(q.aprs.map((a) => [a.name, a.ssid, a.path1, a.path2, a.comment, a.baud300, a.qsy]),
			ours.aprs.map((a) => [a.name, a.ssid, a.path1, a.path2, a.comment, a.baud300, a.qsy]));
	} finally {
		rmSync(dir, { recursive: true, force: true });
	}
});
