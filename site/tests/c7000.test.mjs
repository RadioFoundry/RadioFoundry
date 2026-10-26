// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 flasher modules: LZ4 and the codec slot against stubtool.py and hrc7000_codec.py (fixtures from
// site/tests/fixtures.py), and the stub protocol end to end against the simulated radio (fakec7000.js): the boot-ROM
// load, entry from RadioFoundry, reads with damaged frames, writes of changed sectors only, the reset back into the firmware.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal, sha256 } from "../static/js/flasher/bytes.js";
import { decodeBlock, encodeBlock } from "../static/js/flasher/lz4.js";
import { Stub, stubImages, StubError, DONE_LED, univFits, univLayout } from "../static/js/flasher/stub.js";
import { Link, probe } from "../static/js/flasher/link.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { FakeC7000, c7000Simulation, fakeC7000Options } from "../static/js/flasher/fakec7000.js";
import { stockVersion } from "../static/js/flasher/usmart.js";
import { openVendorFile } from "../static/js/flasher/vendorfile.js";
import { vendorBundle, hasC7Backup, backupChoices } from "../static/js/flasher/c7000ui.js";
import * as explain from "../static/js/flasher/explain.js";
import * as crumbs from "../static/js/flasher/trail.js";
import * as diag from "../static/js/diag.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;
const stubs = stubImages(json("c7000-stub.json"));
const quiet = () => {};

test("the universal stub decodes to its SHA-256 and fits every way in", async () => {
	assert.equal(await sha256(stubs.image), stubs.sha);
	assert.equal(stubs.load, 0x10000);
	assert.deepEqual(univLayout(stubs.image), [stubs.run, stubs.end]);
	assert.equal(stubs.run, 0x30000);
	// The stub feeds the watchdog in its wait and read loops; RadioFoundry leaves it running (docs/spec/watchdog.md 3.2): the
	// feed value 0x55AADD22 is in the image
	assert.ok(Buffer.from(stubs.image).includes(Buffer.from([0x22, 0xDD, 0xAA, 0x55])), "the stub feeds the watchdog");
	// boot ROM / first stage, RadioFoundry on the DM-32UV and DM-1801 Pro, the DB25-D with and without DMR, a stock DB25-D's console
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

test("#sim=<board>: every C7 stub radio's fake comes from its data", async () => {
	const FB = json("flash-boards.json");
	const radios = json("radios.json").radios.filter((r) => r.flash.backend === "c7000-stub");
	assert.ok(radios.length > 0);
	for (const r of radios) {
		const o = fakeC7000Options(r, FB);
		assert.ok(FB.boards[r.id].debuglink_names.includes(o.board), r.id);
		assert.equal(o.codecId, FB.boards[r.id].codec.id, r.id);
		const sim = await c7000Simulation(C, r.id, 7000, r);
		const fs = r.flash.first_stage;
		const head = [...sim.progmem.subarray(C.app_start, C.app_start + 4)].map((x) => x.toString(16).padStart(2, "0")).join("");
		if (fs) assert.equal(head, fs.head, `${r.id}: the stock application starts as the vendor loader checks`);
		// a stock console's version string is where the console's build check reads it
		if (r.flash.stock_console) {
			assert.equal(o.usmart, r.flash.stock_versions[0].slice(-3), r.id);
			assert.equal(stockVersion(sim.progmem), r.flash.stock_versions[0], r.id);
		} else assert.equal(o.usmart, null, r.id);
	}
});

async function radio(firmware = "stock", opts = {}) {
	const sim = await c7000Simulation(C, "dm1801pro");
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200, ...opts });
	if (firmware !== "stock") {
		const rfoundry = sim.progmem.slice(0x30000, 0x70000);
		rfoundry.set(new TextEncoder().encode("RadioFoundry"), 0x1000);
		r.progmem.set(rfoundry, 0xC000);
		r.progmem.set(c7.slotBytes(sim.c, "dm1801pro", await c7.locateCodec(sim.progmem, sim.c, "dm1801pro")), 0xE8000);
		r.stubEntry = firmware === "rfoundry";
	}
	return { sim, r };
}

test("firmware flashed before the project was renamed is still recognized as RadioFoundry", async () => {
	const sim = await c7000Simulation(C, "dm1801pro");
	const image = sim.progmem.slice();
	image.set(new TextEncoder().encode("OpenDigitalRadio"), C.app_start + 0x1000);
	assert.equal((await c7.analyze(image, sim.c, "dm1801pro")).firmware, "rfoundry");
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

test("boot ROM load through a cable that echoes what it sends: the echo is dropped and the load goes on", async (t) => {
	const { r } = await radio("stock", { faults: { romEcho: true } });
	t.after(() => r.stop());
	const lines = [];
	const s = new Stub(r.stream, { log: (l) => lines.push(l) });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	assert.equal(r.state, "stub");
	assert.ok(s.echo, "the echo was seen");
	assert.ok(lines.some((l) => /echoes/.test(l)), "and logged once");
	assert.equal(lines.filter((l) => /echoes what/.test(l)).length, 1);
});

test("boot ROM load answered with something else: the error has the bytes, the cable and the way out", async (t) => {
	const { r } = await radio("stock", { faults: { romNak: true } });
	t.after(() => r.stop());
	const lines = [];
	const s = new Stub(r.stream, { log: (l) => lines.push(l), cable: "CH340" });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await assert.rejects(s.loadThroughRom(stubs.image, stubs.load), (e) => {
		assert.ok(e instanceof StubError);
		assert.match(e.message, /stub block 0 was not acknowledged \(received 02 0a 00 03\)/);
		assert.match(e.message, /CH340 programming cable/);
		assert.match(e.message, /FTDI, CP210x or PL2303/);
		assert.match(e.message, /stock firmware file/);
		return true;
	});
	assert.ok(lines.some((l) => /not acknowledged \(received 02 0a 00 03\), sending it again/.test(l)), "each try logged with its bytes");
});

test("boot ROM ack read: an exact echo is dropped, a partial one waits, anything else fails with its bytes", async () => {
	const stream = { buf: new Uint8Array(0), take(n) { const b = this.buf.slice(0, n); this.buf = this.buf.slice(n); return b; } };
	const s = new Stub(stream, { log: quiet });
	const frame = new Uint8Array([0x02, 0x1B, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00]);
	stream.buf = new Uint8Array([...frame, 0x02, 0x05, 0x00, 0x03]);
	assert.deepEqual(await s.romAck(frame, 200), { ok: true, got: null });
	assert.ok(s.echo);
	stream.buf = new Uint8Array([0x02, 0x05, 0x00, 0x03]);
	assert.equal((await s.romAck(frame, 200)).ok, true, "no echo: the ack alone");
	stream.buf = frame.slice(0, 5);
	setTimeout(() => { stream.buf = new Uint8Array([...stream.buf, ...frame.slice(5), 0x02, 0x05, 0x00, 0x03]); }, 20);
	assert.equal((await s.romAck(frame, 500)).ok, true, "an echo arriving in pieces");
	stream.buf = new Uint8Array([0x02, 0x1B, 0x01, 0x03, 0x00, 0x01, 0x00, 0x07, 0x02, 0x05, 0x00, 0x03]);
	const bad = await s.romAck(frame, 200);
	assert.equal(bad.ok, false, "a near-echo is not an echo");
	assert.deepEqual([...bad.got.slice(0, 4)], [0x02, 0x1B, 0x01, 0x03]);
	stream.buf = new Uint8Array(0);
	assert.deepEqual(await s.romAck(frame, 30), { ok: false, got: new Uint8Array(0) }, "silence");
});

test("boot ROM path: load, read with damaged frames, write, reset into RadioFoundry", async (t) => {
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
	image.set(new TextEncoder().encode("RadioFoundry"), 0x2000);
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
	const ev = r.events.filter((e) => e === "soft-mark" || e === "wdt-off" || e === "reset");
	assert.deepEqual(ev.slice(-3), ["soft-mark", "wdt-off", "reset"], "the restart-cause mark, then the watchdog off, then the soft reset");
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal(r.state, "firmware");
	const p = await probe(new Link(r.stream));
	assert.equal(p.board, "DM-1801 Pro");
	assert.equal(p.codec, "dm1801pro-049");
});

test("simulated radio: after the stub's reset the host link stays silent until a power cycle, as on the real radios", async (t) => {
	const { r } = await radio("rfoundry", { faults: { silentAfterReset: true } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	await s.resetChip();
	const ev = r.events.filter((e) => e === "soft-mark" || e === "wdt-off" || e === "reset");
	assert.deepEqual(ev.slice(-3), ["soft-mark", "wdt-off", "reset"], "the restart-cause mark, then the watchdog off, then the soft reset");
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal(r.state, "firmware");
	assert.equal(await probe(new Link(r.stream, { timeout: 500 })), null, "no answer after the soft reset");
	r.powerOff();
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 1500));
	assert.equal((await probe(new Link(r.stream))).board, "DM-1801 Pro", "answers after the power cycle");
});

test("RadioFoundry path: no buttons, keep the codec, write only what changed", async (t) => {
	const { sim, r } = await radio("rfoundry");
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 800));
	assert.equal(r.state, "firmware");
	const link = new Link(r.stream);
	assert.equal((await probe(link)).codec, "dm1801pro-049");
	assert.ok(await Stub.rfoundryOffer(link));
	const s = new Stub(r.stream, { log: quiet });
	await s.enterFromRFoundry(stubs.image);
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

test("RadioFoundry without stub entry says so; the stub refuses writes outside the application", async (t) => {
	const { r } = await radio("rfoundry-nostub");
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 800));
	assert.equal(await Stub.rfoundryOffer(new Link(r.stream)), null);
	const s = new Stub(r.stream, { log: quiet });
	await assert.rejects(s.enterFromRFoundry(stubs.image), StubError);
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
	// what each failure is, for the page's explanation (explain.js): not a vendor file, another radio's backup, no map
	await assert.rejects(c7.codecFromFile(new Uint8Array(1000), sim.c, "dm1801pro"), (e) => e.code === "not-vendor");
	await assert.rejects(c7.codecFromFile(new Uint8Array(sim.progmem.length), sim.c, "dm1801pro"), (e) => e.code === "not-vendor");
	assert.throws(() => c7.slotBytes({ ...sim.c, codec_maps: [] }, "dm1801pro", got), (e) => e.code === "unmapped");
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

test("the original firmware goes back from its vendor file, without a backup", async (t) => {
	const { sim, r } = await radio("rfoundry");
	t.after(() => r.stop());
	// a vendor file as the manufacturer ships it: the 256-byte BFUV32-V2 header, then the application
	let end = C.app_end;
	while (end > C.app_start && sim.progmem[end - 1] === 0xFF) end--;
	const file = new Uint8Array(256 + end - C.app_start).fill(0);
	file.set(new TextEncoder().encode("BFUV32-V2"), 0);
	file.set(new TextEncoder().encode("END"), 0xFD);
	file.set(sim.progmem.subarray(C.app_start, end), 256);
	const fb = { ...json("flash-boards.json"), c7000: sim.c };
	const opened = await openVendorFile("DM1801Pro.bin", file, fb, "dm1801pro");
	assert.equal(opened.best?.match.board, "dm1801pro", "identified for this radio");
	const bundle = vendorBundle({ id: "dm1801pro", name: "DM-1801 Pro", flash: { board: "dm1801pro" } }, opened.best, sim.c);
	assert.ok(hasC7Backup(bundle), "the restore task takes it as it takes a backup");
	assert.equal(bundle.meta.vendor_file, "DM1801Pro.bin");
	const [start, data] = c7.restoreTarget(sim.c, bundle.parts.get("c7000-progmem").data);
	assert.equal(start, C.app_start);
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	await s.configure(C.boards.dm1801pro.stub_board, C.led);
	assert.ok(!equal(r.progmem.subarray(C.app_start, C.app_end), sim.progmem.subarray(C.app_start, C.app_end)), "RadioFoundry runs before");
	await s.writeProgram(data, start, { appStart: C.app_start, appEnd: C.app_end });
	assert.ok(equal(r.progmem.subarray(0, C.app_end), sim.progmem.subarray(0, C.app_end)), "the original firmware is back, the codec slot erased");
	// a file larger than the application area is refused, as a fault in the page (the file was identified)
	assert.throws(() => c7.progmemFromFile(sim.c, "dm1801pro", new Uint8Array(256 + C.app_end).fill(1)), (e) => e.code === "internal");
	// a build whose codec no profile describes has the signature every C-SKY radio's codec starts with, which tells no
	// radio: it is credited to none (not to the page's radio), explained as unmapped, and never written back
	const changed = file.slice();
	changed[256 + sim.codec.codeAt - C.app_start + 16] ^= 0xFF;
	for (const board of Object.keys(sim.c.boards)) {
		const o = await openVendorFile("DM1801Pro-new.bin", changed, fb, board);
		assert.equal(o.best, null, board);
		assert.equal(o.files[0].match.board, null);
		assert.equal(explain.pickProblem(o, board, { need: "key" }).problem, "unmapped", board);
	}
	const forged = { path: "x.bin", data: changed, match: { board: "dm1801pro", kind: "codec-unmapped" } };
	assert.throws(() => vendorBundle({ id: "dm1801pro", name: "DM-1801 Pro", flash: { board: "dm1801pro" } }, forged, sim.c), (e) => e.code === "unmapped");
	// on an STM32 radio's page it is no vendor file of that radio
	assert.equal(explain.pickProblem(await openVendorFile("DM1801Pro-new.bin", changed, fb, "md380"), "md380").problem, "not-vendor");
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

test("backup choices: the firmware memory on a first install, never all channels and settings unless asked", () => {
	assert.deepEqual(backupChoices(true, true), { progmem: true, cal: true, q128: false });
	assert.deepEqual(backupChoices(false, true), { progmem: false, cal: true, q128: false });
	assert.deepEqual(backupChoices(true, false), { progmem: true, cal: false, q128: false });
});

test("read times: pessimistic, from the radio's count of blank sectors, else the board's rate, else every sector used", () => {
	const MB = 0x100000;
	// a data memory 98 % blank: 82 used sectors whole (336 KB) and 4096 records, 43 s on the wire, plus the checksum pass
	const data = c7.readSeconds(16 * MB, { sizing: { sectors: 4096, blank: 4014, ms: 6000 } });
	assert.equal(data.upTo, false);
	assert.ok(data.seconds > 45 && data.seconds < 52, `${data.seconds}`);
	// half counted: the share counted stands for the whole range, and the checksum time grows with it
	const half = c7.readSeconds(16 * MB, { sizing: { sectors: 2048, blank: 2007, ms: 3000 } });
	assert.ok(Math.abs(half.seconds - data.seconds) < 1);
	// a full program memory: about 2 min
	const pm = c7.readSeconds(MB, { sizing: { sectors: 256, blank: 0, ms: 1000 } });
	assert.ok(pm.seconds > 110 && pm.seconds < 125, `${pm.seconds}`);
	// a measured rate replaces the bench one, but never beats the line
	assert.ok(c7.readSeconds(MB, { sizing: { sectors: 256, blank: 0, ms: 0 }, rate: 4500 }).seconds > 2 * 115);
	assert.ok(c7.readSeconds(MB, { sizing: { sectors: 256, blank: 0, ms: 0 }, rate: 1e6 }).seconds > MB / 11520);
	assert.deepEqual(c7.readSeconds(2 * MB, { plainRate: 46000 }), { seconds: 2 * MB / 46000, upTo: false });
	// nothing known: every sector used, about 31 min for 16 MB, said as "up to"
	const worst = c7.readSeconds(16 * MB);
	assert.equal(worst.upTo, true);
	assert.ok(worst.seconds > 30 * 60 && worst.seconds < 33 * 60, `${worst.seconds}`);
});

test("the radio counts its blank sectors without sending data; a deadline or a stop ends the count early", async (t) => {
	const { sim, r } = await radio();
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	const want = (mem, from, to) => {
		let n = 0;
		for (let a = from; a < to; a += 0x1000) n += mem.subarray(a, a + 0x1000).every((x) => x === 0xFF);
		return n;
	};
	const pm = await c7.blankCount(s, "progmem", 0, 0x100000);
	assert.deepEqual([pm.sectors, pm.blank], [256, want(sim.progmem, 0, 0x100000)]);
	const dm = await c7.blankCount(s, "q128", 0, 0x200000);
	assert.deepEqual([dm.sectors, dm.blank], [512, want(r.q128, 0, 0x200000)]);
	let n = 0;
	const stopped = await c7.blankCount(s, "q128", 0, 0x200000, { stop: () => ++n > 3 });
	assert.equal(stopped.sectors, 3 * 16, "three windows, then stopped");
	assert.equal(await c7.blankCount(s, "q128", 0, 0x200000, { deadline: -1 }), null, "nothing counted past the deadline");
	// the stub still answers a read right after
	assert.ok(equal(await s.dump("progmem", 0, 0x2000), sim.progmem.subarray(0, 0x2000)));
});

test("a firmware memory read stopped for the stock file: the stub stays usable and the codec slot reads after it", async (t) => {
	const { r } = await radio("rfoundry");
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: quiet });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	const ac = new AbortController();
	await assert.rejects(s.dump("progmem", 0, 0x100000, { signal: ac.signal, progress: (d) => { if (d >= 0x20000) ac.abort(); } }), /stopped/);
	await s.drain();
	const lay = C.boards.dm1801pro;
	const slot = await s.dump("progmem", lay.slot[0], lay.slot[1]);
	assert.ok(equal(slot, r.progmem.subarray(lay.slot[0], lay.slot[1])), "the slot after the stop is the radio's");
});

test("trail of a cable radio run: the boot ROM load (an echoing cable), the choices, the codec decision", async (t) => {
	diag.reset();
	const debug = console.debug;
	console.debug = () => {};
	t.after(() => { console.debug = debug; });
	const { sim, r } = await radio("stock", { faults: { romEcho: true } });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: (l) => diag.note(l), cable: "CH340" });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	diag.note(...crumbs.c7Check({ progmem: true, cal: true, q128: false }, { skipFirmware: false, firstInstall: true, how: "rom" }));
	const what = await c7.analyze(sim.progmem, sim.c, "dm1801pro");
	diag.note(...crumbs.c7Decision(await c7.decide(sim.c, "dm1801pro", { flashCodec: what.codec }), what, null));
	diag.note(...crumbs.c7Decision(null, { firmware: "stock", notRead: true }, null));
	const tr = diag.trail(200);
	assert.ok(tr.some((l) => /the cable echoes what the page sends; the echo is ignored/.test(l)));
	assert.ok(tr.some((l) => /cable radio check \{"progmem":true,"cal":true,"q128":false,"skipFirmware":false,"firstInstall":true,"how":"rom"\}/.test(l)));
	assert.ok(tr.some((l) => /cable radio codec decision \{"mode":"[a-z]+"/.test(l)));
	assert.ok(tr.some((l) => /cable radio codec decision \{"mode":null,"firmware":"stock","notRead":true,"file":false\}/.test(l)));
});
