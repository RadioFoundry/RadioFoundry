// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 flasher modules: LZ4 and the codec slot against stubtool.py and hrc7000_codec.py (fixtures from
// site/tests/fixtures.py), and the stub protocol end to end against the simulated radio (fakec7000.js): the boot-ROM
// load, entry from ODR, reads with damaged frames, writes of changed sectors only, the reset back into the firmware.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal, sha256, sleep } from "../static/js/flasher/bytes.js";
import { decodeBlock, encodeBlock } from "../static/js/flasher/lz4.js";
import { Stub, stubImages, StubError, DONE_LED, univFits, univLayout, patchUniv, boardLed, RX2_F } from "../static/js/flasher/stub.js";
import { Link, probe } from "../static/js/flasher/link.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { FakeC7000, c7000Simulation } from "../static/js/flasher/fakec7000.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;
const stubs = stubImages(json("c7000-stub.json"));
const quiet = () => {};

test("the GD-88's stub images are patched as stubtool patches them; other radios get the image unchanged", () => {
	for (const id of ["gd88", "gd88b"]) {
		const got = patchUniv(stubs.image, C.boards[id].stub_board);
		assert.ok(equal(got, bin(`c7000_stub_${id}.bin`)), id);
	}
	assert.deepEqual(boardLed(C, "gd88"), { red: 255, green: 255 });
	assert.deepEqual(boardLed(C, "gd88b"), { red: 16, green: 49 });
	assert.equal(C.boards.gd88b.stub_board.uart, 0x14090000);
	assert.equal(C.boards.gd88b.stub_board.bridge, true);
	for (const id of ["dm32", "dm1801pro", "db25d"]) {
		assert.equal(patchUniv(stubs.image, C.boards[id].stub_board), stubs.image, id);
		assert.deepEqual(boardLed(C, id), C.led);
	}
});

test("the universal stub decodes to its SHA-256 and fits every way in", async () => {
	assert.equal(await sha256(stubs.image), stubs.sha);
	assert.equal(stubs.load, 0x10000);
	assert.deepEqual(univLayout(stubs.image), [stubs.run, stubs.end]);
	assert.equal(stubs.run, 0x30000);
	// The stub feeds the watchdog in its wait and read loops; ODR leaves it running (docs/spec/watchdog.md 3.2): the
	// feed value 0x55AADD22 is in the image
	assert.ok(Buffer.from(stubs.image).includes(Buffer.from([0x22, 0xDD, 0xAA, 0x55])), "the stub feeds the watchdog");
	// boot ROM / first stage, ODR on the DM-32UV and DM-1801 Pro, the DB25-D with and without DMR, a stock DB25-D's console
	for (const [load, win] of [[0x10000, null], [0x17000, [0x17000, 0x18000]], [0x1D000, [0x1D000, 0x12000]], [0x30000, [0x30000, 0x20000]], [0x54000, null]])
		univFits(stubs.image, load, win);
	assert.throws(() => univFits(stubs.image, 0x30800), StubError);		// overlaps its run copy
	assert.throws(() => univFits(stubs.image, 0x57000), StubError);		// past the end of SRAM
	assert.throws(() => univFits(stubs.image, 0x1D000, [0x1D000, 0x1000]), StubError);
	assert.throws(() => univFits(new Uint8Array(4096), 0x10000), StubError);
});

test("LZ4: stubtool's block decodes, and our encoder round-trips", () => {
	const plain = bin("lz4_plain.bin");
	assert.ok(equal(decodeBlock(bin("lz4_block.bin"), plain.length), plain));
	assert.ok(equal(decodeBlock(encodeBlock(plain), plain.length), plain));
	assert.throws(() => decodeBlock(bin("lz4_block.bin").subarray(0, 100), plain.length));
});

test("codec located wherever the stock firmware put it, slot as hrc7000_codec builds it", async () => {
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
	assert.ok(equal(data.subarray(0, merged.length), merged), "image, padding and slot match hrc7000_codec.merge()");
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

test("GD-88 receiver B: ODR on the first SoC hands the cable over, the stub loads through the bridge", async (t) => {
	const { r } = await radio("odr", { rx2: true, board: "GD-88" });
	t.after(() => r.stop());
	r.powerOn();
	await sleep(800);
	const link = new Link(r.stream, { timeout: 1000 });
	assert.equal((await probe(link)).board, "GD-88");
	assert.equal(await Stub.bridgeFromOdr(link), 0x0F);
	const s = new Stub(r.stream, { log: quiet });
	await s.waitForRom(3000);
	const image = patchUniv(stubs.image, C.boards.gd88b.stub_board);
	await s.loadThroughRom(image, stubs.load);
	assert.ok(r.events.includes("bridge") && r.state === "stub");
	assert.ok(equal(r.loaded.subarray(0, image.length), image), "B got the patched image");
	await s.configure(C.boards.gd88b.stub_board, boardLed(C, "gd88b"));
	assert.equal(r.stubCfg.board, 5, "the gd88b profile reached the stub");
	// a refused token, and a radio without a second receiver
	const plain = await radio("odr");
	t.after(() => plain.r.stop());
	plain.r.powerOn();
	await sleep(800);
	await assert.rejects(Stub.bridgeFromOdr(new Link(plain.r.stream, { timeout: 300 })), /route to the second receiver/);
});

test("GD-88 receiver B on its original firmware: the first SoC reports it down and the bridge reaches nothing", async (t) => {
	const { r } = await radio("odr", { rx2: { stock: true }, board: "GD-88" });
	t.after(() => r.stop());
	r.powerOn();
	await sleep(800);
	const link = new Link(r.stream, { timeout: 1000 });
	const flags = await Stub.secondState(link);
	assert.ok(flags & RX2_F.present && !(flags & RX2_F.up), "present, not up: the page offers update mode (IAP_B)");
	await Stub.bridgeFromOdr(link);
	await assert.rejects(new Stub(r.stream, { log: quiet }).waitForRom(600));
	assert.ok(!r.events.includes("bridge"));
});

test("a sector record that belongs to another sector is caught by the radio's checksums and read again", async (t) => {
	const { sim, r } = await radio("stock", { faults: { misplace: [0x41000, 0x9F000] } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
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
	await s.loadThroughRom(stubs.image, stubs.load);
	assert.equal(r.state, "stub");
	await s.configure(C.boards.dm1801pro.stub_board, C.led);
	assert.equal(r.stubCfg.board, 2, "the DM-1801 Pro profile reached the stub");
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
	const ev = r.events.filter((e) => e === "wdt-off" || e === "reset");
	assert.deepEqual(ev.slice(-2), ["wdt-off", "reset"], "the watchdog goes off before the soft reset");
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal(r.state, "firmware");
	const p = await probe(new Link(r.stream));
	assert.equal(p.board, "DM-1801 Pro");
	assert.equal(p.codec, "dm1801pro-049");
});

test("simulated radio: after the stub's reset the host link stays silent until a power cycle, as on the real radios", async (t) => {
	const { r } = await radio("odr", { faults: { silentAfterReset: true } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	await s.resetChip();
	const ev = r.events.filter((e) => e === "wdt-off" || e === "reset");
	assert.deepEqual(ev.slice(-2), ["wdt-off", "reset"], "the watchdog goes off before the soft reset");
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal(r.state, "firmware");
	assert.equal(await probe(new Link(r.stream, { timeout: 500 })), null, "no answer after the soft reset");
	r.powerOff();
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal((await probe(new Link(r.stream))).board, "DM-1801 Pro", "answers after the power cycle");
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
	await s.enterFromOdr(stubs.image);
	assert.equal(r.state, "stub");
	assert.ok(s.v6 && s.v6.run === 0x30000, "HELLO v6 reports the run address");
	const image0 = r.progmem.slice(0xC000, 0xC000 + 0x40000);
	const [st0, d0] = c7.target(sim.c, "dm1801pro", image0, null);
	await assert.rejects(s.writeProgram(d0, st0, { appStart: C.app_start, appEnd: C.app_end }), /configuration/, "no write before CONFIG");
	await s.configure(C.boards.dm1801pro.stub_board, C.led);
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
	await assert.rejects(s.enterFromOdr(stubs.image), StubError);
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
	// a codec slot that came with the user's firmware file has no name of its own: its text must not print an empty one
	const fromImage = await c7.decide(sim.c, "dm1801pro", { fileCodec: slot });
	assert.equal(fromImage.source, "image");
	const text = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url)));
	for (const src of ["radio", "browser", "file", "image"]) assert.ok(text[`flash.c7.codec.from_${src}`], src);
	assert.ok(!text["flash.c7.codec.from_image"].includes("{name}"));
	assert.equal(await c7.decide(sim.c, "dm1801pro", {}), null);
	assert.equal((await c7.decide(sim.c, "dm1801pro", { allowNone: true })).mode, "none");
	assert.equal(crc32(slot.subarray(128, 136)), crc32(c7.sigBytes(sim.c)));
});

test("factory calibration: the first page of the stock codeplug area whose ID byte is 2, and only in that area", () => {
	const area = new Uint8Array(c7.CAL_AREA_END + 0x2000).fill(0xFF);
	assert.equal(c7.findCalibration(area), -1);
	area[0x0FFF] = 2;				// page 0 isn't a codeplug page
	area[c7.CAL_AREA_END + 0xFFF] = 2;		// past the area
	assert.equal(c7.findCalibration(area), -1);
	area[0x7CFFF] = 2;
	area[0x9CFFF] = 2;
	assert.equal(c7.findCalibration(area), 0x7C000);
	// the simulated radio carries one where the DM-1801 Pro examined had it
	assert.equal(c7.findCalibration(new FakeC7000({ progmem: new Uint8Array(0x100000) }).q128), 0x7C000);
});

// Slots (docs/spec/codec-placement.md): the same bytes as hrc7000_codec.py, read back; a version 1 slot from before the
// code moved, and a build without a codec map, are refused
test("a codec with a map gets hrc7000_codec.py's slot; a version 1 slot or a build without a map is refused", async () => {
	const fx = json("c7000.json").v2;
	const want = bin("c7000_slot_v2.bin");
	const c = { ...C, codec_maps: { [fx.map.profile]: fx.map }, boards: { ...C.boards, db25d: { ...C.boards.db25d, run: fx.run } } };
	const r = await c7.parseSlot(c, "db25d", want);
	assert.ok(r && r.version === 2 && r.length === want.length);
	const w = (i) => want[4 * i] | (want[4 * i + 1] << 8) | (want[4 * i + 2] << 16) | (want[4 * i + 3] * 2 ** 24);
	const code = want.slice(w(2), w(2) + w(3)), tables = want.slice(w(5), w(5) + w(6));
	const profile = { name: "synthetic", run: w(4), tables_ram: w(7), entries: Array.from({ length: 7 }, (_, i) => w(8 + i)), tag: w(15) };
	const got = { profile, code, tables, codeSha: await sha256(code) };
	assert.ok(equal(c7.slotBytes(c, "db25d", got), want), "the same bytes as hrc7000_codec.py");
	assert.equal(await c7.parseSlot({ ...c, boards: { ...c.boards, db25d: { ...c.boards.db25d, run: [fx.run[0], fx.run[0] + 0x1000] } } }, "db25d", want), null,
		"a codec bigger than the window is refused");
	const v1 = want.slice();
	v1[4] = 1;
	assert.equal(await c7.parseSlot(c, "db25d", v1), null, "a version 1 slot is refused");
	assert.equal(await c7.decide(c, "db25d", { savedSlot: v1 }), null, "and isn't written from a saved copy");
	assert.throws(() => c7.slotBytes({ ...c, codec_maps: {} }, "db25d", got), /no codec map/);
	const bad = code.slice();
	bad.fill(0, 0x40, 0x44);
	assert.throws(() => c7.slotBytes(c, "db25d", { ...got, code: bad }), /doesn't hold/);
});
