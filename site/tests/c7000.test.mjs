// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 flasher modules: LZ4 and the codec slot against stubtool.py and dm32_codec.py (fixtures from
// site/tests/fixtures.py), and the stub protocol end to end against the simulated radio (fakec7000.js): the boot-ROM
// load, entry from ODR, reads with damaged frames, writes of changed sectors only, the reset back into the firmware.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal, sha256 } from "../static/js/flasher/bytes.js";
import { decodeBlock, encodeBlock } from "../static/js/flasher/lz4.js";
import { Stub, stubImages, StubError, DONE_LED } from "../static/js/flasher/stub.js";
import { Link, probe } from "../static/js/flasher/link.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { FakeC7000, c7000Simulation } from "../static/js/flasher/fakec7000.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;
const stubs = stubImages(json("c7000-stub.json"));
const quiet = () => {};

test("the embedded stub images decode to their SHA-256", async () => {
	assert.equal(await sha256(stubs.rom), stubs.romSha);
	assert.equal(await sha256(stubs.odr), stubs.odrSha);
	assert.equal(stubs.romLoad, 0x10000);
});

test("LZ4: stubtool's block decodes, and our encoder round-trips", () => {
	const plain = bin("lz4_plain.bin");
	assert.ok(equal(decodeBlock(bin("lz4_block.bin"), plain.length), plain));
	assert.ok(equal(decodeBlock(encodeBlock(plain), plain.length), plain));
	assert.throws(() => decodeBlock(bin("lz4_block.bin").subarray(0, 100), plain.length));
});

test("codec located wherever the stock firmware put it, slot as dm32_codec builds it", async () => {
	const fx = json("c7000.json");
	const c = { ...C, profiles: [fx.profile] };
	const progmem = bin("c7000_progmem.bin");
	const found = await c7.locateCodec(progmem, c, "dm1801pro");
	assert.equal(found.codeAddr, fx.found.code);
	assert.equal(found.tablesAddr, fx.found.tables);
	assert.equal(await c7.locateCodec(progmem, c, "dm32"), null);
	const image = bin("c7000_image.bin"), merged = bin("c7000_merged.bin");
	const [start, data] = c7.target(c, "dm1801pro", image, c7.slotBytes(c, "dm1801pro", found));
	assert.equal(start, 0xC000);
	assert.ok(equal(data.subarray(0, merged.length), merged), "image, padding and slot match dm32_codec.merge()");
	assert.ok(data.subarray(merged.length).every((b) => b === 0xFF), "the slot's tail is erased");
	const slot = merged.subarray(C.boards.dm1801pro.slot[0] - C.app_start);
	const ps = await c7.parseSlot(c, "dm1801pro", slot);
	assert.equal(ps.tag, 49);
	assert.equal(ps.name, "synthetic");
	const broken = slot.slice();
	broken[500] ^= 1;
	assert.equal(await c7.parseSlot(c, "dm1801pro", broken), null);
	assert.equal(await c7.parseSlot(c, "dm32", slot), null, "another radio's RAM windows");
	assert.throws(() => c7.target(c, "dm1801pro", new Uint8Array(0xE0000).fill(1)), c7.CodecError);
});

async function radio(firmware = "stock", opts = {}) {
	const sim = await c7000Simulation(C, "dm1801pro");
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200, ...opts });
	if (firmware !== "stock") {
		const odr = sim.progmem.slice(0x30000, 0x70000);
		odr.set(new TextEncoder().encode("OpenDigitalRadio"), 0x1000);
		r.progmem.set(odr, 0xC000);
		r.progmem.set(c7.slotBytes(sim.c, "dm1801pro", await c7.locateCodec(sim.progmem, sim.c, "dm1801pro")), 0xE8000);
		r.stubEntry = firmware === "odr";
	}
	return { sim, r };
}

test("a sector record that belongs to another sector is caught by the radio's checksums and read again", async (t) => {
	const { sim, r } = await radio("stock", { faults: { misplace: [0x41000, 0x9F000] } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.rom, stubs.romLoad);
	const got = await s.dump("progmem", 0x40000, 0xA0000);
	assert.ok(equal(got, sim.progmem.subarray(0x40000, 0xA0000)), "the read matches the radio");
	assert.equal(r.faults.misplace.length, 0, "both bad records were sent");
	assert.ok(s.retried >= 2, "the two sectors were read again");
});

test("boot ROM path: load, read with damaged frames, write, reset into ODR", async (t) => {
	const { sim, r } = await radio("stock", { faults: { flip: 0.05 } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.rom, stubs.romLoad);
	assert.equal(r.state, "stub");
	await s.setupLed(C.led.red, C.led.green);
	const progmem = await s.dump("progmem", 0, C.progmem_size);
	assert.ok(equal(progmem, sim.progmem), "program memory read exactly despite damaged frames");
	assert.ok(s.retried > 0, "damaged frames were asked for again");
	const what = await c7.analyze(progmem, sim.c, "dm1801pro");
	assert.equal(what.firmware, "stock");
	assert.equal(what.codec.codeAddr, sim.codec.codeAt);
	const d = await c7.decide(sim.c, "dm1801pro", { flashCodec: what.codec });
	assert.equal(d.mode, "write");
	const image = sim.progmem.slice(0x40000, 0x70000);
	image.set(new TextEncoder().encode("OpenDigitalRadio"), 0x2000);
	const [start, data] = c7.target(sim.c, "dm1801pro", image, d.slot);
	const states = new Map();
	r.faults.flip = 0;
	const res = await s.writeProgram(data, start, { appStart: C.app_start, appEnd: C.app_end, onSector: (a, st) => states.set(a, st) });
	assert.ok(res.written > 0);
	assert.ok(equal(r.progmem.subarray(0, 0xC000), sim.progmem.subarray(0, 0xC000)), "bootloader untouched");
	assert.ok(equal(r.progmem.subarray(0xC000), data), "application and slot written");
	assert.equal(states.get(0xC000), "written");
	await s.ledSequence(DONE_LED);
	await s.resetChip();
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal(r.state, "firmware");
	const p = await probe(new Link(r.stream));
	assert.equal(p.board, "DM-1801 Pro");
	assert.equal(p.codec, "dm1801pro-049");
});

test("ODR path: no buttons, keep the codec, write only what changed", async (t) => {
	const { sim, r } = await radio("odr");
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 800));
	assert.equal(r.state, "firmware");
	const link = new Link(r.stream);
	assert.equal((await probe(link)).codec, "dm1801pro-049");
	assert.ok(await Stub.odrOffer(link));
	const s = new Stub(r.stream, { log: quiet });
	await s.enterFromOdr(stubs.odr);
	assert.equal(r.state, "stub");
	const slot = await s.dump("progmem", C.boards.dm1801pro.slot[0], C.boards.dm1801pro.slot[1]);
	assert.ok(await c7.parseSlot(sim.c, "dm1801pro", slot), "the slot in flash checks");
	const image = r.progmem.slice(0xC000, 0xC000 + 0x40000);
	image[0x21000] ^= 0xFF;
	const before = r.progmem.slice(0xE8000);
	const [start, data] = c7.target(sim.c, "dm1801pro", image, null);
	const res = await s.writeProgram(data, start, { appStart: C.app_start, appEnd: C.app_end });
	assert.deepEqual(res, { written: 1, same: 63 });
	assert.ok(equal(r.progmem.subarray(0xE8000), before), "codec slot kept");
});

test("ODR without stub entry says so; the stub refuses writes outside the application", async (t) => {
	const { r } = await radio("odr-nostub");
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 800));
	assert.equal(await Stub.odrOffer(new Link(r.stream)), null);
	const s = new Stub(r.stream, { log: quiet });
	await assert.rejects(s.enterFromOdr(stubs.odr), StubError);
	await assert.rejects(s.writeProgram(new Uint8Array(4096), 0x8000, { appStart: C.app_start, appEnd: C.app_end }), /outside the application/);
});

test("no radio: waiting for the boot ROM ends with a clear error", async (t) => {
	const { r } = await radio();
	t.after(() => r.stop());
	await assert.rejects(new Stub(r.stream).waitForRom(300), /boot ROM did not answer/);
});

test("a codec from a stock file or a program memory backup", async () => {
	const { sim } = await radio();
	const got = await c7.codecFromFile(sim.progmem, sim.c, "dm1801pro");
	assert.equal(got.codeAddr, sim.codec.codeAt);
	await assert.rejects(c7.codecFromFile(new Uint8Array(1000), sim.c, "dm1801pro"), c7.CodecError);
	const slot = c7.slotBytes(sim.c, "dm1801pro", got);
	assert.equal((await c7.decide(sim.c, "dm1801pro", { savedSlot: slot })).source, "browser");
	assert.equal(await c7.decide(sim.c, "dm1801pro", {}), null);
	assert.equal((await c7.decide(sim.c, "dm1801pro", { allowNone: true })).mode, "none");
	assert.equal(crc32(slot.subarray(128, 136)), crc32(c7.sigBytes(sim.c)));
});
