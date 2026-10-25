// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher's codec map port (static/js/flasher/codecmap.js) against rfoundry's: the same synthetic vectors
// (tools/rfoundry/tests/codecmap_vectors.json, normalized by site/tests/fixtures.py) must give the same bytes.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, u32le } from "../static/js/flasher/bytes.js";
import * as cm from "../static/js/flasher/codecmap.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const V = JSON.parse(readFileSync(join(DIR, "codecmap.json"), "utf8"));
const CODE = fromHex(V.code);

test("moving the code gives rfoundry's bytes", async () => {
	const out = await cm.moveCode(CODE, V.map, V.new_run);
	assert.equal(toHex(out), V.moved);
	assert.equal(u32le(out, 0x40), V.new_run + 0x10);
	assert.equal(toHex(await cm.moveCode(CODE, V.map, V.map.code.run)), V.code, "the stock address changes nothing");
	assert.deepEqual(cm.relocations(V.map), [0x40, 0x84, 0xFC]);
});

test("another build or a stale word is refused", async () => {
	const other = CODE.slice();
	other[5] ^= 1;
	await assert.rejects(cm.moveCode(other, V.map, V.new_run), cm.MapError);
	const noHash = { ...V.map, code: { ...V.map.code, sha256: null } };
	const stale = CODE.slice();
	stale.fill(0, 0x40, 0x44);
	await assert.rejects(cm.moveCode(stale, noHash, V.new_run), cm.MapError);
});

test("moving tables gives rfoundry's bytes, and groups move together", () => {
	const moves = Object.fromEntries(Object.entries(V.table_moves).map(([k, v]) => [Number(k), v]));
	assert.equal(toHex(cm.moveTables(CODE, V.map, moves)), V.tables_moved);
	assert.equal(toHex(cm.moveTables(CODE, V.map, {})), V.code);
	assert.throws(() => cm.moveTables(CODE, V.map, { [0x18001600]: 0x18003000 }), cm.MapError);
	assert.equal(cm.byCodeSha({ a: V.map }, V.map.code.sha256)[0].profile, "test-csky");
});

test("Thumb-2: movw/movt self references and island calls move as rfoundry moves them", async () => {
	const T = V.thumb;
	const code = fromHex(T.code), win = fromHex(T.window);
	const delta = T.new_run - T.map.code.run;
	const out = await cm.moveCode(code, T.map, T.new_run);
	assert.equal(toHex(out), T.moved);
	assert.equal(cm.pairValue(out, T.map.self_refs[1]), T.map.self_refs[1].value + delta);
	assert.equal(toHex(cm.moveBranches(T.window_addr, win, T.map, T.new_run)), T.window_moved);
	assert.equal(cm.branchesIn(T.map, T.window_addr, win.length), 2);
	await assert.rejects(cm.moveCode(code, T.map, T.new_run + 2), cm.MapError);
	assert.throws(() => cm.relocations(T.map), cm.MapError);
	const b = new Uint8Array(4);
	for (const [at, to] of [[0x080D1A00, 0x080E3AF4], [0x080D1A04, 0x08074794], [0x080D1A10, 0x07F00000]]) {
		cm.putBl(b, 0, at, to);
		assert.equal(cm.blTarget(b, 0, at), to);
	}
	const stale = win.slice();
	stale.fill(0, 0x10, 0x14);
	assert.throws(() => cm.moveBranches(T.window_addr, stale, T.map, T.new_run), cm.MapError);
});
