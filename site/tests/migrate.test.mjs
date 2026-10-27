// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bringing the original firmware's channels over after an install (flasher/migrate.js): telling a stock codeplug from
// RadioFoundry's, converting it, the sectors the write changes, and the read-back check. Fixtures from the spec
// (stockfix.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";
import { stockLayoutOf, readSpan, convertStock, imageCounts, migrationSectors, checkMigration } from "../static/js/flasher/migrate.js";
import { CODEPLUG_SPAN } from "../static/js/flasher/backup.js";
import { STOCK } from "../static/js/codeplug/stock_tyt.js";
import { Stock, uv380 } from "./stockfix.mjs";

// The two stock families by what tells them apart here: one whose tables reach past RadioFoundry's codeplug span, and the
// 256 KB one, which shares the chip's first sectors with RadioFoundry's codeplug
const WIDE = Object.keys(STOCK).find((k) => STOCK[k].size > CODEPLUG_SPAN);
const SMALL = Object.keys(STOCK).find((k) => STOCK[k].size === 0x40000);

// The radio after a write of the given sectors
function written(radio, sectors) {
	const out = radio.slice();
	for (const [a, d] of sectors) out.set(d, a);
	return out;
}

test("a stock codeplug is recognized and read in full; a RadioFoundry one is left alone", () => {
	const s = uv380();
	assert.equal(stockLayoutOf(s.b), WIDE);
	assert.equal(readSpan(WIDE), STOCK[WIDE].size);	// the contacts lie past RadioFoundry's codeplug span
	assert.equal(readSpan(SMALL), CODEPLUG_SPAN);
	assert.equal(readSpan(null), CODEPLUG_SPAN);
	const { image } = convertStock(s.b, WIDE);
	assert.equal(stockLayoutOf(image), null);	// after the conversion, the next install finds nothing to do
	assert.equal(stockLayoutOf(new Uint8Array(CODEPLUG_SPAN).fill(0xFF)), null);
});

test("the conversion counts what it made, and the image holds that", () => {
	const s = uv380();
	const c = convertStock(s.b, WIDE);
	assert.equal(c.image.length, CODEPLUG_SPAN);
	assert.deepEqual(c.counts, { channels: 3, contacts: 4, rxLists: 2, zones: 3 });
	assert.deepEqual(imageCounts(c.image), c.counts);
	assert.ok(c.notes.some((x) => x.code === "scan_lists"));
	assert.throws(() => convertStock(s.b, SMALL));
	assert.throws(() => convertStock(s.b, null));
});

test("the write changes codeplug sectors only, and the read-back check passes", () => {
	const s = uv380();
	const radio = s.b.slice(0, readSpan(null));
	const c = convertStock(s.b, WIDE);
	const sectors = migrationSectors(c.image, radio);
	assert.ok(sectors.length > 0);
	for (const [a] of sectors) assert.ok(a < 0x10000 || a >= 0x20000, `sector 0x${a.toString(16)} is RadioFoundry's own area`);
	const check = checkMigration(sectors, written(radio, sectors), c.counts);
	assert.equal(check.ok, true);
	assert.deepEqual(check.differ, []);
	assert.deepEqual(check.have, c.counts);
});

test("a sector the radio did not take fails the check", () => {
	const s = uv380();
	const radio = s.b.slice(0, readSpan(null));
	const c = convertStock(s.b, WIDE);
	const sectors = migrationSectors(c.image, radio);
	const check = checkMigration(sectors, written(radio, sectors.slice(1)), c.counts);
	assert.equal(check.ok, false);
	assert.deepEqual(check.differ, [sectors[0][0]]);
	// nothing written at all: the radio still holds the stock codeplug
	assert.equal(checkMigration(sectors, radio, c.counts).ok, false);
});

test("a 256 KB stock codeplug, which shares the chip's first sectors with RadioFoundry's codeplug", () => {
	const m = new Stock(SMALL);
	m.general({ name: "N0CALL", id: 3100123 });
	m.contact(1, { name: "TG 91", id: 91, type: "group" });
	m.channel(1, { name: "Simplex", mode: "fm", rx: 146520000 });
	m.channel(2, { name: "TG 91", mode: "dmr", rx: 439000000, tx: 431400000, cc: 1, ts: 1, contact: 1 });
	m.zone(1, "Home", [1, 2]);
	const radio = new Uint8Array(CODEPLUG_SPAN).fill(0xFF);
	radio.set(m.b);
	assert.equal(stockLayoutOf(radio), SMALL);
	const c = convertStock(radio, SMALL);
	assert.deepEqual(c.counts, { channels: 2, contacts: 1, rxLists: 0, zones: 1 });
	const sectors = migrationSectors(c.image, radio);
	assert.equal(checkMigration(sectors, written(radio, sectors), c.counts).ok, true);
});
