// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio on another open firmware, backed up over that firmware's CPS before RadioFoundry is installed (openfw.js), run
// against the simulated radio (fake.js state "openfw") for every board the page offers it on: only R requests to areas
// 9, 5 and 1, never more than 2045 bytes, never past the MCU flash, the VFO overlay read around; a radio that stops
// answering, then a resume; the key and the moved codec found in the read; the bundle, its restore, and the install
// that follows from what the backup found.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { sha256, u32le } from "../static/js/flasher/bytes.js";
import { Link, probe, whyNoAnswer } from "../static/js/flasher/link.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import * as openfw from "../static/js/flasher/openfw.js";
import { makeBundle, openBundle, describeFirmware } from "../static/js/flasher/backup.js";
import { targets, planInternal } from "../static/js/flasher/restore.js";
import { decideCodec, DecisionError } from "../static/js/flasher/decide.js";
import { codecSegments } from "../static/js/flasher/codecsave.js";
import { makePlan, MERGE, KEEP, RESTORE } from "../static/js/flasher/plan.js";
import { segments } from "../static/js/flasher/manifest.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { unmoveCode, unmoveBranches, moveCode, moveBranches } from "../static/js/flasher/codecmap.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const fx = JSON.parse(readFileSync(join(DIR, "fixtures.json"), "utf8"));
const fb = JSON.parse(readFileSync(join(DIR, "flash-boards.json"), "utf8"));
const image = new Uint8Array(readFileSync(join(DIR, "image.bin")));
const SPI = 0x100000;		// a small SPI flash keeps the reads quick; the VFO pair is inside it
const radioOf = () => ({ backup: { spi_size: SPI } });
const noWait = { wait: async () => {} };
const BOARDS = Object.keys(fb.boards).filter((id) => openfw.supported(fb.boards[id]));

async function openRadio(id, opts = {}) {
	const sim = await simulation(fb, id, 11);
	const radio = new FakeRadio(sim, { state: "openfw", spiSize: SPI, openfw: opts });
	await radio.ready;
	return { sim, radio, link: new Link(radio.serialStream(), { timeout: 30 }) };
}

// Every request the backup sent: reads only, of areas 9, 5 and 1, at most 2045 bytes, area 5 never past the MCU flash
function checkRequests(radio, board) {
	for (const r of radio.openfwLog) {
		const m = r.match(/^R (\d+) 0x([0-9a-f]+) (\d+)$/);
		assert.ok(m, `only reads: ${r}`);
		const [area, addr, len] = [Number(m[1]), parseInt(m[2], 16), Number(m[3])];
		assert.ok([1, 5, 9].includes(area), `area ${area}`);
		assert.ok(len <= openfw.CLIP && len > 0, `length ${len}`);
		if (area === 5) assert.ok(addr + len <= board.flash_end - board.rom_base, "inside the MCU flash");
	}
	assert.equal(radio.openfw.controls, 0, "no C command");
}

test("the boards it is offered on come from the board data", () => {
	assert.ok(BOARDS.length, "at least one board");
	for (const [id, b] of Object.entries(fb.boards)) {
		const want = b.cps_radio_types.length > 0 && b.family === "usb-dfu" && !!b.key_sha256;
		assert.equal(openfw.supported(b), want, id);
		if (want) assert.equal(b.rom_base, b.protected[0][0], id);
	}
	assert.equal(openfw.supported(null), false);
});

test("requests: 2045 bytes at most, the VFO pair in 32-byte reads, nothing skipped or read twice", () => {
	const ps = openfw.pieces(0, SPI, { avoid: openfw.VFO });
	let at = 0;
	for (const [a, n] of ps) {
		assert.equal(a, at);
		assert.ok(n > 0 && n <= openfw.CLIP);
		// a read spanning the pair would come back with the live VFOs in it
		assert.ok(!(a <= openfw.VFO[0] && a + n > openfw.VFO[1]));
		if (a >= openfw.VFO[0] && a < openfw.VFO[1]) assert.ok(n <= openfw.PIECE);
		at += n;
	}
	assert.equal(at, SPI);
	assert.ok(ps.some(([a, n]) => a === openfw.VFO[0] && n === openfw.PIECE));
	// resuming in the middle of the pair
	assert.deepEqual(openfw.pieces(0x75A0, 0x7700, { avoid: openfw.VFO }).slice(0, 4), [[0x75A0, 32], [0x75C0, 32], [0x75E0, 32], [0x7600, 0x100]]);
	assert.deepEqual(openfw.pieces(0x100000 - 10, 0x100000), [[0x100000 - 10, 10]], "the last read stops at the end");
});

for (const id of BOARDS) {
	test(`a full backup of a radio on another open firmware, the key and the moved codec found (${id})`, async () => {
		const { sim, radio, link } = await openRadio(id);
		const board = sim.board;
		const steps = new Set();
		const st = await openfw.readBackup(link, board, radioOf(), { progress: (part) => steps.add(part) });
		assert.equal(st.stopped, null);
		assert.ok(openfw.complete(st));
		assert.deepEqual([...steps], ["internal", "spi"]);
		assert.deepEqual(st.rom.data, radio.flash.subarray(0, board.flash_end - board.rom_base));
		assert.deepEqual(st.spi.data, radio.spi, "the flash's own bytes, no VFO overlay");
		assert.equal(st.info.radioType, board.cps_radio_types[0]);
		checkRequests(radio, board);
		assert.ok(radio.openfwLog.every((r) => !r.startsWith("R 10 ")), "area 10 never asked");

		const { key, codec } = await openfw.analyze(st, board, sim.fb);
		assert.deepEqual(key, sim.key);
		assert.ok(codec, "codec found");
		assert.equal(codec.where, `0x${radio.openfw.codecAt.toString(16).toUpperCase().padStart(8, "0")}`);
		// as the merge from the stock file writes it, and as this browser keeps it
		assert.deepEqual(codec.segments, sim.codecSegments);
		assert.deepEqual(await codecSegments(codec.profile, codec.save, sim.fb), sim.codecSegments);

		// the bundle: no bootloader, so no key; the open firmware named
		const parts = openfw.bundleParts(st, board);
		assert.deepEqual(parts.map((p) => [p.what, p.complete]), [["radio-info", true], ["internal-flash", true], ["spi-flash", true]]);
		const meta = { radio: { board: id, name: board.name, model: board.name, chip: null, version: null }, firmware_now: openfw.firmware(st.info), contents: "all" };
		const zipped = await makeBundle(meta, parts);
		const bundle = await openBundle(zipped);
		assert.equal(bundle.meta.firmware_now.kind, "openfw");
		assert.equal(bundle.parts.get("internal-flash").addr, board.app_base);
		assert.equal(bundle.parts.get("internal-flash").data.length, board.flash_end - board.app_base);
		assert.match(describeFirmware(bundle.meta.firmware_now), /^another open firmware sim-open1 \(built 20261025120000, CPS radio type \d+\)$/);
		const k = sim.key.subarray(0, 32);
		assert.ok(!zipped.some((_, i) => i + 32 <= zipped.length && zipped[i] === k[0] && k.every((x, j) => zipped[i + j] === x)), "the key is not in the bundle");
		assert.deepEqual(targets(bundle), ["openfw"]);
	});
}

const ONE = BOARDS[0];

test("the codec is found whether the tables still call the stock address or were patched for the move", async () => {
	for (const tables of ["stock", "patched"]) {
		const { sim, radio } = await openRadio(ONE, { tables });
		const got = await openfw.codecFromImage(radio.flash, sim.board.rom_base, sim.board, sim.fb);
		assert.deepEqual(got?.segments, sim.codecSegments, tables);
	}
	// where the stock firmware keeps it, or where RadioFoundry's merge placed it: the forms the page already knew
	const sim = await simulation(fb, ONE, 11);
	for (const state of ["stock", "rfoundry"]) {
		const radio = new FakeRadio(sim, { state, spiSize: SPI });
		const got = await openfw.codecFromImage(radio.flash, sim.board.rom_base, sim.board, sim.fb);
		assert.equal(got?.where, "known", state);
		assert.deepEqual(got.segments, sim.codecSegments);
	}
});

test("no codec in the radio: none is made up, and the key is still found", async () => {
	const { sim, link } = await openRadio(ONE, { codec: false });
	const st = await openfw.readBackup(link, sim.board, radioOf());
	const got = await openfw.analyze(st, sim.board, sim.fb);
	assert.equal(got.codec, null);
	assert.deepEqual(got.key, sim.key);
	// a key that doesn't match the board's hash is never taken
	assert.equal(await openfw.keyFromImage(st.rom.data, { ...sim.board, key_sha256: "00".repeat(32) }, sim.fb.key_len), null);
});

test("a damaged copy is not taken for the codec", async () => {
	const { sim, radio } = await openRadio(ONE);
	const at = radio.openfw.codecAt - sim.board.rom_base;
	const img = radio.flash.slice();
	img[at + 0x400] ^= 0x01;
	assert.equal(await openfw.codecFromImage(img, sim.board.rom_base, sim.board, sim.fb), null);
});

test("moving the code and the islands' calls back is the inverse of moving them", async () => {
	const sim = await simulation(fb, ONE, 11);
	const m = sim.profile.map;
	const [[run, code], [tab, tables]] = sim.stockSegments;
	const moved = await moveCode(code, m, run + 0x1000);
	assert.deepEqual(await unmoveCode(moved, m, run + 0x1000), code);
	assert.equal(await unmoveCode(moved, m, run + 0x2000), null, "another address");
	assert.deepEqual(unmoveBranches(tab, moveBranches(tab, tables, m, run + 0x1000), m), tables);
	assert.deepEqual(unmoveBranches(tab, tables, m), tables, "nothing moved");
});

test("a radio that stops answering: the backup stops cleanly, keeps what it read, and continues after a power cycle", async () => {
	const { sim, radio, link } = await openRadio(ONE, { hangAfter: 100 });
	const board = sim.board;
	const st = await openfw.readBackup(link, board, radioOf());
	assert.equal(st.stopped.part, "internal");
	assert.equal(st.stopped.why, "no answer");
	assert.equal(st.rom.done, 98 * openfw.CLIP, "the radio info, then 98 reads answered");
	assert.equal(st.stopped.addr, board.rom_base + st.rom.done);
	assert.equal(st.spi.done, 0, "the SPI flash isn't started on a radio that stopped");
	assert.ok(!openfw.complete(st));
	// what was read can be saved, marked incomplete (the bootloader alone gives no internal flash part yet)
	const parts = openfw.bundleParts(st, board);
	const internal = parts.find((p) => p.what === "internal-flash");
	assert.equal(internal?.complete ?? false, false);
	radio.powerCycle();
	const log = radio.openfwLog.length;
	const again = await openfw.readBackup(link, board, radioOf(), { state: st });
	assert.equal(again, st, "the same state, continued");
	assert.ok(openfw.complete(again));
	assert.deepEqual(again.rom.data, radio.flash);
	// it went on where it stopped: the first area 5 read of the second run
	const first = radio.openfwLog.slice(log).find((r) => r.startsWith("R 5 "));
	assert.equal(first, `R 5 0x${(98 * openfw.CLIP).toString(16)} ${openfw.CLIP}`);
	checkRequests(radio, board);
});

test("a short silence is ridden out by asking again", async () => {
	const { sim, radio, link } = await openRadio(ONE, { hangAfter: 40, hangFor: 1 });
	const st = await openfw.readBackup(link, sim.board, radioOf());
	assert.equal(st.stopped, null);
	assert.deepEqual(st.rom.data, radio.flash);
});

test("another radio's type, another firmware, or no radio info: nothing is read", async () => {
	const { sim, radio, link } = await openRadio(ONE, { radioType: 63 });
	await assert.rejects(openfw.readBackup(link, sim.board, radioOf()), { name: "OpenFwError", code: "type" });
	assert.deepEqual(radio.openfwLog, ["R 9 0x0 46"]);
	const quiet = new Link({ buf: new Uint8Array(0), clear() { this.buf = new Uint8Array(0); }, async write() {}, take() {} }, { timeout: 10 });
	await assert.rejects(openfw.readBackup(quiet, sim.board, radioOf()), { code: "no-info" });
	// a state from another firmware build is not continued
	const { radio: r2, link: l2 } = await openRadio(ONE, { hangAfter: 5 });
	const st = await openfw.readBackup(l2, sim.board, radioOf());
	r2.powerCycle();
	r2.openfw.git = "other";
	const fresh = await openfw.readBackup(l2, sim.board, radioOf(), { state: st });
	assert.notEqual(fresh, st);
	assert.ok(openfw.complete(fresh));
});

test("from the backup to RadioFoundry: the saved codec merged, the found key opens the bootloader, then a restore of the open firmware", async () => {
	const { sim, radio, link } = await openRadio(ONE);
	const board = sim.board;
	const st = await openfw.readBackup(link, board, radioOf());
	const { key, codec } = await openfw.analyze(st, board, sim.fb);
	const d = decideCodec(board, fx.manifest, { haveSaved: true });
	assert.equal(d.mode, MERGE);
	assert.equal(d.source, "saved");
	const merged = await codecSegments(codec.profile, codec.save, sim.fb);
	const before = radio.flash.slice();
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(board, segments(fx.manifest, image), MERGE, merged,
		(await import("../static/js/flasher/stockfw.js")).layoutWindows(codec.profile, sim.fb)), key);
	assert.deepEqual(radio.violations, []);
	assert.equal(radio.running, "rfoundry", "RadioFoundry with its codec");
	assert.equal(radio.codecReport(), board.codec.id);
	// the bundle puts the open firmware back through the same bootloader
	const bundle = await openBundle(await makeBundle({ radio: { board: ONE, name: board.name }, firmware_now: openfw.firmware(st.info) }, openfw.bundleParts(st, board)));
	const plan = planInternal(bundle, board);
	assert.equal(plan.mode, RESTORE);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(plan, key);
	assert.deepEqual(radio.flash, before);
	assert.equal(u32le(radio.flash, board.app_base - board.rom_base), u32le(before, board.app_base - board.rom_base));
	assert.equal(await sha256(radio.flash), await sha256(before));
});

// ---------------------------------------------------------------------------------------------------------------
// The user flows, as far as they run outside the page (the fake's bootloader keeps its key at 0x2340): what the current-firmware step does with the probe's answer
// (openfw.recognized), what the backup finds, and what the codec step then needs. The codec step's rules, as app.js
// codecView applies them: a decision from decideCodec (the codec: the saved copy, or the stock file), and the key from
// the radio or else the stock file. Never a KEEP the user didn't ask for: without the codec the decision throws, and
// the page asks for the stock file

// What the codec step does with what the backup found: {decision, needCodec, needKey}
function codecStep(board, found, { haveStock = false } = {}) {
	let decision = null;
	try {
		decision = decideCodec(board, fx.manifest, { haveSaved: !!found?.codec, haveStock });
	} catch (e) {
		if (!(e instanceof DecisionError)) throw e;
	}
	assert.notEqual(decision?.mode, KEEP, "never a codec kept unverified");
	return { decision, needCodec: !decision && !haveStock, needKey: !found?.key && !haveStock };
}

// The install from there: the codec from the saved copy or the stock file, the key from the radio or the stock file
async function install(sim, radio, found) {
	const board = sim.board;
	const key = found.key || await stockfw.deriveKey(sim.stock, board.key_sha256, sim.fb);
	let prof, codec;
	if (found.codec) ({ profile: prof } = found.codec), codec = await codecSegments(prof, found.codec.save, sim.fb);
	else ({ profile: prof, segments: codec } = await stockfw.extract(sim.stock, key, sim.fb));
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(board, segments(fx.manifest, image), MERGE, codec, stockfw.layoutWindows(prof, sim.fb)), key);
	assert.deepEqual(radio.violations, []);
	assert.equal(radio.codecReport(), board.codec.id, "RadioFoundry with its codec");
}

const backupOf = async (opts = {}, prepare = () => {}) => {
	const r = await openRadio(ONE, opts);
	prepare(r);
	const st = await openfw.readBackup(r.link, r.sim.board, radioOf());
	return { ...r, st, found: await openfw.analyze(st, r.sim.board, r.sim.fb) };
};

test("flow: the probe finds the other firmware (RadioFoundry chosen, or Check the Radio under Other / Not sure)", async () => {
	const { sim, radio, link } = await openRadio(ONE);
	assert.equal(await probe(link), null);
	const why = await whyNoAnswer(link);
	assert.equal(why.kind, "openfw");
	assert.equal(openfw.recognized(sim.board, why), "backup");
	// the probe's frames are refused, then one R 9: nothing else before the user asks for the backup
	assert.deepEqual(radio.openfwLog.filter((r) => !r.startsWith("other")), ["R 9 0x0 46"]);
	assert.equal(radio.openfw.controls, 0);
});

test("flow: a wrong radio picked, a board without this route, a RadioFoundry radio, something else", async () => {
	const { sim, link } = await openRadio(ONE, { radioType: 63 });
	await probe(link);
	assert.equal(openfw.recognized(sim.board, await whyNoAnswer(link)), "wrong-type");
	// a board the page can't back up from another open firmware: the stock-file route, as before
	for (const [id, b] of Object.entries(fb.boards)) if (!openfw.supported(b)) assert.equal(openfw.recognized(b, { kind: "openfw", radioType: 6 }), "unsupported", id);
	// RadioFoundry answers the probe: never this route
	const rf = new FakeRadio(sim, { state: "rfoundry", spiSize: SPI });
	const p = await probe(new Link(rf.serialStream(), { timeout: 30 }));
	assert.ok(p?.board);
	assert.equal(openfw.recognized(sim.board, { kind: "other" }), null);
	assert.equal(openfw.recognized(sim.board, { kind: "silent" }), null);
});

test("flow: backup taken, key and codec found: the saved codec is merged, no stock file", async () => {
	const { sim, radio, found } = await backupOf();
	assert.deepEqual(codecStep(sim.board, found), { decision: decideCodec(sim.board, fx.manifest, { haveSaved: true }), needCodec: false, needKey: false });
	await install(sim, radio, found);
});

test("flow: key only (no codec in the radio): the codec step asks for the stock file, which then gives the codec", async () => {
	const { sim, radio, found } = await backupOf({ codec: false });
	assert.ok(found.key && !found.codec);
	const step = codecStep(sim.board, found);
	assert.equal(step.decision, null);
	assert.ok(step.needCodec && !step.needKey);
	assert.equal(codecStep(sim.board, found, { haveStock: true }).decision.mode, MERGE);
	await install(sim, radio, found);
});

test("flow: codec only (no key in the bootloader read): the saved codec, the key from the stock file", async () => {
	const { sim, radio, found } = await backupOf({}, ({ radio: r }) => r.flash.fill(0x5A, 0x2340, 0x2340 + fb.key_len));
	assert.ok(!found.key && found.codec);
	const step = codecStep(sim.board, found);
	assert.equal(step.decision.source, "saved");
	assert.ok(step.needKey && !step.needCodec);
	await install(sim, radio, found);
});

test("flow: neither (no key, no codec): the stock file for both, as from any other firmware", async () => {
	const { sim, radio, found } = await backupOf({ codec: false }, ({ radio: r }) => r.flash.fill(0x5A, 0x2340, 0x2340 + fb.key_len));
	assert.ok(!found.key && !found.codec);
	const step = codecStep(sim.board, found);
	assert.ok(step.needCodec && step.needKey);
	await install(sim, radio, found);
});

test("flow: backup skipped: the stock file, never a silent keep", async () => {
	const sim = await simulation(fb, ONE, 11);
	const step = codecStep(sim.board, null);
	assert.ok(step.needCodec && step.needKey);
	// keeping the radio's codec only when the user forces it, with the risk said
	const forced = decideCodec(sim.board, fx.manifest, { requested: KEEP });
	assert.equal(forced.mode, KEEP);
	assert.ok(forced.risk);
});

test("flow: area 5 refused midway: the read stops (refused), the key from what was read, the codec from the stock file", async () => {
	const sim = await simulation(fb, ONE, 11);
	const at = sim.board.app_base - sim.board.rom_base + 0x1000;	// past the bootloader, before the codec copy ends
	const { radio, st, found } = await backupOf({ refuse5: at });
	assert.equal(st.stopped.why, "refused");
	assert.equal(st.stopped.part, "internal");
	assert.ok(st.rom.done <= at && st.rom.done > at - openfw.CLIP);
	assert.ok(found.key, "the bootloader was read");
	assert.equal(found.codec, null);
	assert.ok(codecStep(sim.board, found).needCodec);
	await install(sim, radio, found);
});

test("flow: stopped before the bootloader was read: neither found, the stock file for both", async () => {
	const { st, found, sim } = await backupOf({ hangAfter: 3 });
	assert.equal(st.stopped.why, "no answer");
	assert.ok(st.rom.done < sim.board.app_base - sim.board.rom_base);
	assert.ok(!found.key && !found.codec);
	assert.deepEqual(openfw.bundleParts(st, sim.board).map((p) => p.what), ["radio-info"], "nothing of the application to save yet");
});

test("flow: the user stops the read (Stop Reading, or leaves the step), then continues", async () => {
	const { sim, radio, link } = await openRadio(ONE);
	const stop = new AbortController();
	let n = 0;
	const st = await openfw.readBackup(link, sim.board, radioOf(), { signal: stop.signal, progress: () => { if (++n === 50) stop.abort(); } });
	assert.equal(st.stopped.why, "cancelled");
	assert.equal(st.rom.done, 50 * openfw.CLIP);
	const again = await openfw.readBackup(link, sim.board, radioOf(), { state: st });
	assert.ok(openfw.complete(again));
	assert.deepEqual(again.rom.data, radio.flash);
});

test("flow: a radio that answers in smaller pieces is read whole", async () => {
	const { st, radio } = await backupOf({ clip: 1000 });
	assert.ok(openfw.complete(st));
	assert.deepEqual(st.rom.data, radio.flash);
	assert.deepEqual(st.spi.data, radio.spi);
});

test("flow: RadioFoundry with the CPS shim but no debug link looks the same, and its placed codec is found as it is", async () => {
	const { sim, radio, found, st } = await backupOf({ codec: false }, ({ sim: s, radio: r }) => {
		for (const [a, d] of s.codecSegments) r.flash.set(d, a - s.board.rom_base);
	});
	assert.ok(openfw.complete(st));
	assert.equal(found.codec.where, "known");
	assert.deepEqual(found.codec.segments, sim.codecSegments);
	await install(sim, radio, found);
});

test("flow: after the install, the other firmware's data in the settings area is claimed (the backup holds it)", async () => {
	const { sim, radio, found, st } = await backupOf();
	assert.ok(openfw.complete(st));
	await install(sim, radio, found);
	const link = new Link(radio.serialStream());
	assert.equal((await link.storageInfo()).region, "foreign");
	assert.equal(await link.storageClaim(), true);
	assert.equal((await link.storageInfo()).region, "ours");
});
