// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A cable radio the boot ROM starts directly (board.toml [flash.stub] rom_entry), against the simulated radio: the
// whole program memory backup, the codec from the radio's own flash, the install, the boot ROM's flag sector written
// last, an install interrupted before it, and the restore that puts the original firmware and its flag back. Run with
// site/tests/run.sh (fixtures from site/tests/fixtures.py).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { equal, le32, u32le, concat } from "../static/js/flasher/bytes.js";
import { Stub, StubError, stubImages, ROM_FLAG, romFlagSector } from "../static/js/flasher/stub.js";
import { Link, probe } from "../static/js/flasher/link.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { FakeC7000, c7000Simulation, fakeC7000Options, STOCK_ENTRY } from "../static/js/flasher/fakec7000.js";
import { installWrite, restoreWrite, romEntry, startState, stubFor } from "../static/js/flasher/c7000ui.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const FB = json("flash-boards.json");
const C = FB.c7000;
const stubs = stubImages(json("c7000-stub.json"));
const radios = json("radios.json").radios;
const quiet = () => {};
const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));
// the boards the boot ROM starts directly, from the data: none named here
const DIRECT = Object.keys(C.boards).filter((id) => romEntry(C, id) !== null);

// The radio switched on with its original firmware, the stub loaded through the boot ROM and configured for the board
async function connect(t, id) {
	const radio = radios.find((r) => r.id === id);
	const sim = await c7000Simulation(C, id, 7000, radio);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200,
		...fakeC7000Options(radio, FB) });
	t.after(() => r.stop());
	assert.equal(r.firmware(), "stock", "the boot ROM starts the original firmware its flag names");
	const s = new Stub(r.stream, { log: quiet });
	s.size.progmem = c7.totalSize(C, id);
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubFor(C, id, stubs.image), stubs.load);
	await s.configure({ ...C.boards[id].stub_board, protect: C.boards[id].protect || [] });
	return { sim, r, s, radio };
}

// An application image the page takes for RadioFoundry, with the codec decided from the radio's own flash
async function installData(sim, id, backup) {
	const what = await c7.analyze(backup, sim.c, id);
	assert.equal(what.firmware, "stock");
	assert.ok(what.codec, "the codec found in the radio's own program flash");
	const d = await c7.decide(sim.c, id, { flashCodec: what.codec });
	assert.equal(d.mode, "write");
	const image = sim.progmem.slice(0x20000, 0x60000);
	image.set(new TextEncoder().encode("RadioFoundry"), 0x2000);
	return { what, target: c7.target(sim.c, id, image, d.slot) };
}

const flagOf = (pm) => [[...pm.subarray(0, 4)], u32le(pm, 4)];

test("the data names a board the boot ROM starts directly, with its entry and its whole program flash size", () => {
	assert.ok(DIRECT.length > 0, "a rom_entry board in flash-boards.json");
	assert.deepEqual([...ROM_FLAG], [...Buffer.from(C.rom_flag, "hex")], "the flag bytes are stubtool's");
	for (const id of DIRECT) {
		assert.equal(C.boards[id].rom_entry, C.progmem_base + C.app_start, `${id}: RadioFoundry's entry is the application's start`);
		assert.ok(c7.totalSize(C, id) >= C.progmem_size, id);
	}
	const flag = romFlagSector(new Uint8Array(4096).fill(0x11), 0x0300C000);
	assert.deepEqual([...flag.subarray(0, 9)], [0x74, 0x68, 0x64, 0x0C, 0x00, 0xC0, 0x00, 0x03, 0xFF], "a sector without the flag: flag, entry, 0xFF");
	const kept = romFlagSector(concat(ROM_FLAG, le32(STOCK_ENTRY), new Uint8Array(4088).fill(0x22)), 0x0300C000);
	assert.equal(u32le(kept, 4), 0x0300C000);
	assert.ok(kept.subarray(8).every((b) => b === 0x22), "a sector with the flag keeps the rest");
});

for (const id of DIRECT) {
	test(`${id}: whole backup, codec from the radio, install, the flag sector last naming the entry, restart`, async (t) => {
		const { sim, r, s } = await connect(t, id);
		const backup = await s.dump("progmem", 0, c7.totalSize(C, id));
		assert.equal(backup.length, c7.totalSize(C, id), "the whole program flash is read");
		assert.ok(equal(backup, sim.progmem), "read exactly");
		const { what, target: [start, data] } = await installData(sim, id, backup);
		const real = C.profiles.find((p) => p.board === id);
		if (real) assert.equal(what.codec.profile.name, `simulated ${real.name}`, "the board's own codec profile");
		const states = new Map(), order = [];
		const res = await installWrite(s, sim.c, id, start, data, (a, st) => {
			states.set(a, st);
			if (st === "written") order.push(a);
		});
		const entry = C.boards[id].rom_entry;
		assert.deepEqual(flagOf(r.progmem), [[...ROM_FLAG], entry], "the flag names RadioFoundry's entry");
		assert.ok(equal(r.progmem.subarray(8, 4096), sim.progmem.subarray(8, 4096)), "the rest of the flag sector kept");
		assert.ok(equal(r.progmem.subarray(4096, C.app_start), sim.progmem.subarray(4096, C.app_start)), "nothing else below the application written");
		assert.ok(equal(r.progmem.subarray(C.app_start, C.app_start + data.length), data), "application and codec slot written");
		assert.ok(equal(r.progmem.subarray(C.app_end), sim.progmem.subarray(C.app_end)), "nothing past the application area written");
		assert.equal(order.at(-1), 0, "the flag sector is the last written");
		assert.equal(states.get(0), "written");
		assert.equal(res.written, order.length);
		const seen = new Map();
		const again = await installWrite(s, sim.c, id, start, data, (a, st) => seen.set(a, st));
		assert.equal(again.written, 0, "the same install again writes nothing, the flag included");
		assert.equal(seen.get(0), "same", "the map shows the flag sector unchanged, not still to write");
		assert.equal(startState(C, id, 0), "todo", "the flag sector is written, so it isn't drawn as never written");
		assert.equal(startState(C, id, C.sector), "boot");
		assert.equal(startState(C, id, C.app_start), "");
		await s.resetChip();
		await sleep(1500);
		assert.equal(r.firmware(), "rfoundry", "the boot ROM now starts RadioFoundry");
		const p = await probe(new Link(r.stream));
		assert.ok(p, "RadioFoundry answers after the restart");
	});

	test(`${id}: an install stopped before the flag sector leaves the radio starting its original firmware; run again, it completes`, async (t) => {
		const { sim, r, s } = await connect(t, id);
		const backup = await s.dump("progmem", 0, c7.totalSize(C, id));
		const { target: [start, data] } = await installData(sim, id, backup);
		// the cable pulled as the flag sector would go out: every application sector is in place, verified
		const writeSector = s.writeSector.bind(s);
		s.writeSector = async (addr, sec) => {
			if (addr === 0) throw new StubError("the cable was pulled");
			return writeSector(addr, sec);
		};
		await assert.rejects(installWrite(s, sim.c, id, start, data), /cable was pulled/);
		assert.deepEqual(flagOf(r.progmem), [[...ROM_FLAG], STOCK_ENTRY], "the flag still names the original firmware");
		r.fwKind = null;
		assert.equal(r.firmware(), "stock");
		s.writeSector = writeSector;
		const res = await installWrite(s, sim.c, id, start, data);
		assert.equal(res.written, 1, "only the flag sector is left to write");
		assert.equal(u32le(r.progmem, 4), C.boards[id].rom_entry);
	});

	test(`${id}: the restore of the whole backup puts the original firmware and its flag back; a backup without the flag is refused`, async (t) => {
		const { sim, r, s } = await connect(t, id);
		const backup = await s.dump("progmem", 0, c7.totalSize(C, id));
		const { target: [start, data] } = await installData(sim, id, backup);
		await installWrite(s, sim.c, id, start, data);
		assert.equal(u32le(r.progmem, 4), C.boards[id].rom_entry);
		// not a whole backup of this radio: no flag at 0 (a file laid out from the application start)
		const before = r.progmem.slice();
		const flagless = backup.slice();
		flagless.fill(0xFF, 0, 4096);
		await assert.rejects(restoreWrite(s, sim.c, id, flagless), /no_rom_flag|start-up flag/);
		// the radio's flash below the application differs from the backup's: nothing writes there, so nothing is written
		const other = backup.slice();
		other[0x2000] ^= 0x01;
		await assert.rejects(restoreWrite(s, sim.c, id, other), /boot_differs|start-up area differs/);
		assert.ok(equal(r.progmem, before), "a refused restore wrote nothing");
		const states = new Map();
		const res = await restoreWrite(s, sim.c, id, backup, (a, st) => states.set(a, st));
		assert.ok(res.written > 1);
		assert.equal(states.get(0), "written");
		assert.ok(equal(r.progmem, sim.progmem), "the whole program flash is the backup's again");
		assert.deepEqual(flagOf(r.progmem), [[...ROM_FLAG], STOCK_ENTRY]);
		r.fwKind = null;
		assert.equal(r.firmware(), "stock", "the boot ROM starts the original firmware again");
	});
}

test("a board with a bootloader: the install never writes below the application, a restore never asks for a flag", async (t) => {
	const id = Object.keys(C.boards).find((b) => romEntry(C, b) === null && C.boards[b].bootloader_id);
	assert.ok(id);
	const radio = radios.find((x) => x.id === id);
	const sim = await c7000Simulation(C, id, 7000, radio);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200 });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	s.size.progmem = c7.totalSize(C, id);
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	await s.configure(C.boards[id].stub_board);
	const backup = await s.dump("progmem", 0, c7.totalSize(C, id));
	const { target: [start, data] } = await installData(sim, id, backup);
	await installWrite(s, sim.c, id, start, data);
	assert.ok(equal(r.progmem.subarray(0, C.app_start), sim.progmem.subarray(0, C.app_start)), "the bootloader untouched");
	await restoreWrite(s, sim.c, id, backup);
	assert.ok(equal(r.progmem.subarray(0, C.app_end), sim.progmem.subarray(0, C.app_end)));
});
