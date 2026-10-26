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
import { Link } from "../static/js/flasher/link.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import * as openfw from "../static/js/flasher/openfw.js";
import { makeBundle, openBundle, describeFirmware } from "../static/js/flasher/backup.js";
import { targets, planInternal } from "../static/js/flasher/restore.js";
import { decideCodec } from "../static/js/flasher/decide.js";
import { codecSegments } from "../static/js/flasher/codecsave.js";
import { makePlan, MERGE, RESTORE } from "../static/js/flasher/plan.js";
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
