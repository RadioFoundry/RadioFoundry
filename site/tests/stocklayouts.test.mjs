// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Every codec layout of the stock files the flasher supports, against rfoundry (fixtures.py stock_layouts): one
// synthetic stock file per layout, extracted, placed and planned in JavaScript as stockfw.py and plan.py do it. A
// layout the flasher places anywhere else than the Python, or with another descriptor, fails here.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, sha256, u32le } from "../static/js/flasher/bytes.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import * as vendorfile from "../static/js/flasher/vendorfile.js";
import * as openfw from "../static/js/flasher/openfw.js";
import { moveWindow } from "../static/js/flasher/codecmap.js";
import { makePlan, MERGE } from "../static/js/flasher/plan.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const fx = json("fixtures.json");
const fb = json("flash-boards.json");
const image = bin("image.bin");
const L = fx.layouts;

test("every placed layout has a fixture, each layout once", () => {
	const reals = Object.entries(fb.profiles).filter(([, p]) => p.placement);
	assert.ok(L.files.length > 0);
	const covered = new Set(L.files.flatMap((f) => stockfw.sameLayout(fb.profiles[f.real_sha256], fb.profiles)));
	for (const [sha, p] of reals) assert.ok(covered.has(p), `${p.name} (${sha.slice(0, 8)})`);
	// a layout is where the codec sits, stock and placed: addresses and lengths (the hashes are of random bytes)
	const where = (f) => JSON.stringify([fb.profiles[f.real_sha256].board, fb.profiles[f.real_sha256].windows.map(([a, n]) => [a, n]),
		f.placed.slice(0, -1).map(([a, , n]) => [a, n])]);
	assert.equal(new Set(L.files.map(where)).size, L.files.length);
});

for (const f of L.files) {
	const real = fb.profiles[f.real_sha256];
	test(`${real.board} ${f.versions.join(", ")}: extract, place and merge as rfoundry`, async () => {
		const profiles = { [f.stock_sha256]: f.profile };
		const { profile, segments } = await stockfw.extract(bin(f.file), fromHex(f.key), fb, profiles, real.board);
		// the same windows, bytes and lengths as stockfw.extract
		assert.deepEqual(await Promise.all(segments.map(async ([a, d]) => [a, await sha256(d), d.length])), f.placed);
		// the descriptor: tag, code window and table windows with their CRCs, as the real profile says
		const [da, desc] = segments.at(-1);
		assert.equal(da, fb.descriptor.addr);
		const w = (i) => u32le(desc, 4 * i);
		assert.equal(w(0), fb.descriptor.magic);
		assert.equal(w(2), f.descriptor.tag);
		assert.equal(w(2), real.placement.tag);
		assert.deepEqual([w(3), w(4), w(5)], f.descriptor.code);
		assert.deepEqual([w(3), w(4)], [real.placement.code, real.windows[0][1]]);
		assert.deepEqual(Array.from({ length: w(6) }, (_, k) => [w(7 + 3 * k), w(8 + 3 * k), w(9 + 3 * k)]), f.descriptor.tables);
		assert.equal(f.descriptor.tables[0][0], real.placement.tables ?? real.windows[1][0]);
		assert.equal(f.descriptor.tables[0][1], real.placement.tables_len ?? real.windows[1][1]);
		// the merge writes this file's layout only, as plan.py plans it
		const wins = stockfw.layoutWindows(profile, fb);
		assert.deepEqual(wins, f.merge_windows);
		const board = fb.boards[real.board];
		assert.deepEqual(makePlan(board, [[board.app_base, image]], MERGE, segments, wins).describe(), f.merge);
	});
}

// A codec from another radio's file (file_radio: the MD-9600 codec an open-firmware install leaves on a DM-1701): the
// key is the file's own, never the radio's, and the codec is found in a flash read wherever another firmware put it
for (const f of L.files.filter((x) => fb.profiles[x.real_sha256].file_radio)) {
	const real = fb.profiles[f.real_sha256];
	test(`${real.board} ${f.versions.join(", ")}: codec only, and found moved in a flash read`, async () => {
		const profiles = { [f.stock_sha256]: f.profile };
		const stock = bin(f.file);
		assert.match(stockfw.codecOnly(real), /gives the codec only, not the bootloader key/);
		// the radio's key (any other) is not the file's: extract takes the file's own
		const { segments } = await stockfw.extract(stock, new Uint8Array(fb.key_len), fb, profiles, real.board);
		const segs = stockfw.parseContainer(stock, fb);
		const key = fromHex(f.key);
		const [code, tables] = f.profile.windows.slice(0, 2).map(([a, n]) => stockfw.decrypt(stock, stockfw.appFileOffset(segs, a, fb), n, key, fb));
		const m = f.profile.map, ta = f.profile.windows[1][0];
		const fbX = { ...fb, profiles };
		const board = fb.boards[real.board];
		// the synthetic map holds no table ref, so its tables are found at their stock address only (moved tables:
		// codecmap.test.mjs, with the shared vector)
		for (const [cr, tr, verbatim] of [[0x0807537C, ta, false], [0x0807537C, ta, true], [f.profile.windows[0][0], ta, false]]) {
			const [c2, t2] = await moveWindow(code, tables, m, cr, tr);
			const flash = new Uint8Array(0x100000).fill(0xFF);
			flash.set(c2, cr - 0x08000000);
			flash.set(verbatim ? tables : t2, tr - 0x08000000);
			const got = await stockfw.fromMoved(flash, fbX);
			assert.ok(got, `code ${cr.toString(16)} tables ${tr.toString(16)} verbatim ${verbatim}`);
			assert.deepEqual(got.segments, segments);
			const c = await openfw.codecFromImage(flash, 0x08000000, board, fbX);
			assert.equal(c.where, cr === f.profile.windows[0][0] && tr === ta && !verbatim ? "known" : `0x${cr.toString(16).toUpperCase().padStart(8, "0")}`);
			assert.deepEqual(c.segments, segments);
			flash[cr - 0x08000000 + 0x200] ^= 1;
			assert.equal(await stockfw.fromMoved(flash, fbX), null, "a changed byte");
		}
	});
}

test("a readout is named by its codec's layout, as rfoundry names it", () => {
	for (const [sha, p] of Object.entries(fb.profiles)) assert.equal(stockfw.layoutName(p, fb.profiles), L.names[sha], p.name);
	// files that put the same codec at the same addresses share one name, whichever of them the readout came from
	for (const f of L.files.filter((x) => x.versions.length > 1)) {
		const names = stockfw.sameLayout(fb.profiles[f.real_sha256], fb.profiles).map((p) => stockfw.layoutName(p, fb.profiles));
		assert.equal(new Set(names).size, 1);
		for (const v of f.versions) assert.ok(names[0].includes(v.split(" ")[0]), v);
	}
});

// Both readouts stockfw.fromFlash takes: a radio on its stock firmware (the codec windows plain at their stock
// addresses) and one running RadioFoundry (the windows placed, with the descriptor)
test("the page names a readout of a shared layout by the layout, not the first file", async () => {
	const shared = L.files.find((x) => x.versions.length > 1);
	if (!shared) return;
	const real = fb.profiles[shared.real_sha256];
	const stock = bin(shared.file);
	const key = fromHex(shared.key);
	const { segments } = await stockfw.extract(stock, key, fb, { [shared.stock_sha256]: shared.profile }, real.board);
	const segs = stockfw.parseContainer(stock, fb);
	const plain = shared.profile.windows.map(([a, n]) => [a, stockfw.decrypt(stock, stockfw.appFileOffset(segs, a, fb), n, key, fb)]);
	// the synthetic codec under every file of the layout, as the real files share theirs
	const group = stockfw.sameLayout(real, fb.profiles);
	const withP = { ...fb, profiles: Object.fromEntries(group.map((p, k) => [`${k}`.padStart(64, "0"), { ...shared.profile, name: p.name, version: p.version }])) };
	for (const [how, parts] of [["stock addresses", plain], ["placed", segments]]) {
		const flash = new Uint8Array(0x100000).fill(0xFF);
		for (const [a, d] of parts) flash.set(d, a - 0x08000000);
		const m = await vendorfile.identifyFile(flash, withP, real.board);
		assert.equal(m.kind, "readout", how);
		assert.equal(m.known, `${fb.boards[real.board].name} ${L.names[shared.real_sha256]}`, how);
		assert.ok(!m.known.includes(real.name), how);
	}
	// the stock readout really is the stock layout: no window at a placed address
	assert.ok(plain.every(([a], i) => i > 0 || a !== segments[0][0]));
});
