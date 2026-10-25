// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug editor's CSV tables (static/js/codeplug/csv.js): parsing, export and import round trips on cpsSim's
// populated codeplug, matching rows to existing records, references by name, partial files and problems.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { CodeplugImage, CODEPLUG_SPAN } from "../static/js/codeplug/image.js";
import { decode, encode, empty, newChannel, newContact } from "../static/js/codeplug/model.js";
import { parse, stringify, detect, exportTable, importTable, exportAll, importAll, ORDER } from "../static/js/codeplug/csv.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const sim = () => decode(CodeplugImage.from(new Uint8Array(readFileSync(join(DIR, "codeplug_sim.bin"))).slice(0, CODEPLUG_SPAN)));

test("parse: quotes, doubled quotes, newlines in a field, CRLF, BOM, blank lines", () => {
	const rows = parse('﻿a,"b,c","say ""hi"""\r\n\r\n1,"two\nlines",\n');
	assert.deepEqual(rows, [["a", "b,c", 'say "hi"'], ["1", "two\nlines", ""]]);
});

test("parse: semicolon and tab files", () => {
	assert.deepEqual(parse("Name;RX\nA;446,1\n"), [["Name", "RX"], ["A", "446,1"]]);
	assert.deepEqual(parse("Name\tRX\nA\t446.1"), [["Name", "RX"], ["A", "446.1"]]);
});

test("stringify quotes what needs it and parses back", () => {
	const rows = [["a", "b,c", 'q"', " lead", "x\ny", ""]];
	assert.equal(stringify(rows), 'a,"b,c","q""",\" lead\","x\ny",\r\n');
	assert.deepEqual(parse(stringify(rows)), rows);
});

test("every table: export, import into the same codeplug, nothing changes", () => {
	const m = sim();
	const before = encode(m).bytes;
	for (const table of ORDER) {
		const text = exportTable(m, table);
		assert.equal(detect(text), table, table);
		const r = importTable(m, text);
		assert.deepEqual(r.problems, [], table);
		assert.equal(r.removed, 0, table);
		assert.equal(r.added, 0, table);
	}
	assert.deepEqual(encode(m).bytes, before);
});

test("all tables into a blank codeplug give the same records", () => {
	const m = sim();
	const files = exportAll(m).map((f) => ({ name: f.name, text: f.data }));
	const n = empty();
	const { results, skipped } = importAll(n, files);
	assert.deepEqual(skipped, []);
	for (const r of results) assert.deepEqual(r.problems, [], r.name);
	const strip = (x) => JSON.parse(JSON.stringify(x, (k, v) => (k === "base" || k === "slot" || k === "iddb" ? undefined : v)));
	// references compare by what they point at
	const view = (mm) => ({
		channels: mm.channels.map((c) => ({ ...c, contact: c.contact?.name ?? null, rxList: c.rxList?.name ?? null, aprs: c.aprs?.name ?? null })),
		zones: mm.zones.map((z) => ({ name: z.name, channels: z.channels.map((c) => c.number) })),
		contacts: mm.contacts, rxLists: mm.rxLists.map((l) => ({ ...l, contacts: l.contacts.map((c) => c.number) })),
		dtmfContacts: mm.dtmfContacts, aprs: mm.aprs,
	});
	assert.deepEqual(strip(view(n)), strip(view(m)));
	// and the blank codeplug's image decodes to the same again
	assert.deepEqual(strip(view(decode(encode(n)))), strip(view(m)));
});

test("rows match records by number, then name; a renamed contact keeps its channels", () => {
	const m = empty();
	const tg = newContact({ number: 1, name: "TG 91", id: 91 });
	const local = newContact({ number: 2, name: "Local", id: 9 });
	m.contacts.push(tg, local);
	const ch = newChannel({ number: 1, name: "Hotspot", mode: "dmr", contact: tg });
	m.channels.push(ch);
	const r = importTable(m, "Number,Name,ID,Call Type\n1,Worldwide,91,Group\n,Local,9,Group\n,Parrot,9990,Private\n");
	assert.deepEqual([r.added, r.updated, r.removed], [1, 2, 0]);
	assert.equal(m.contacts[0], tg);
	assert.equal(tg.name, "Worldwide");
	assert.equal(ch.contact, tg);
	assert.equal(m.contacts[1], local);
	assert.equal(m.contacts[2].number, 3);
	assert.equal(m.contacts[2].type, "private");
});

test("records missing from the file are removed and their references cleared", () => {
	const m = empty();
	const a = newContact({ number: 1, name: "A", id: 1 }), b = newContact({ number: 2, name: "B", id: 2 });
	m.contacts.push(a, b);
	m.rxLists.push({ number: 1, name: "L", contacts: [a, b] });
	const ch = newChannel({ number: 1, name: "C", mode: "dmr", contact: b });
	m.channels.push(ch);
	const r = importTable(m, "Name,ID\nA,1\n");
	assert.equal(r.removed, 1);
	assert.equal(ch.contact, null);
	assert.deepEqual(m.rxLists[0].contacts, [a]);
});

test("a bad value in an existing record keeps the old one", () => {
	const m = empty();
	const ch = newChannel({ number: 1, name: "Rpt", rxHz: 444000000 });
	m.channels.push(ch);
	const r = importTable(m, "Number,Name,RX Frequency\n1,Rpt,abc\n");
	assert.equal(m.channels[0], ch);
	assert.equal(ch.rxHz, 444000000);
	assert.deepEqual(r.problems.map((p) => [p.code, !!p.skipped]), [["bad_freq", false]]);
});

test("a partial file changes only its columns", () => {
	const m = empty();
	const ch = newChannel({ number: 1, name: "Rpt", rxHz: 444000000, txHz: 449000000, rxTone: "100.0", power: 9 });
	m.channels.push(ch);
	importTable(m, "Name,RX Frequency\nRpt,444.1\n", "channels");
	assert.equal(ch.rxHz, 444100000);
	assert.equal(ch.txHz, 449000000);
	assert.equal(ch.rxTone, "100.0");
	assert.equal(ch.power, 9);
});

test("references by name, '#n' for duplicate names, and problems for unknown ones", () => {
	const m = empty();
	const c1 = newChannel({ number: 1, name: "Same" }), c2 = newChannel({ number: 2, name: "Same" }), c3 = newChannel({ number: 3, name: "Other" });
	m.channels.push(c1, c2, c3);
	m.zones.push({ name: "Z", channels: [c2, c3], slot: 0 });
	const text = exportTable(m, "zones");
	assert.match(text, /"?#2\|Other"?/);
	const r = importTable(m, 'Name,Channels\nZ,#2|other|Nope|Same\n');
	assert.deepEqual(m.zones[0].channels, [c2, c3]);
	assert.deepEqual(r.problems.map((p) => p.code), ["unknown_ref", "ambiguous_ref"]);
});

test("a name with the list separator is written as its number", () => {
	const m = empty();
	const a = newChannel({ number: 7, name: "A|B" });
	m.channels.push(a);
	m.zones.push({ name: "Z", channels: [a], slot: 0 });
	assert.match(exportTable(m, "zones"), /^Z,#7\r$/m);
	assert.deepEqual(importTable(m, exportTable(m, "zones")).problems, []);
	assert.deepEqual(m.zones[0].channels, [a]);
});

test("values: aliases, Yes/No, power names, tones, bad values reported with row and column", () => {
	const m = empty();
	const text = [
		"Channel Name;RX Freq;TX Freq;Mode;TX Power;Decode;Encode;CC;TS;RX Only;Bandwidth",
		"Simplex;446,00625;446,00625;fm;high;88,5;d23;;;yes;25",
		"Bad;abc;446.1;FM;Huge;12;D023N;;;maybe;20",
		";446.1;446.1;FM;;;;;;;",
	].join("\n");
	assert.equal(detect(text), "channels");
	const r = importTable(m, text);
	const [a] = m.channels;
	assert.equal(m.channels.length, 1);
	assert.deepEqual([a.rxHz, a.mode, a.power, a.rxTone, a.txTone, a.rxOnly, a.bw], [446006250, "fm", 9, "88.5", "D023N", true, 25]);
	// row 3 is new and its RX frequency is bad: reported and skipped; row 4 has no name
	assert.deepEqual(r.problems.map((p) => [p.row, p.column, p.code, !!p.skipped]), [
		[3, "RX Frequency (MHz)", "bad_freq", true], [3, "Power", "bad_value", false], [3, "RX Tone", "bad_tone", false],
		[3, "RX Only", "bad_bool", false], [3, "Bandwidth (kHz)", "bad_value", false], [4, "Name", "no_name", true],
	]);
	assert.deepEqual(r.unknown, []);
});

test("unknown headers are listed, and a file of no table is refused", () => {
	const m = empty();
	const r = importTable(m, "Name,ID,Favorite Color\nA,1,blue\n");
	assert.equal(r.table, "contacts");
	assert.deepEqual(r.unknown, ["Favorite Color"]);
	assert.equal(detect("Foo,Bar\n1,2\n"), null);
	assert.throws(() => importTable(m, "Foo,Bar\n1,2\n"), { code: "not_a_table" });
});
