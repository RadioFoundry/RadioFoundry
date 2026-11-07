// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's record forms (static/js/editor/views.js): what a field shows and what typing into it stores.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

import { bulkColumns, channelForm, columns } from "../static/js/editor/views.js";
import { empty } from "../static/js/codeplug/model.js";
import { Grid } from "../static/js/editor/grid.js";

const t = (k) => (k === "editor.tot.default" ? "Radio Default" : k);
const field = (key) => channelForm(empty(), t).find((f) => f.key === key);

test("channel TX timeout: 0 shows as the radio default, the rest in the 15 s steps the record stores", () => {
	const f = field("tot");
	assert.equal(f.format(0), "Radio Default");
	assert.equal(f.format(180), "180");
	const typed = (s) => {
		const r = { tot: 99 };
		return f.parse(r, "tot", s) ?? r.tot;
	};
	for (const s of ["", " ", "0", "Radio Default", "radio default", "default"]) assert.equal(typed(s), 0, JSON.stringify(s));
	assert.deepEqual(["1", "15", "16", "180", "3825"].map(typed), [15, 15, 30, 180, 3825]);
	for (const s of ["3826", "-5", "1.5", "3 min", "off"]) assert.equal(typed(s), "editor.err.tot", s);
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
	assert.deepEqual(["121.5", "462.5625"].map((v) => [txField.disabled(null, () => v), pair.disabled(null, () => v)]), [[true, true], [false, false]]);
	const note = channelForm(empty(), t).find((f) => f.key === "band");
	assert.deepEqual(["121.5", "88.1", "462.5625", "x"].map((v) => note.text(() => v)), ["editor.band.air", "editor.band.bcast", "", ""]);
	// the form's RX Only box follows the RX frequency as it is typed
	const rxOnly = field("rxOnly");
	assert.deepEqual(["121.5", "88.1", "462.5625", "x"].map((v) => rxOnly.implied(() => v)), [true, true, false, false]);
});

test("the channel table shows the mode the radio uses in a receive-only band", () => {
	const mode = columns("channels", empty(), t).find((c) => c.key === "mode");
	assert.deepEqual([[118100000, "fm"], [98500000, "fm"], [146520000, "fm"], [118100000, "dmr"]].map(([rxHz, m]) => mode.format(m, { rxHz, mode: m })),
		["editor.mode.air", "editor.mode.bcast", "FM", "DMR"]);
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
