// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bringing the original firmware's channels over after an install (flasher/migrate.js): telling a stock codeplug from
// RadioFoundry's, converting it, the sectors the write changes, and the read-back check. Fixtures from the spec
// (stockfix.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";
import { stockLayoutOf, stockSource, savedStock, channelsWanted, readSpan, convertStock, imageCounts, migrationSectors, checkMigration } from "../static/js/flasher/migrate.js";
import { VFOS } from "../static/js/codeplug/transport.js";
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
	const radio = s.b.slice(0, readSpan(null));
	assert.equal(stockLayoutOf(written(radio, migrationSectors(image, radio))), null);
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

test("the backup's read is converted: the claim erased the stock zones from the live radio", () => {
	// a 256 KB stock codeplug keeps its zones inside RadioFoundry's settings area, which the backup's last step claims
	const m = new Stock(SMALL);
	m.general({ name: "N0CALL", id: 3100123 });
	m.channel(1, { name: "Simplex", mode: "fm", rx: 146520000 });
	m.channel(2, { name: "Calling", mode: "fm", rx: 446000000 });
	m.zone(1, "Home", [1, 2]);
	m.zone(2, "Away", [2]);
	const backup = new Uint8Array(CODEPLUG_SPAN).fill(0xFF);
	backup.set(m.b);
	const live = backup.slice();
	live.fill(0xFF, 0x11000, 0x1B000);	// the settings area and journal, as the claim leaves them
	assert.equal(convertStock(live, SMALL).counts.zones, 0);	// what the install converted before the fix
	const src = stockSource(live, backup);
	assert.equal(src.from, "backup");
	assert.equal(src.layout, SMALL);
	assert.equal(convertStock(src.bytes, src.layout).counts.zones, 2);
	// no backup, or one without a stock codeplug: the live read
	assert.equal(stockSource(live, null).from, "radio");
	assert.equal(stockSource(live, new Uint8Array(CODEPLUG_SPAN).fill(0xFF)).from, "radio");
	// a backup too short for the family's tables is not used
	const s = uv380();
	assert.equal(stockSource(s.b, s.b.slice(0, CODEPLUG_SPAN)).from, "radio");
});

test("the write keeps the radio's bytes outside the codeplug's regions", () => {
	const s = uv380();
	const radio = s.b.slice(0, readSpan(null));
	radio.fill(0x5A, 0x604B, 0x6060);	// the radio's own settings block, inside the first region's sector
	radio.fill(0x3C, VFOS[0], VFOS[1]);	// the live VFOs, as a read shows them
	radio.fill(0x77, 0x50000, 0x50010);	// the ID database area
	const c = convertStock(s.b, WIDE);
	const sectors = migrationSectors(c.image, radio);
	const after = written(radio, sectors);
	assert.ok(after.subarray(0x604B, 0x6060).every((x) => x === 0x5A));
	assert.ok(after.subarray(VFOS[0], VFOS[1]).every((x) => x === 0x3C));
	assert.ok(after.subarray(0x50000, 0x50010).every((x) => x === 0x77));
	// the codeplug's regions end at 0xAEE60: the stock tables past them (this family's channels) stay as they were
	for (const [a] of sectors) assert.ok(a < 0xAF000, `sector 0x${a.toString(16)} is past the codeplug's regions`);
	assert.deepEqual(imageCounts(after), c.counts);
});

test("the check doesn't count the live VFOs a read shows", () => {
	const s = uv380();
	const radio = s.b.slice(0, readSpan(null));
	const c = convertStock(s.b, WIDE);
	const sectors = migrationSectors(c.image, radio);
	const back = written(radio, sectors);
	back.fill(0x11, VFOS[0], VFOS[1]);
	if (!sectors.some(([a]) => a === (VFOS[0] & ~0xFFF))) sectors.push([VFOS[0] & ~0xFFF, radio.slice(VFOS[0] & ~0xFFF, (VFOS[0] & ~0xFFF) + 4096)]);
	assert.equal(checkMigration(sectors, back, c.counts).ok, true);
	back[VFOS[0] - 1] ^= 0xFF;
	assert.equal(checkMigration(sectors, back, c.counts).ok, false);
});

test("the step reads the radio only where a stock codeplug can be", () => {
	const s = uv380();
	const none = new Uint8Array(readSpan(WIDE)).fill(0xFF);
	const spiSize = 0x1000000;
	assert.equal(channelsWanted({ before: "stock", spiSize, saved: null }), true);
	assert.equal(channelsWanted({ before: null, spiSize, saved: null }), true);
	assert.equal(channelsWanted({ before: "unknown", spiSize, saved: s.b }), true);
	// an update or a move from another open firmware: the codeplug is already the open-firmware one
	assert.equal(channelsWanted({ before: "rfoundry", spiSize, saved: null }), false);
	assert.equal(channelsWanted({ before: "openfw", spiSize, saved: null }), false);
	// no memory chip
	assert.equal(channelsWanted({ before: "stock", spiSize: 0, saved: null }), false);
	// a backup that read every family's tables and found none; one too short to tell doesn't decide
	assert.equal(channelsWanted({ before: "stock", spiSize, saved: none }), false);
	assert.equal(channelsWanted({ before: "stock", spiSize, saved: none.subarray(0, CODEPLUG_SPAN) }), true);
	assert.equal(savedStock(s.b), WIDE);
	assert.equal(savedStock(s.b.subarray(0, CODEPLUG_SPAN)), null);
	assert.equal(savedStock(none), null);
	assert.equal(savedStock(null), null);
});
