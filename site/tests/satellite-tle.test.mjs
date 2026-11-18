// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { buildTleBundle, DEFAULT_SELECTED_IDS, filterFmSatellites, MAX_SELECTED_PROFILES,
	parseTleCatalog, SATELLITES } from "../static/js/satellite-tle.js";

const SAMPLE = `ISS (ZARYA)\n1 25544U 98067A   26317.20662175  .00008559  00000+0  16132-3 0  9990\n2 25544  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594859\nSO-50\n1 27607U 02058C   26316.49281663  .00001582  00000+0  20311-3 0  9999\n2 27607  64.5539  30.0380 0068124 235.0518 124.4178 14.83333417286454\nAO-91\n1 43017U 17073E   26317.60524444  .00008151  00000+0  33970-3 0  9990\n2 43017  97.4443 181.7665 0144887 171.0350 189.3523 15.14807395487992\n`;

test("site satellite profiles match the firmware frequency table", async () => {
	const source = await readFile(new URL("../../core/satellite/frequency.c", import.meta.url), "utf8");
	const firmware = [...source.matchAll(/\{\s*(\d+)u,\s*"[^"]+",\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\s*\}/g)]
		.map((m) => m.slice(1).map(Number));
	assert.deepEqual(SATELLITES.map(({ id, rxHz, txHz, toneHz }) => [id, rxHz, txHz, toneHz]), firmware);
	assert.deepEqual(DEFAULT_SELECTED_IDS, [25544, 27607, 43017]);
});

test("AMSAT fetch failures distinguish a missing local API route", async () => {
	const source = await readFile(new URL("../static/js/tletool.js", import.meta.url), "utf8");
	assert.match(source, /response\.status === 404\s*\? t\("tools\.satellite\.status\.api_not_found"\)/);
	assert.match(source, /t\("tools\.satellite\.status\.fetch_failed", \{ status: response\.status \}\)/);
	assert.doesNotMatch(source, /throw new Error\(t\("tools\.satellite\.status\.fetch_failed"/);
});

test("only known FM satellites are included in the selectable catalog", () => {
	const records = [...SATELLITES.map(({ id, name }) => ({ catalog_id: id, name })),
		{ catalog_id: 99001, name: "Non-FM test object" }];
	assert.deepEqual(filterFmSatellites(records).map(({ catalog_id }) => catalog_id), SATELLITES.map(({ id }) => id));
});

test("parses a named TLE catalog and builds the selected JSON bundle", () => {
	const catalog = parseTleCatalog(`Daily bulletin\n\n${SAMPLE}`);
	const now = new Date("2026-11-14T00:00:00Z");
	const bundle = buildTleBundle(catalog, [43017, 25544], { now });
	assert.deepEqual(Object.keys(bundle), ["schema_version", "provider", "fetched_at", "profile_ids", "profiles", "records"]);
	assert.equal(bundle.schema_version, 3);
	assert.deepEqual(bundle.profile_ids, [25544, 43017]);
	assert.deepEqual(bundle.profiles.map(({ catalog_id }) => catalog_id), [25544, 43017]);
	assert.equal(bundle.fetched_at, "2026-11-14T00:00:00Z");
	assert.deepEqual(bundle.records.map((record) => record.catalog_id), [25544, 43017]);
	assert.equal(bundle.records[0].name, "ISS (ZARYA)");
	assert.equal(SATELLITES.find(({ id }) => id === 25544).name, "ISS");
});

test("rejects malformed, stale, and unselected catalog data", () => {
	const catalog = parseTleCatalog(SAMPLE);
	assert.throws(() => buildTleBundle(catalog, []), /Select at least one satellite/);
	assert.throws(() => buildTleBundle(catalog, [40908]), /missing selected IDs/);
	assert.throws(() => buildTleBundle(catalog, [...SATELLITES.slice(0, MAX_SELECTED_PROFILES + 1).map(({ id }) => id)],
		{ now: new Date("2026-11-14T00:00:00Z") }), /no more than 10/);
	const badChecksum = catalog.map((record) => ({ ...record }));
	badChecksum[0].line1 = badChecksum[0].line1.slice(0, 68) + (badChecksum[0].line1[68] === "0" ? "1" : "0");
	assert.throws(() => buildTleBundle(badChecksum, [25544], { now: new Date("2026-11-14T00:00:00Z") }), /checksum/);
	assert.throws(() => buildTleBundle(catalog, [25544], { now: new Date("2027-01-01T00:00:00Z") }), /30-day age limit/);
	assert.throws(() => parseTleCatalog("Name\n1 25544\n"), /incomplete TLE pair/);
});

test("accepts bulletin satellites with manually supplied frequency profiles", () => {
	const customText = `Manual satellite\n1 99001U 26001A   26317.20662175  .00008559  00000+0  16132-3 0  9998\n2 99001  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594858\n`;
	const catalog = parseTleCatalog(customText);
	const profile = { catalog_id: 99001, rx_hz: 145900000, tx_hz: 435100000, tx_tone_tenths: 885 };
	const bundle = buildTleBundle(catalog, [99001], {
		now: new Date("2026-11-14T00:00:00Z"), profiles: [profile],
	});
	assert.deepEqual(bundle.profiles, [profile]);
	assert.throws(() => buildTleBundle(catalog, [99001], { profiles: [] }), /enter valid RX/);
	assert.throws(() => buildTleBundle(catalog, [99001], {
		profiles: [{ ...profile, tx_hz: 1000000000 }],
	}), /enter valid RX/);
});