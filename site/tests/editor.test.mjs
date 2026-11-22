// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's record forms (static/js/editor/views.js): what a field shows and what typing into it stores.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

import { bulkColumns, channelForm, columns, parseAreaKey } from "../static/js/editor/views.js";
import { empty } from "../static/js/codeplug/model.js";
import { KEYS } from "../static/js/codeplug/kv.js";
import { Grid } from "../static/js/editor/grid.js";

const t = (k) => (k === "editor.tot.default" ? "Radio Default" : k);
const field = (key) => channelForm(empty(), t).find((f) => f.key === key);

test("channel TX timeout: a list of the radio default and the 15 s steps the record stores", () => {
	const f = field("tot");
	assert.equal(f.type, "select");
	const opts = f.options({ tot: 0 });
	assert.deepEqual(opts[0], [0, "Radio Default"]);
	assert.deepEqual(opts.slice(1, 5).map(([v]) => v), [15, 30, 45, 60]);
	assert.equal(opts.length, 1 + 255);
	assert.deepEqual(opts.at(-1)[0], 3825);
	assert.deepEqual(opts.find(([v]) => v === 90), [90, "1:30 (90 s)"]);
	// the steps only: every import rounds up to them
	assert.ok(opts.every(([v]) => v % 15 === 0));
});

test("tone fields offer every standard CTCSS tone and DCS code, and still take any valid tone typed", () => {
	const tones = field("txTone").suggest();
	assert.equal(tones.length, 50 + 104 * 2);
	assert.ok(tones.includes("88.5") && tones.includes("D023N") && tones.includes("D754I"));
	assert.ok(field("rxTone").suggest().includes("254.1"));
	const col = columns("channels", empty(), t).find((c) => c.key === "rxTone");
	assert.equal(col.suggest().length, tones.length);
	const r = {};
	assert.equal(field("rxTone").parse(r, "rxTone", "69.4"), undefined);
	assert.equal(r.rxTone, "69.4");
});

test("an RX frequency in the air band or the broadcast band makes the channel RX only and names the band", () => {
	const freqs = channelForm(empty(), t).find((f) => f.key === "freqs").fields;
	const rx = freqs.find((f) => f.key === "rxHz");
	const typed = (s) => {
		const r = { rxHz: 446006250, txHz: 446006250, rxOnly: false };
		rx.parse(r, "rxHz", s);
		return r.rxOnly;
	};
	assert.deepEqual(["118.1", "98.5", "136.975", "145.5", "107.9"].map(typed), [true, true, true, false, true]);
	// TX goes to RX there, so no stray TX frequency stays behind; elsewhere TX is left as it was
	const tx = (s) => {
		const r = { rxHz: 446006250, txHz: 446006250, rxOnly: false };
		rx.parse(r, "rxHz", s);
		return r.txHz;
	};
	assert.deepEqual(["118.1", "145.5"].map(tx), [118100000, 446006250]);
	// the form's TX box and the RX/TX buttons are locked to RX while RX is in such a band
	const txField = freqs.find((f) => f.key === "txHz");
	const pair = channelForm(empty(), t).find((f) => f.key === "freqs");
	assert.equal(txField.follows, "rxHz");
	const off = () => false;
	assert.deepEqual(["121.5", "462.5625"].map((v) => [txField.disabled(null, () => v, off), pair.disabled(null, () => v, off)]), [[true, true], [false, false]]);
	// RX Only ticked outside those bands locks TX too (and the TX tone and power), but TX doesn't follow RX there
	const on = (k) => k === "rxOnly";
	const form = channelForm(empty(), t);
	assert.deepEqual(["power", "tot", "txTone", "vox", "toneBurst"].map((k) => form.find((f) => f.key === k).disabled(null, () => "462.5625", on)), [true, true, true, true, true]);
	assert.deepEqual(["power", "tot", "txTone", "vox", "toneBurst"].map((k) => form.find((f) => f.key === k).disabled(null, () => "462.5625", off)), [false, false, false, false, false]);
	assert.deepEqual([txField.disabled(null, () => "462.5625", on), txField.followsIf(() => "462.5625"), txField.followsIf(() => "121.5")], [true, false, true]);
	const note = channelForm(empty(), t).find((f) => f.key === "band");
	assert.deepEqual(["121.5", "88.1", "462.5625", "x"].map((v) => note.text(() => v)), ["editor.band.air", "editor.band.bcast", "", ""]);
	// the form's RX Only box follows the RX frequency as it is typed
	const rxOnly = field("rxOnly");
	assert.deepEqual(["121.5", "88.1", "462.5625", "x"].map((v) => rxOnly.implied(() => v)), [true, true, false, false]);
});

test("the channel table grays and locks TX, TX tone and power on an RX Only channel (#924)", () => {
	const cols = columns("channels", empty(), t);
	const na = (key, r) => cols.find((c) => c.key === key).readOnly(r);
	const ch = (rxOnly) => ({ rxHz: 146520000, txHz: 146520000, mode: "fm", rxOnly });
	assert.deepEqual(["txHz", "power", "txTone"].map((k) => [na(k, ch(true)), na(k, ch(false))]), [[true, false], [true, false], [true, false]]);
	// and blank there: no TX frequency, power or tone that the channel never uses
	const shown = (key, r) => {
		const c = cols.find((x) => x.key === key);
		return c.format(r[key], r);
	};
	assert.deepEqual(["txHz", "power"].map((k) => shown(k, ch(true))), ["", ""]);
	assert.deepEqual(shown("txHz", ch(false)), "146.520");
});

test("a TX frequency typed in the air or broadcast band is refused and the channel is left as it was (#924)", () => {
	const say = (k, p) => (k === "editor.err.tx_listen" ? `${p.mhz} MHz is not a supported transmit frequency.` : k);
	const ch = () => ({ rxHz: 146520000, txHz: 146520000, mode: "fm", rxOnly: false });
	const col = (key) => columns("channels", empty(), say).find((c) => c.key === key);
	const formTx = channelForm(empty(), say).find((f) => f.key === "freqs").fields.find((f) => f.key === "txHz");
	// the table and the form refuse it alike, with the same warning, and keep the old TX frequency and RX Only
	for (const typeTx of [(r, v) => col("txHz").set(r, v), (r, v) => formTx.parse(r, "txHz", v)]) {
		for (const [v, mhz] of [["121.5", "121.500"], ["98.1", "98.100"]]) {
			const r = ch();
			assert.equal(typeTx(r, v), `${mhz} MHz is not a supported transmit frequency.`);
			assert.deepEqual([r.txHz, r.rxOnly], [146520000, false]);
		}
		// a transmit frequency is taken as before
		const r = ch();
		assert.equal(typeTx(r, "146.55"), undefined);
		assert.deepEqual([r.txHz, r.rxOnly], [146550000, false]);
		// a TX frequency already there (an import) is kept when it isn't changed, so the form still saves
		const imported = { ...ch(), txHz: 121500000, rxOnly: true };
		assert.equal(typeTx(imported, "121.5"), undefined);
		assert.equal(imported.txHz, 121500000);
	}
	// an RX frequency in a listen band still makes the channel RX only with TX on RX
	const r = ch();
	assert.equal(col("rxHz").set(r, "121.5"), undefined);
	assert.deepEqual([r.rxHz, r.txHz, r.rxOnly], [121500000, 121500000, true]);
	// and the form's TX, following RX there, saves
	const w = ch();
	formTx.parse(w, "txHz", "121.5");
	assert.equal(w.txHz, 146520000);
	const rx = channelForm(empty(), say).find((f) => f.key === "freqs").fields.find((f) => f.key === "rxHz");
	rx.parse(w, "rxHz", "121.5");
	assert.equal(formTx.parse(w, "txHz", "121.5"), undefined);
	// an imported TX frequency in a listen band stays shown and open in the table, so it can be corrected there
	const tx = col("txHz");
	const imp = { ...ch(), txHz: 121500000, rxOnly: true };
	assert.deepEqual([tx.readOnly(imp), tx.format(imp.txHz, imp)], [false, "121.500"]);
	assert.equal(tx.set(imp, "146.55"), undefined);
	assert.deepEqual([imp.txHz, tx.readOnly(imp), tx.format(imp.txHz, imp)], [146550000, true, ""]);
});

test("the channel table shows the mode the radio uses in a receive-only band", () => {
	const mode = columns("channels", empty(), t).find((c) => c.key === "mode");
	assert.deepEqual([[118100000, "fm"], [98500000, "fm"], [146520000, "fm"], [118100000, "dmr"]].map(([rxHz, m]) => mode.format(m, { rxHz, mode: m })),
		["editor.mode.air", "editor.mode.bcast", "FM", "DMR"]);
});

test("Tier III channels expose the talk target and RX group list in the grid and form", () => {
	const model = empty();
	const contact = { number: 1, name: "TestGroup", id: 147862, type: "group" };
	const rxList = { number: 1, name: "Test groups", contacts: [contact] };
	model.contacts.push(contact);
	model.rxLists.push(rxList);
	const channel = { kind: 1, mode: "dmr" };
	const grid = columns("channels", model, t);
	const form = channelForm(model, t);
	for (const [key, entry] of [["contact", contact], ["rxList", rxList]]) {
		const column = grid.find((c) => c.key === key);
		const selector = form.find((f) => f.key === key);
		assert.equal(column.readOnly(channel), false);
		assert.equal(selector.show(channel), true);
		column.set(channel, entry);
		assert.equal(channel[key], entry);
		assert.equal(column.format(entry, channel), entry.name);
	}
	for (const key of ["colorCode", "timeslot"]) {
		assert.equal(grid.find((c) => c.key === key).readOnly(channel), true);
		assert.equal(form.find((f) => f.key === key).show(channel), false);
	}
});

test("Bulk Edit sets no names or numbers, and has nothing to set on zones and RX lists", () => {
	const keys = (table) => bulkColumns(table, empty(), t).map((c) => c.key);
	assert.deepEqual([keys("zones"), keys("rxLists")], [[], []]);
	assert.ok(keys("channels").includes("power"));
	assert.ok(!keys("channels").includes("name") && !keys("contacts").includes("number"));
	assert.ok(keys("contacts").includes("type"));
});

test("the table filter and sort take every column's text, numbers too", () => {
	// a bare Grid: shown() filters and sorts without the DOM
	const m = empty();
	m.contacts.push({ number: 12, name: "Local", id: 9, type: "group", timeslot: 0 }, { number: 3, name: "World", id: 91, type: "group", timeslot: 0 });
	const g = Object.assign(Object.create(Grid.prototype), { columns: columns("contacts", m, t), rows: m.contacts, filter: "", sort: null });
	for (const c of g.columns) for (const r of g.rows) assert.equal(typeof g.text(c, r), "string", c.key);
	g.filter = "12";
	assert.deepEqual(g.shown().map((r) => r.name), ["Local"]);
	g.filter = "";
	g.sort = { key: "number", dir: 1 };
	assert.deepEqual(g.shown().map((r) => r.number), [3, 12]);
});

test("Radio tab ARS server and IP gateway: empty is an absent key, 0 is off", () => {
	const typed = (s) => parseAreaKey(KEYS[0], s, (k, p) => `${k} ${p.min}-${p.max}`);
	for (const s of ["", " ", null]) assert.deepEqual(typed(s), { value: null }, JSON.stringify(s));
	assert.deepEqual(typed("0"), { value: 0 });
	assert.deepEqual(typed(" 3100500 "), { value: 3100500 });
	assert.deepEqual(typed("16776415"), { value: 16776415 });
	for (const s of ["16776416", "-1", "1.5", "abc"]) assert.deepEqual(typed(s), { error: "editor.err.range 0-16776415" }, s);
});
