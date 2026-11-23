// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A codec moving to a new layout in the web flasher (docs/spec/codec-placement.md, "Moving to a new layout"): every board
// whose profiles name a "former" layout, against the simulated radio (fake.js) holding its codec where an earlier
// release put it. The codec is found there and moved (stockfw.js fromFlash, codecsave.js), the decision merges it from
// the copy read back, else the stock file, else refuses (decide.js), the bootloader route writes it where it goes now,
// and an install that stops halfway is finished from the same copy. The loader route's side is in f4stub.test.mjs.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { equal } from "../static/js/flasher/bytes.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import { makePlan, KEEP, MERGE } from "../static/js/flasher/plan.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { FakeRadio, simulation, randomBytes, rng } from "../static/js/flasher/fake.js";
import { decideCodec, DecisionError } from "../static/js/flasher/decide.js";
import { codecSegments, loadCodec, readCodec, saveCodec } from "../static/js/flasher/codecsave.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const fb = JSON.parse(readFileSync(join(DIR, "flash-boards.json"), "utf8"));
const BASE = 0x08000000;
const noWait = { wait: async () => {} };
// the boards whose codec moves: each names the ids its earlier layouts reported
const MOVING = Object.values(fb.boards).filter((b) => b.codec.former?.length).map((b) => b.id);

const mem = (radio) => ({ async readMem(a, n) { return radio.flash.slice(a - BASE, a - BASE + n); } });
const at = (radio, a, n) => radio.flash.subarray(a - BASE, a - BASE + n);

// A radio running RadioFoundry from an earlier release: the codec where that release put it
async function former(id) {
	const sim = await simulation(fb, id, 7);
	const radio = new FakeRadio(sim, { state: "rfoundry-former", spiSize: 0x200000 });
	return { sim, radio, formerId: sim.profile.former[0].codec_profile };
}

// An application image: a RAM stack pointer first (the bootloader checks it), then firmware-like bytes
function app(n) {
	const d = randomBytes(rng(5), n).map((b) => (b < 90 ? b : 0));
	d.set([0xFC, 0xFF, 0x01, 0x20], 0);
	return d;
}

test("the boards that move their codec: each names its earlier ids", () => {
	assert.ok(MOVING.length > 0);
	for (const id of MOVING) {
		const b = fb.boards[id];
		assert.ok(!b.codec.former.includes(b.codec.id), id);
		for (const p of Object.values(fb.profiles).filter((x) => x.board === id)) {
			assert.ok(p.former.every((f) => b.codec.former.includes(f.codec_profile)), p.name);
			// every layout's windows fit the descriptor and sit below it
			for (const f of stockfw.forms(p)) {
				const lay = stockfw.layoutWindows(p, fb, f);
				assert.ok(lay.slice(0, -1).every(([a, n]) => a + n <= fb.descriptor.addr), `${p.name} ${f.codec_profile}`);
				assert.ok(lay.length - 2 <= fb.descriptor.windows);
			}
		}
	}
});

test("a codec in an earlier layout is found, checked and moved to where it goes now", async () => {
	for (const id of MOVING) {
		const { sim, radio, formerId } = await former(id);
		assert.equal(radio.codecReport(), formerId, id);
		// a readout of the flash (a backup): placed again as this release places it
		const found = await stockfw.fromFlash(radio.flash, sim.fb);
		assert.equal(found.profile, sim.profile, id);
		assert.deepEqual(found.segments, sim.codecSegments, id);
		// over the debug link: the copy as the radio holds it, which the merge then moves
		const got = await readCodec(mem(radio), sim.board, sim.fb);
		assert.deepEqual(got.segments, sim.formerSegments.slice(0, -1), id);
		assert.deepEqual(await codecSegments(got.profile, got.segments, sim.fb), sim.codecSegments, id);
		// the stock windows are a layout too, moved the same way
		assert.deepEqual(await codecSegments(sim.profile, sim.stockSegments, sim.fb), sim.codecSegments, id);
		// one byte off in a window that moves: no copy, no codec found
		const [a] = sim.formerSegments[0];
		radio.flash[a - BASE + 0x40] ^= 1;
		assert.equal(await readCodec(mem(radio), sim.board, sim.fb), null, id);
		assert.equal(await stockfw.fromFlash(radio.flash, sim.fb), null, id);
	}
});

test("the current layout is checked by CRC-32 where the profile pins no SHA-256", async () => {
	for (const id of MOVING) {
		const sim = await simulation(fb, id, 7);
		assert.ok(stockfw.placedWindows(sim.profile).some(([, , sha]) => sha === null), id);
		const radio = new FakeRadio(sim, { state: "rfoundry", spiSize: 0x200000 });
		const got = await readCodec(mem(radio), sim.board, sim.fb);
		assert.deepEqual(await codecSegments(got.profile, got.segments, sim.fb), sim.codecSegments, id);
		const [a, d] = sim.codecSegments[1];
		radio.flash[a - BASE + d.length - 1] ^= 0x80;
		assert.equal(await readCodec(mem(radio), sim.board, sim.fb), null, `${id}: a changed table byte`);
		// a profile whose CRC doesn't match the codec it moves: refused, never written
		const bad = structuredClone(sim.profile);
		bad.placed.code[0] ^= 1;
		await assert.rejects(stockfw.place(bad, sim.stockSegments, sim.fb), stockfw.StockError);
	}
});

test("the decision for a codec in an earlier layout: the copy read back, else the stock file, else nothing is written", async () => {
	for (const id of MOVING) {
		const board = fb.boards[id];
		const m = { codec: { profile: board.codec.id } };
		const probe = { board: board.debuglink_names[0], codec: board.codec.former[0] };
		const saved = decideCodec(board, m, { probe, haveSaved: true, haveStock: true });
		assert.deepEqual([saved.mode, saved.source, saved.whyKey, saved.args.codec], [MERGE, "saved", "flash.codec.why.moved_saved", probe.codec]);
		const st = decideCodec(board, m, { probe, haveStock: true });
		assert.deepEqual([st.mode, st.source, st.whyKey], [MERGE, "stock", "flash.codec.why.moved_stock"]);
		assert.throws(() => decideCodec(board, m, { probe }), (e) => e instanceof DecisionError && e.key === "flash.codec.why_not.moved" && e.hint);
		// a copy only in the page (the browser keeps nothing): a move cut short could not be finished from it, so the
		// stock file, else nothing
		const unkept = decideCodec(board, m, { probe, haveSaved: true, savedKept: false, haveStock: true });
		assert.deepEqual([unkept.mode, unkept.source], [MERGE, "stock"]);
		assert.throws(() => decideCodec(board, m, { probe, haveSaved: true, savedKept: false }),
			(e) => e instanceof DecisionError && e.key === "flash.codec.why_not.moved_unkept" && e.hint);
		// keeping it is the user's choice, with the risk said: the build overwrites part of it
		const keep = decideCodec(board, m, { probe, requested: KEEP, haveSaved: true });
		assert.deepEqual([keep.mode, keep.riskKey, keep.expectCodec], [KEEP, "flash.codec.risk.no_voice", false]);
		assert.equal(decideCodec(board, m, { probe: { ...probe, codec: board.codec.id } }).mode, KEEP, "the current layout is kept");
	}
});

// The bootloader route with the copy read back before anything is erased: the codec where it goes now
async function bootloaderInstall(sim, radio, copy, img) {
	const codec = await codecSegments(copy.profile, copy.segments, sim.fb);
	const plan = makePlan(sim.board, [[sim.board.app_base, img]], MERGE, codec, stockfw.layoutWindows(copy.profile, sim.fb), { vectorLast: true });
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(plan, sim.key);
	assert.deepEqual(radio.violations, []);
}

test("the bootloader route moves the codec from the copy read back first", async () => {
	for (const id of MOVING) {
		const { sim, radio } = await former(id);
		const copy = await readCodec(mem(radio), sim.board, sim.fb);
		const img = app(0x60000);
		await bootloaderInstall(sim, radio, copy, img);
		assert.ok(equal(at(radio, sim.board.app_base, img.length), img), id);
		for (const [a, d] of sim.codecSegments) assert.ok(equal(at(radio, a, d.length), d), `${id} ${a}`);
		radio.powerOn();
		assert.equal(radio.codecReport(), sim.board.codec.id, id);
	}
});

test("an install that stops halfway through the move is finished from the same copy", async () => {
	for (const id of MOVING) {
		const { sim, radio } = await former(id);
		const copy = await readCodec(mem(radio), sim.board, sim.fb);	// saved before anything is erased
		const img = app(0x60000);
		// the first run stops halfway (the cable pulled): every sector erased and a few blocks written. The radio holds
		// neither layout whole; whatever it starts, the user brings it to its bootloader for the next run
		const codec = await codecSegments(copy.profile, copy.segments, sim.fb);
		const plan = makePlan(sim.board, [[sim.board.app_base, img]], MERGE, codec, stockfw.layoutWindows(copy.profile, sim.fb), { vectorLast: true });
		radio.enterBootloader();
		const t = radio.dfuTransport();
		const cut = { ...t, async out(...a) { if (radio.blocks >= 4) throw new Error("the device has been lost"); return t.out(...a); } };
		await assert.rejects(new Dfu(cut, noWait).run(plan, sim.key));
		radio.powerOn();
		assert.equal(radio.codecReport(), "absent", id);
		assert.equal(await readCodec(mem(radio), sim.board, sim.fb), null, `${id}: no codec left to read back`);
		// the next run can't ask the radio; the copy decides, then writes the codec where it goes now
		const d = decideCodec(sim.board, { codec: { profile: sim.board.codec.id } }, { haveSaved: true });
		assert.deepEqual([d.mode, d.source], [MERGE, "saved"]);
		await bootloaderInstall(sim, radio, copy, img);
		for (const [a, x] of sim.codecSegments) assert.ok(equal(at(radio, a, x.length), x), `${id} ${a}`);
		radio.powerOn();
		assert.equal(radio.codecReport(), sim.board.codec.id, id);
	}
});

test("the bootloader route erases the vector table's sector first and writes it last: a cut anywhere leaves nothing to start", async () => {
	for (const id of MOVING) {
		const { sim, radio } = await former(id);
		const copy = await readCodec(mem(radio), sim.board, sim.fb);
		const img = app(0x60000);
		const base = sim.board.app_base;
		const sec = sim.board.sectors.find((s) => s.addr === base);
		const codec = await codecSegments(copy.profile, copy.segments, sim.fb);
		const plan = makePlan(sim.board, [[base, img]], MERGE, codec, stockfw.layoutWindows(copy.profile, sim.fb), { vectorLast: true });
		// an install that doesn't move the codec keeps the board's order (plan.py)
		const plain = makePlan(sim.board, [[base, img]], MERGE, codec, stockfw.layoutWindows(copy.profile, sim.fb));
		assert.deepEqual(plain.eraseOrder, plain.erase);
		assert.deepEqual(plain.writeOrder, plain.writes);
		assert.ok(!plain.describe().some((l) => l.startsWith("order:")));
		assert.ok(plan.describe().some((l) => l.startsWith("order:")));
		// every DNLOAD that went through: ["erase", addr], ["block", its sector] or ["command"]
		const run = async (left) => {
			const events = [];
			radio.enterBootloader();
			const t = radio.dfuTransport();
			const cut = { ...t, async out(req, value, data) {
				if (left !== null && left-- === 0) throw new Error("the device has been lost");
				const r = await t.out(req, value, data);
				if (value) events.push(["block", radio.addr]);
				else if (data?.[0] === 0x41) events.push(["erase", radio.erased.at(-1)]);
				else events.push(["command"]);
				return r;
			} };
			if (left === null) await new Dfu(cut, noWait).run(plan, sim.key);
			else await assert.rejects(new Dfu(cut, noWait).run(plan, sim.key));
			return events;
		};
		const orig = radio.flash.slice();
		const ev = await run(null);
		assert.equal(ev.find(([k]) => k === "erase")[1], base, `${id}: first erase`);
		const written = ev.filter(([k]) => k === "block").map(([, a]) => a);
		const first = written.indexOf(base);
		assert.ok(first > 0 && written.slice(first).every((a) => a === base), `${id}: the vector table's sector is written last`);
		const start = ev.findIndex(([k, a]) => k === "block" && a === base);
		const cuts = new Set([0, 1, 2, 3, 4, Math.floor(start / 2), start - 1, start, start + 1, ev.length - 2]);
		for (const c of [...cuts].sort((x, y) => x - y)) {
			radio.flash.set(orig);
			radio.erased = [];
			await run(c);
			if (radio.erased.includes(base) && c <= start) {
				assert.ok(at(radio, base, sec.size).every((b) => b === 0xFF), `${id} cut ${c}: the vector table's sector is blank`);
				radio.powerOn();
				assert.equal(radio.mode, "bootloader", `${id} cut ${c}: the bootloader stays in its update mode`);
			}
			// the same plan run again finishes it
			await run(null);
			assert.ok(equal(at(radio, base, img.length), img), `${id} cut ${c}`);
			for (const [a, d] of sim.codecSegments) assert.ok(equal(at(radio, a, d.length), d), `${id} cut ${c} ${a}`);
			radio.powerOn();
			assert.equal(radio.codecReport(), sim.board.codec.id, `${id} cut ${c}`);
		}
	}
});

// app.js needs the DOM, so its source is checked: every codec decision says whether the copy is kept in this browser, the
// loader route takes a move whether or not it is (the stock file then), and a refusal by the codec guard restarts the radio
test("the page: a move takes a copy only when this browser keeps it, through the loader when the radio offers it", () => {
	const src = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	const calls = src.match(/decideCodec\(S\.board[^\n]*(?:\n[^\n]*){0,2}/g) || [];
	assert.ok(calls.length >= 3);
	for (const c of calls) assert.match(c, /savedKept: savedKept\(\)/, c);
	const route = src.match(/const stubRoute = [^;]*;/)[0];
	assert.match(route, /codecMoves\(\)\)/);
	assert.doesNotMatch(route, /stored/);
	assert.match(src, /stockfw\.extract\(S\.stock\.data, await getKey\(S\.board\)/, "the loader route takes the codec from the stock file");
	assert.match(src, /e\.codecGuard[\s\S]{0,200}resetChip/, "a codec guard refusal restarts the radio out of the loader");
	// the move's codec is settled before the radio is handed to the loader, so a refusal leaves it as it was
	const go = src.slice(src.indexOf("const go = button(t(\"flash.f4.go\")"));
	assert.ok(go.indexOf("stubSegments(") > 0 && go.indexOf("stubSegments(") < go.indexOf("enterFromRFoundry("));
	assert.doesNotMatch(src, /writeImage\([^)]*stubSegments/);
	// any failure in the loader restarts the radio; only a loader that doesn't answer is tried again
	assert.match(src, /stub\.erased \? "flash\.f4\.failed_bootloader"/);
	// only a move writes the vector table's sector last
	assert.match(src, /vectorLast: !!S\.decision\.moves/);
	// a move reads the radio's codec again, and takes only this radio's saved copy when it can't
	assert.match(src, /!moves && \(await loadCodec/);
	assert.match(src, /loadCodec\(S\.board, fb\(\), \{ uid, strict: true \}\)/);
});

// IndexedDB as much of it as codecsave.js uses
function fakeIndexedDB() {
	const data = new Map();
	const store = { put(v, k) { data.set(k, v); return { result: k }; }, get(k) { return { result: data.get(k) }; } };
	const db = {
		createObjectStore() {},
		close() {},
		transaction() {
			const t = { objectStore: () => store };
			queueMicrotask(() => t.oncomplete());
			return t;
		},
	};
	return { data, open() { const r = {}; queueMicrotask(() => { r.result = db; r.onupgradeneeded?.(); r.onsuccess(); }); return r; } };
}

test("one saved copy per radio: a move never takes another radio's", async () => {
	const idb = fakeIndexedDB();
	globalThis.indexedDB = idb;
	try {
		const id = MOVING[0];
		const { sim, radio } = await former(id);
		const got = await readCodec(mem(radio), sim.board, sim.fb);
		const A = "00AA11BB22CC33DD44EE55FF", B = "99aa11bb22cc33dd44ee55ff";
		assert.ok(await saveCodec(sim.board, got.segments, A));
		assert.ok(idb.data.has(`${id}-${sim.board.codec.id}-${A.toLowerCase()}`));
		const mine = await loadCodec(sim.board, sim.fb, { uid: A, strict: true });
		assert.deepEqual(mine.segments, got.segments);
		assert.equal(mine.uid, A.toLowerCase());
		assert.equal(await loadCodec(sim.board, sim.fb, { uid: B, strict: true }), null, "radio B: not A's copy");
		assert.equal(await loadCodec(sim.board, sim.fb, { uid: B }), null, "nor outside a move");
		// a radio that can't say which it is (in its bootloader): the copy saved last
		assert.equal((await loadCodec(sim.board, sim.fb)).uid, A.toLowerCase());
		// a copy an earlier page saved without a UID: outside a move only
		assert.ok(await saveCodec(sim.board, got.segments));
		assert.equal((await loadCodec(sim.board, sim.fb, { uid: B })).uid, null);
		assert.equal(await loadCodec(sim.board, sim.fb, { uid: B, strict: true }), null);
	} finally {
		delete globalThis.indexedDB;
	}
});
