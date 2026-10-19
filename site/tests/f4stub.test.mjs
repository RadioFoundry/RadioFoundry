// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 stub route (f4stub.js) against the simulated radio (fake.js): the hand-over from ODR, an install in the
// interlock's order, the MD-380's sectors, a shared sector, a cable pulled halfway, SPI flash writes, and backups through the
// loader (readers.js). Run with
// site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal } from "../static/js/flasher/bytes.js";
import { FakeRadio, simulation, randomBytes, rng } from "../static/js/flasher/fake.js";
import { F4Stub, f4Image, APP_BASE, MEM_SPI } from "../static/js/flasher/f4stub.js";
import { StubError } from "../static/js/flasher/stub.js";
import { Link } from "../static/js/flasher/link.js";
import { readRadio } from "../static/js/flasher/backup.js";
import { f4LoaderReader } from "../static/js/flasher/readers.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const stubJson = json("f4-stub.json");
const image = f4Image(stubJson);
const quiet = () => {};
const at = (radio, addr, n) => radio.flash.subarray(addr - 0x08000000, addr - 0x08000000 + n);

// An application image: a RAM stack pointer first (the bootloader checks it), then firmware-like bytes
function app(seed, size) {
	const d = randomBytes(rng(seed), size).map((b) => (b < 90 ? b : 0));
	d.set([0xFC, 0xFF, 0x01, 0x20], 0);	// 0x2001FFFC, as ODR's
	return d;
}

async function running(boardId = "dm1701", opts = {}) {
	const sim = await simulation(fb, boardId, 7);
	const radio = new FakeRadio(sim, { state: "odr", spiSize: 0x200000, ...opts });
	radio.flash.set(app(1, 0x30000), APP_BASE - 0x08000000);	// an ODR already installed
	return { sim, radio };
}

async function enter(radio) {
	const s = new F4Stub(radio.serialStream(), { log: quiet, reopen: async () => radio.serialStream() });
	await s.enterFromOdr(image, () => {}, { entry: stubJson.run });
	return s;
}

test("the embedded stub matches its checksum", async () => {
	const { createHash } = await import("node:crypto");
	assert.equal(createHash("sha256").update(image).digest("hex"), stubJson.sha256);
	assert.ok(image.length < 8192, "fits ODR's staging buffer");
});

test("hand-over: the stub adopts ODR's port, or comes back on a new one", async () => {
	for (const adopt of [true, false]) {
		const { radio } = await running("dm1701", { adopt });
		const s = await enter(radio);
		assert.equal(s.info.flashKb, 1024);
		assert.equal(s.info.spiSize, 0x200000);
		assert.equal(s.info.adopted, adopt);
		assert.deepEqual(radio.events, ["stub (from ODR)"]);
	}
});

test("a build without stub entry is refused before anything is sent", async () => {
	const { radio } = await running("dm1701", { stubEntry: false });
	await assert.rejects(enter(radio), /CONFIG_STUB_ENTRY/);
	assert.equal(radio.mode, "normal");
});

test("DM-1701 install: sector 3 erased first and committed last, codec sectors untouched, then ODR starts", async () => {
	const { sim, radio } = await running();
	const owned = fb.f4stub.boards.dm1701.owned;
	const codecBefore = sim.codecSegments.map(([a, d]) => crc32(at(radio, a, d.length)));
	const s = await enter(radio);
	const segs = [[APP_BASE, app(2, 0x48000)]];
	const r = await s.writeImage(owned, segs);
	assert.equal(radio.events[1], "erase 3", "sector 3 first");
	assert.ok(!radio.events.some((e) => /^erase (0|1|2|10|11)$/.test(e)), "bootloader and codec sectors never erased");
	assert.ok(r.written >= 3);
	assert.ok(equal(at(radio, APP_BASE, 0x48000), segs[0][1]));
	assert.ok(at(radio, APP_BASE + 0x48000, 0x1000).every((b) => b === 0xFF), "the old image's tail is gone");
	assert.deepEqual(sim.codecSegments.map(([a, d]) => crc32(at(radio, a, d.length))), codecBefore);
	await s.resetChip();
	assert.equal(radio.mode, "normal");
	assert.equal(radio.running, "odr");
	// the same image again: nothing written
	const s2 = await enter(radio);
	assert.deepEqual(await s2.writeImage(owned, segs), { written: 0, same: r.written + r.same });
});

test("MD-380: sectors 3-7 are the application's, the codec sectors stay as they are", async () => {
	const { sim, radio } = await running("md380");
	const owned = fb.f4stub.boards.md380.owned;
	assert.deepEqual(owned, [[APP_BASE, 0x08080000]]);
	const s = await enter(radio);
	const codec = at(radio, 0x08080000, 0x80000).slice();
	const kept = [];
	await s.writeImage(owned, [[APP_BASE, app(4, 0x74000)]], { onKept: async (sec) => kept.push(sec) });
	assert.equal(kept.length, 0, "no sector shared with the codec");
	assert.ok(equal(at(radio, APP_BASE, 0x74000), app(4, 0x74000)));
	assert.ok(equal(at(radio, 0x08080000, 0x80000), codec), "sectors 8-11 unchanged");
	for (const [a, d] of sim.codecSegments) assert.ok(equal(at(radio, a, d.length), d));
});

test("a sector shared with other data: a copy is handed out first and the other bytes stay", async () => {
	// No radio has one since the codec moved; the rule stays for one that does
	const { radio } = await running("md380");
	const owned = [[APP_BASE, 0x08050C44]];
	const s = await enter(radio);
	const sec6 = at(radio, 0x08040000, 0x20000).slice();
	const kept = [];
	await s.writeImage(owned, [[APP_BASE, app(4, 0x44000)]], { onKept: async (sec) => kept.push(sec) });
	assert.equal(kept.length, 1);
	assert.equal(kept[0].index, 6);
	assert.ok(equal(kept[0].kept, sec6), "the copy is sector 6 as it was");
	assert.ok(equal(at(radio, 0x08050C44, 0x08060000 - 0x08050C44), sec6.subarray(0x10C44)), "the rest of sector 6 is unchanged");
});

test("images outside the owned flash, or without a stack pointer, are refused before anything is erased", async () => {
	const { radio } = await running();
	const s = await enter(radio);
	const owned = fb.f4stub.boards.dm1701.owned;
	await assert.rejects(s.writeImage(owned, [[APP_BASE, app(5, 0xB8000)]]), /outside the flash the firmware owns/);
	await assert.rejects(s.writeImage(owned, [[APP_BASE, app(5, 0x1000)], [0x080C0000, app(5, 0x1000)]]), /outside the flash the firmware owns/);
	await assert.rejects(s.writeImage(owned, [[APP_BASE, new Uint8Array(0x1000)]]), /stack pointer/);
	assert.ok(!radio.events.some((e) => e.startsWith("erase")));
});

test("a cable pulled halfway leaves the radio in its bootloader, and a second run finishes the job", async () => {
	const { radio } = await running("dm1701", { cutAfter: 30 });
	const owned = fb.f4stub.boards.dm1701.owned;
	const segs = [[APP_BASE, app(6, 0x48000)]];
	const s = await enter(radio);
	await assert.rejects(s.writeImage(owned, segs));
	radio.cutAfter = null;
	radio.powerOn();
	assert.equal(radio.mode, "bootloader", "nothing committed: the bootloader stays in DFU");
	// the bootloader route would finish it; here ODR's image is put back by hand and the stub runs again
	radio.flash.set(app(1, 0x30000), APP_BASE - 0x08000000);
	radio.powerOn();
	const s2 = await enter(radio);
	await s2.writeImage(owned, segs);
	assert.ok(equal(at(radio, APP_BASE, 0x48000), segs[0][1]));
});

test("SPI flash: only changed sectors are written, then verified; outside the chip is refused", async () => {
	const { radio } = await running();
	const s = await enter(radio);
	const data = radio.spi.slice(0, 0x20000);
	data.set(randomBytes(rng(9), 0x1800), 0x5000);
	assert.deepEqual(await s.spiWrite(data, 0), { written: 2, same: 30 });
	assert.ok(equal(radio.spi.subarray(0, 0x20000), data));
	assert.deepEqual(await s.diff(data, 0, MEM_SPI), []);
	const back = await s.readFlash(0x4000, 0x4000, { mem: MEM_SPI });
	assert.ok(equal(back, data.subarray(0x4000, 0x8000)));
	await assert.rejects(s.spiWrite(new Uint8Array(0x2000), 0x1FF000), StubError);
});

// A backup through the loader the way the backup step runs it
async function backupVia(radio, sim, what) {
	const link = new Link(radio.serialStream());
	let handedOver = false;
	const loader = f4LoaderReader(link, { stubJson: async () => stubJson, reopen: async () => radio.serialStream(), onHandOver: () => { handedOver = true; } });
	const parts = await readRadio(link, sim.board, { backup: { spi_size: radio.spi.length } }, { what, readers: [loader] });
	return { parts, loader, handedOver, get: (w) => parts.find((x) => x.what === w) };
}

test("backup through the loader: the security registers over the debug link, both memories through the loader, then ODR again", async () => {
	for (const adopt of [true, false]) {
		const { sim, radio } = await running("dm1701", { adopt });
		const b = await backupVia(radio, sim, {});
		assert.ok(b.loader.used && b.handedOver);
		assert.deepEqual(radio.events, ["stub (from ODR)", "reset"]);
		assert.equal(radio.mode, "normal", "the radio runs ODR again");
		assert.ok(b.get("security-registers").complete, "read over the debug link before the hand-over");
		const internal = b.get("internal-flash");
		assert.equal(internal.addr, APP_BASE);
		assert.ok(internal.complete);
		assert.deepEqual(internal.data, radio.flash.subarray(APP_BASE - 0x08000000));
		const spi = b.get("spi-flash");
		assert.ok(spi.complete);
		assert.deepEqual(spi.data, radio.spi);
	}
});

test("backup without stub entry: the loader doesn't start and the debug link reads everything, ODR untouched", async () => {
	const { sim, radio } = await running("dm1701", { stubEntry: false });
	const b = await backupVia(radio, sim, { spi: "none" });
	assert.ok(!b.loader.used && !b.handedOver);
	assert.deepEqual(radio.events, []);
	assert.deepEqual(b.get("internal-flash").data, radio.flash.subarray(APP_BASE - 0x08000000));
	assert.equal(b.get("spi-flash"), undefined);
});

test("a codeplug-only backup with no reader reads over the debug link", async () => {
	const { sim, radio } = await running("dm1701");
	const parts = await readRadio(new Link(radio.serialStream()), sim.board, { backup: { spi_size: radio.spi.length } }, { what: { internal: false, spi: "codeplug" } });
	assert.deepEqual(radio.events, []);
	assert.equal(parts.find((x) => x.what === "spi-flash").data.length, 0x120000);
});

test("a read that fails inside the loader restarts the radio before the error goes up", async () => {
	const { sim, radio } = await running("dm1701");
	const link = new Link(radio.serialStream());
	const loader = f4LoaderReader(link, { stubJson: async () => stubJson, reopen: async () => radio.serialStream() });
	const read = loader.read;
	loader.read = async (mem, ...rest) => { if (mem === "spi") throw new StubError("cable trouble"); return read(mem, ...rest); };
	await assert.rejects(readRadio(link, sim.board, { backup: { spi_size: radio.spi.length } }, { readers: [loader] }), /cable trouble/);
	assert.deepEqual(radio.events, ["stub (from ODR)", "reset"]);
	assert.equal(radio.mode, "normal");
});

