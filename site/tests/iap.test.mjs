// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The IAP route of the web flasher (the stock console and the vendor IAP loader): the codec carved from a plain stock file against hrc7000_codec.py (fixtures
// from site/tests/fixtures.py), the stub loaded through the stock console and the IAP write against simulated radios, and the stub choice by RAM span.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

import { equal, sha256, be32, concat } from "../static/js/flasher/bytes.js";
import { stubImages, univFits, StubError } from "../static/js/flasher/stub.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { Usmart, HIJACK, UsmartError, stockVersion, stockName, BUILDS, STUB_WINDOW, stubTarget, UNKNOWN_BUILD, versionOf, buildsFor } from "../static/js/flasher/usmart.js";
import {
	Iap, iapPlan, readPacket, checkInstallImage as checkInstall, checkRestoreImage as checkRestore, trimTail, erasePacket,
	IapError,
} from "../static/js/flasher/iap.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;
const fx = json("db25d.json");
const radio = json("radios.json").radios.find((r) => r.id === "db25d");
const FS = radio.flash.first_stage;
const checkInstallImage = (image) => checkInstall(image, FS);
const checkRestoreImage = (image) => checkRestore(image, FS);
const iapOn = (stream) => new Iap(stream, { firstStage: FS });

test("the DB25-D's data in the flasher tables", () => {
	const b = C.boards.db25d;
	assert.equal(b.progmem_size, 0x200000);
	assert.equal(b.data_flash, false);
	// the stock-side routes the page takes, from board.toml and board.mk
	assert.equal(radio.flash.stock_console, "usmart");
	assert.equal(radio.flash.update_mode, "iap");
	assert.deepEqual(FS, { size: 0x1000, head: "2c1101c0" });
	assert.equal(radio.flash.read_rate, 46000);
	// every build the console route knows of this board's series is a stock version of it or an older build, the ones the
	// stub goes in through by the board's names; every build known has its version string inside the application, and
	// those whose RAM copy is known have it in the console's RAM
	for (const v of radio.flash.stock_versions) assert.ok(BUILDS.some((b) => versionOf(b) === v), `the console knows ${v}`);
	for (const b of buildsFor(radio.flash.stock_versions).filter((x) => x.stub)) assert.equal(stockName(radio.flash, b.name), `909E.DBB.EARSAB.${b.name}`);
	for (const b of BUILDS) {
		assert.ok(b.at >= 0x0300C000 && b.at < 0x03090000 && b.at % 4 === 0, b.date);
		assert.ok(b.ram === undefined || (b.ram >= 0x18000000 && b.ram < 0x18001000 && b.ram % 4 === 0), b.date);
	}
	assert.equal(new Set(BUILDS.map((b) => b.date)).size, BUILDS.length);
	assert.deepEqual(b.stub_board, { id: 3, latch: 45, key: 44, nand: true });
	assert.deepEqual(b.slot, [0xE8000, 0x100000]);
	const ps = C.profiles.filter((p) => p.board === "db25d");
	assert.equal(ps.length, 5);
	// the 018 builds come as vendor files; 020 has no vendor file, only a program flash read of the radio
	assert.ok(ps.filter((p) => p.tag !== 20).every((p) => p.stock_header === 0 && p.stock_file));
	assert.equal(ps.find((p) => p.tag === 20).stock_file, null);
	assert.equal(C.boards.dm32.data_flash, true);
});

test("codec carved from a plain stock file is byte-identical to hrc7000_codec.py", async () => {
	const base = C.profiles.find((p) => p.board === "db25d" && p.tag === 2501);
	const c = { ...C, profiles: [{ ...base, stock_file: { ...base.stock_file, sha256: fx.sha256 }, code_sha256: null, tables_sha256: null }] };
	const plain = bin("db25d_stock.bin");
	const got = await c7.codecFromFile(plain, c, "db25d");
	assert.equal(got.profile.tag, 2501);
	const image = bin("db25d_image.bin"), merged = bin("db25d_merged.bin");
	const [start, data] = c7.target(c, "db25d", image, c7.slotBytes(c, "db25d", got));
	assert.equal(start, 0xC000);
	assert.ok(equal(data.subarray(0, merged.length), merged), "image, padding and slot match hrc7000_codec.merge()");
	assert.ok(data.subarray(merged.length).every((b) => b === 0xFF));
	await assert.rejects(c7.codecFromFile(plain.slice(1), c, "db25d"), c7.CodecError);
	await assert.rejects(c7.codecFromFile(plain, c, "dm32"), c7.CodecError, "another radio's file");
});

test("a real stock file gives the slot hrc7000_codec.py gives", { skip: !fx.real && "set RFOUNDRY_DB25D_STOCK when making the fixtures" }, async () => {
	const data = new Uint8Array(readFileSync(fx.real.path));
	const got = await c7.codecFromFile(data, C, "db25d");
	assert.ok(equal(c7.slotBytes(C, "db25d", got), bin("db25d_real_slot.bin")));
	assert.equal(got.profile.name, fx.real.name);
});

test("a program flash read of a radio that runs a stock DB25-D firmware is analyzed", async () => {
	const base = C.profiles.find((p) => p.board === "db25d" && p.tag === 2501);
	const plain = bin("db25d_stock.bin");
	const c = { ...C, profiles: [{ ...base, stock_file: { ...base.stock_file, sha256: fx.sha256 } }] };
	// the synthetic file's codec bytes are random, so their hashes are the synthetic ones
	const code = plain.subarray(fx.code - 0x0300C000, fx.code - 0x0300C000 + base.code_len);
	const tables = plain.subarray(fx.tables - 0x0300C000, fx.tables - 0x0300C000 + base.tables_len);
	c.profiles[0].code_sha256 = await sha256(code);
	c.profiles[0].tables_sha256 = await sha256(tables);
	const { crc32 } = await import("../static/js/flasher/bytes.js");
	c.profiles[0].tables_head_crc = crc32(tables.subarray(0, 64));
	const progmem = new Uint8Array(0x200000).fill(0xFF);
	progmem.set(plain, 0xC000);
	const a = await c7.analyze(progmem, c, "db25d");
	assert.equal(a.firmware, "stock");
	assert.equal(a.codec.codeAddr, fx.code - 0x03000000);
	const first1M = await c7.codecFromFile(progmem, c, "db25d");
	assert.equal(first1M.tablesAddr, fx.tables - 0x03000000);
	const none = new Uint8Array(0x200000).fill(0xFF);
	none.set(plain.subarray(0, 0x1000), 0xC000);
	assert.equal((await c7.analyze(none, c, "db25d")).codec, null, "an unsupported stock version has no codec recipe");
});

// The synthetic stock file's profile with the hashes of its (random) codec bytes, so the codec can be found by content
async function syntheticProfile(fileSha = fx.sha256) {
	const { crc32 } = await import("../static/js/flasher/bytes.js");
	const base = C.profiles.find((p) => p.board === "db25d" && p.tag === 2501);
	const plain = bin("db25d_stock.bin");
	const code = plain.subarray(fx.code - 0x0300C000, fx.code - 0x0300C000 + base.code_len);
	const tables = plain.subarray(fx.tables - 0x0300C000, fx.tables - 0x0300C000 + base.tables_len);
	const p = { ...base, stock_file: { ...base.stock_file, sha256: fileSha }, code_sha256: await sha256(code),
		tables_sha256: await sha256(tables), tables_head_crc: crc32(tables.subarray(0, 64)) };
	return { c: { ...C, profiles: [p] }, plain };
}

// A build whose codec is another compile: the signature at its start, the rest of the code different
function otherBuild(plain) {
	const f = plain.slice();
	const at = fx.code - 0x0300C000;
	for (let i = at + 64; i < at + 0x2000; i++) f[i] ^= 0x5A;
	return f;
}

test("a stock file not known by its SHA-256 gives its codec when the codec's own hashes match", async () => {
	const { c, plain } = await syntheticProfile("00".repeat(32));
	const longer = new Uint8Array(plain.length + 300).fill(0x11);
	longer.set(plain, 0);		// a re-release: other bytes after the codec, the codec unchanged
	for (const f of [plain, longer]) {
		const got = await c7.codecFromFile(f, c, "db25d");
		assert.equal(got.profile.tag, 2501);
		assert.equal(got.codeAddr, fx.code - 0x03000000);
		assert.equal(got.tablesAddr, fx.tables - 0x03000000);
	}
	await assert.rejects(c7.codecFromFile(otherBuild(plain), c, "db25d"), /no recipe/, "a codec of another compile is refused");
	const noSig = plain.slice();
	noSig.fill(0, fx.code - 0x0300C000, fx.code - 0x0300C000 + 8);
	await assert.rejects(c7.codecFromFile(noSig, c, "db25d"), /not a stock firmware file the codec can be taken from/);
	await assert.rejects(c7.codecFromFile(new Uint8Array(0x300000), c, "db25d"), c7.CodecError, "larger than the program flash");
});

test("a radio whose stock build carries a codec without a recipe is told apart from one without a codec", async () => {
	const { c, plain } = await syntheticProfile();
	const progmem = (f) => {
		const p = new Uint8Array(0x200000).fill(0xFF);
		p.set(f, 0xC000);
		return p;
	};
	const known = await c7.analyze(progmem(plain), c, "db25d");
	assert.equal(known.otherCodec, false);
	const other = progmem(otherBuild(plain));
	const a = await c7.analyze(other, c, "db25d");
	assert.equal(a.codec, null);
	assert.equal(a.firmware, "unknown");
	assert.equal(a.otherCodec, true);
	await assert.rejects(c7.codecFromFile(other, c, "db25d"), /backup holds a stock codec build RadioFoundry has no recipe for/);
	const none = new Uint8Array(0x200000).fill(0xFF);
	none.set(plain.subarray(0, 0x1000), 0xC000);
	assert.equal((await c7.analyze(none, c, "db25d")).otherCodec, false);
	await assert.rejects(c7.codecFromFile(none, c, "db25d"), /contains no codec recognized/);
	// RadioFoundry's own code may hold the signature words as constants: that is not a stock codec
	const rfoundry = progmem(otherBuild(plain));
	rfoundry.set(new TextEncoder().encode("RadioFoundry"), 0xD000);
	assert.equal((await c7.analyze(rfoundry, c, "db25d")).otherCodec, false);
});

test("the stock version string is read from the program flash wherever the build put it", () => {
	const p = new Uint8Array(0x200000).fill(0xFF);
	assert.equal(stockVersion(p), null);
	p.set(new TextEncoder().encode("909E.D6x.EARSAB.020.Jan 10 2026"), 0x7DCC4);
	assert.equal(stockVersion(p), "909E.D6x.EARSAB.020");
	const q = new Uint8Array(0x200000).fill(0xFF);
	q.set(new TextEncoder().encode("909E.D6x.EARSAB.018"), 0x1000);		// in the bootloader, not the application
	assert.equal(stockVersion(q), null);
});

// ---- USMART

class Pipe {
	constructor(onWrite) {
		this.buf = new Uint8Array(0);
		this.onWrite = onWrite;
	}
	clear() { this.buf = new Uint8Array(0); }
	take(n) { const o = this.buf.slice(0, n); this.buf = this.buf.slice(n); return o; }
	push(b) { this.buf = concat(this.buf, typeof b === "string" ? new TextEncoder().encode(b) : b); }
	async write(b) { this.onWrite(b, this); }
}

const randomFlash = (size) => {
	const f = new Uint8Array(size);
	let x = 12345;
	for (let i = 0; i < size; i++) { x = (x * 1103515245 + 12345) >>> 0; f[i] = x >>> 24; }
	return f;
};

// A DB25-D on its original firmware, build 018, with the stock console (fakec7000.js), fast
async function stockDb25d(t, opts = {}) {
	const { FakeC7000, c7000Simulation } = await import("../static/js/flasher/fakec7000.js");
	const sim = await c7000Simulation(C, "db25d", 7000, radio);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 50, bootTime: 50, board: "DB25-D",
		usmart: "018", ...opts });
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 300));
	return { r, sim };
}

test("USMART: every command is one framed write; nothing unframed and no write outside the stub window is sent", async () => {
	const sent = [];
	const pipe = new Pipe((b, p) => {
		sent.push(new TextDecoder().decode(b));
		p.push("\r\nread_addr(0X300C000)=0XC001112C;\r\nclose slot.\r\n");
	});
	const u = new Usmart(pipe);
	assert.equal(await u.probe(), true);
	assert.deepEqual(sent, ["@AT read_addr(0x0300c000)\r\n"]);
	for (const bad of ["write_addr(0x0300c000,0x00000000)", "write_addr(0x00053ffc,0x00000000)", "write_addr(0x00058000,0x00000000)",
		"write_addr(0x18000844,0x00054000)", "DMRCALL", "01", "TC58_INTF_BlocksErase(0)", "read_addr(0x0300c000);printk"])
		await assert.rejects(u.send(bad), UsmartError, bad);
	await u.send("write_addr(0x00057ffc,0x00000001)");
	await u.send(`write_addr(0x${HIJACK.toString(16)},0x00054000)`);
	assert.equal(await new Usmart(new Pipe(() => {})).probe(200), false, "nothing answers");
});

test("USMART: a write counts only with its echo, and the echo must carry the value", async () => {
	let mode = "ok";
	const pipe = new Pipe((b, p) => {
		const m = /write_addr\(0x([0-9a-f]+),0x([0-9a-f]+)\)/.exec(new TextDecoder().decode(b));
		const v = mode === "wrong" ? 1 : parseInt(m[2], 16);
		if (mode !== "silent") p.push(`\r\nwrite_addr(0X${parseInt(m[1], 16).toString(16).toUpperCase()},0X${v.toString(16).toUpperCase()});\r\nclose slot.\r\n`);
	});
	const u = new Usmart(pipe, { quiet: 5 });
	assert.equal(await u.writeWord(0x54000, 0x7020C080), true);
	mode = "wrong";
	assert.equal(await u.writeWord(0x54000, 0x7020C080), false);
	mode = "silent";
	assert.equal(await u.writeWord(0x54000, 0x7020C080, 100), false);
	await assert.rejects(u.writeWordRetry(0x54000, 5, 2), /did not answer/);
});

test("USMART: the build comes from the version string; an unknown build is reported as null", async (t) => {
	const { r } = await stockDb25d(t);
	const b = await new Usmart(r.stream).build();
	assert.deepEqual([b.name, b.date, b.stub], ["018", "2025-01-21", true]);
	r.progmem.fill(0x20, 0x79F30, 0x79F50);
	assert.equal(await new Usmart(r.stream).build(), null);
});

test("GD-88: its main SoC's console build 021 is found by its own version string, and takes the stub as the DB25-D's", { timeout: 120000 }, async (t) => {
	const { FakeC7000, c7000Simulation } = await import("../static/js/flasher/fakec7000.js");
	const { Stub, patchUniv, boardLed } = await import("../static/js/flasher/stub.js");
	const gd88 = json("radios.json").radios.find((x) => x.id === "gd88");
	const sim = await c7000Simulation(C, "gd88", 7000, gd88);
	assert.equal(stockVersion(sim.progmem, gd88.flash.stock_versions), "B49E.D64.EOPSAB.021");
	assert.equal(stockVersion(sim.progmem, radio.flash.stock_versions), null, "not a DB25-D version");
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 50, bootTime: 50, board: "GD-88",
		usmart: "021" });
	t.after(() => r.stop());
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 300));
	assert.equal((await new Usmart(r.stream, { versions: gd88.flash.stock_versions }).build())?.name, "021");
	assert.equal(await new Usmart(r.stream, { versions: radio.flash.stock_versions }).build(), null,
		"the DB25-D's builds are not looked for on a GD-88");
	const image = patchUniv(stubImages(json("c7000-stub.json")).image, C.boards.gd88.stub_board);
	await new Usmart(r.stream, { quiet: 2 }).loadStub(image);
	assert.equal(r.state, "stub");
	assert.ok(equal(r.loaded.subarray(0, image.length), image), "the patched image");
	const s = new Stub(r.stream, { log: () => {} });
	await s.helloWithin(3000, "no stub");
	await s.configure(C.boards.gd88.stub_board, boardLed(C, "gd88"));
	assert.equal(r.stubCfg.board, 4);
});

test("USMART: the version comes from its RAM copy, which carries the variant the flash copy leaves out", async (t) => {
	const { r } = await stockDb25d(t, { variant: "D6T" });
	const u = new Usmart(r.stream);
	const b = await u.build();
	assert.equal(stockVersion(r.progmem), "909E.D6x.EARSAB.018", "the program flash holds the placeholder");
	assert.equal(await u.version(b), "909E.D6T.EARSAB.018");
	assert.ok(r.frames.every((f) => f.startsWith("read_addr(")), "reads only");
});

test("identify: the stock application's answer to the read-only probe is its version, never the loader's", async (t) => {
	const { r } = await stockDb25d(t, { variant: "D64", faults: { consoleSilent: true } });
	const um = await iapOn(r.stream).probe(1500);
	assert.equal(um.version, null);
	assert.match(um.stock, /909E\.D64\.EARSAB\.018/);
	const { versionText, variantCode } = await import("../static/js/flasher/variant.js");
	assert.equal(versionText(radio, um.stock), "909E.D64.EARSAB.018");
	assert.equal(variantCode(radio, um.stock), "D64");
	assert.ok(r.events.includes("identify"));
	assert.equal(r.progmem.length, 0x200000);
	// after an identify the console stays quiet until the next power-on, as on the radio
	r.faults.consoleSilent = false;
	assert.equal(await new Usmart(r.stream).readWord(0x0300C000, 200), null);
});

test("USMART: the stub goes into 0x54000, is read back, and the hijack word starts it; then it reads and writes the flash", { timeout: 120000 }, async (t) => {
	const { r, sim } = await stockDb25d(t);
	const { Stub } = await import("../static/js/flasher/stub.js");
	const stubs = stubImages(json("c7000-stub.json"));
	const u = new Usmart(r.stream, { quiet: 2 });
	const phases = new Set();
	await u.loadStub(stubs.image, { onProgress: (p) => phases.add(p) });
	assert.deepEqual([...phases], ["write", "check", "start"]);
	// the frames: the entry check, every word written, every word read back, the hijack word, one more frame
	const n = Math.ceil(stubs.image.length / 4);
	assert.equal(r.frames.length, 1 + n + n + 1 + 1);
	assert.ok(r.frames.every((f) => /^(read_addr\(0x[0-9a-f]{8}\)|write_addr\(0x[0-9a-f]{8},0x[0-9a-f]{8}\))$/.test(f)));
	assert.equal(r.frames.at(-2), "write_addr(0x18000848,0x00054000)");
	assert.equal(r.state, "stub");
	assert.ok(equal(r.loaded.subarray(0, stubs.image.length), stubs.image), "the RAM holds the image exactly");
	const s = new Stub(r.stream, { log: () => {} });
	await s.helloWithin(3000, "no stub");
	await s.configure(C.boards.db25d.stub_board, C.led);
	s.size.progmem = 0x200000;
	assert.ok(equal(await s.dump("progmem", 0, 0x200000), sim.progmem), "the whole 2 MB, stock firmware included");
	const image = randomFlash(0x3000);
	image.set([0x2C, 0x11, 0x01, 0xC0], 0);
	const res = await s.writeProgram(image, 0xC000, { appStart: C.app_start, appEnd: C.app_end });
	assert.equal(res.written, 3);
	assert.ok(equal(r.progmem.subarray(0xC000, 0xF000), image));
});

test("USMART: a word that reads back wrong stops the load before the hijack word is written", async (t) => {
	const { r } = await stockDb25d(t);
	const stubs = stubImages(json("c7000-stub.json"));
	const set = r.ram.set.bind(r.ram);
	r.ram.set = (a, v) => set(a, a === 0x54100 ? v ^ 0x100 : v);
	await assert.rejects(new Usmart(r.stream, { quiet: 2 }).loadStub(stubs.image), /0x00054100/);
	assert.equal(r.ram.get(HIJACK), undefined);
	assert.equal(r.state, "firmware", "the original firmware still runs");
});

test("USMART: a radio whose application does not start with the stock entry is refused before any write", async (t) => {
	const { r } = await stockDb25d(t);
	r.progmem[0xC000] = 0;
	await assert.rejects(new Usmart(r.stream).loadStub(stubImages(json("c7000-stub.json")).image), /original firmware/);
	assert.ok(r.frames.every((f) => f.startsWith("read_addr")));
});

// ---- IAP

function iapRadio({ corrupt = false } = {}) {
	const got = { erase: null, pages: [] };
	let phase = "banner", want = 0, sum = 0;
	const pipe = new Pipe((b, p) => {
		// the read-only request: the version string, one page and its checksum (hardware-db25d.md 7); phase is unchanged
		if (b.length === 19 && new TextDecoder().decode(b.subarray(0, 11)) === "Flash Read ") {
			got.reads = (got.reads || 0) + 1;
			p.push(concat(new TextEncoder().encode("Read_2M_V1.08BZSRJun 30 2023"), new Uint8Array(2048).fill(0xA5),
				new TextEncoder().encode("ChecksumR"), be32(2048 * 0xA5), new TextEncoder().encode("IAPReady")));
			return;
		}
		if (phase === "banner") {
			got.erase = b.slice();
			const n = (b[15] << 24 | b[16] << 16 | b[17] << 8 | b[18]) >>> 0;
			want = n + 1;
			p.push("  IAPV1.08BZSRJun 30 2023");
			setTimeout(() => p.push("Erase ok\r\n"), 30);
			phase = "pages";
			return;
		}
		got.pages.push(b.slice());
		for (const x of b) sum = (sum + x) >>> 0;
		if (got.pages.length < want) p.push("kyd");
		else {
			setTimeout(() => p.push(concat(new TextEncoder().encode("ChecksumI"), be32(corrupt ? sum ^ 1 : sum))), 20);
			phase = "done";
		}
	});
	return { pipe, got };
}

function installImage(len = 4096 + 20000) {
	const img = randomFlash(len);
	img.set([0x2C, 0x11, 0x01, 0xC0], 0);
	img.set([0x80, 0xC0, 0x20, 0x70], 0x1000);
	return img;
}

test("IAP: the Erase packet and the pages as the vendor tool makes them", () => {
	assert.equal(Buffer.from(erasePacket(413)).toString("hex"), "457261736520202020202000000000" + "0000019d");
	const img = installImage();
	const p = iapPlan(img);
	assert.equal(p.n, Math.floor(img.length / 2048));
	assert.equal(p.pages.length, p.n + 1);
	assert.ok(p.pages.at(-1).subarray(img.length - p.n * 2048).every((b) => b === 0xFF));
	let sum = 0;
	for (const pg of p.pages) for (const b of pg) sum += b;
	assert.equal(p.sum, sum >>> 0);
	assert.throws(() => checkInstallImage(img.subarray(0, 4096)), IapError);
	assert.throws(() => checkInstallImage(Uint8Array.of(...img.subarray(0, 4096), 0, 0, 0, 0, 1)), IapError);
	const bad = img.slice(); bad[0] = 0;
	assert.throws(() => checkInstallImage(bad), IapError);
	assert.equal(trimTail(concat(img, new Uint8Array(5000).fill(0xFF))).length, (img.length + 3) & ~3);
});

test("IAP: banner, erase, every page acknowledged, checksum compared", async () => {
	const img = installImage();
	const { pipe, got } = iapRadio();
	const iap = iapOn(pipe);
	setTimeout(() => pipe.push("FLASH_TC58 ok\r\nIAPReady"), 30);
	await iap.waitBanner(2000);
	const seen = [];
	await iap.write(img, { onPage: (i, n) => seen.push(i / n) });
	assert.equal(got.pages.length, Math.floor(img.length / 2048) + 1);
	assert.equal(seen.at(-1), 1);
	assert.ok(equal(Uint8Array.from(got.pages.flatMap((p) => [...p]).slice(0, img.length)), img));
	await assert.rejects(iapOn(new Pipe(() => {})).waitBanner(150), IapError);
});

test("IAP: a radio stuck in update mode at power-on answers the read-only probe, and the install then needs no banner", async () => {
	const { pipe, got } = iapRadio();
	const iap = iapOn(pipe);
	assert.equal(Buffer.from(readPacket(0, 1)).toString("hex"), Buffer.from("Flash Read ").toString("hex") + "00000000" + "00000001");
	// the banner went by before the page asked: the radio only answers the request
	const um = await iap.probe(1500);
	assert.equal(um.version, "Read_2M_V1.08B");
	assert.equal(got.reads, 1);
	assert.equal(got.erase, null, "the probe never sends Erase");
	assert.equal(got.pages.length, 0, "and writes no page");
	// the install that follows starts at once (no waitBanner), as the page does after a probe
	const img = installImage();
	await iap.write(img);
	assert.equal(got.pages.length, Math.floor(img.length / 2048) + 1);
	// a radio that is off, or runs something else, does not answer
	assert.equal(await iapOn(new Pipe(() => {})).probe(200), null);
});

test("IAP: a checksum that differs is reported, and silence after the erase stops before any data", async () => {
	const img = installImage();
	const { pipe } = iapRadio({ corrupt: true });
	await assert.rejects(iapOn(pipe).write(img), /checksum/);
	const quiet = new Pipe(() => {});
	await assert.rejects(iapOn(quiet).write(img, { eraseTimeout: 200 }), /erase/);
});

test("IAP: a backup's application area goes back through update mode; one without an application entry is refused", async () => {
	const pm = randomFlash(0x200000);
	pm.set([0x2C, 0x11, 0x01, 0xC0], 0xC000);
	const [, area] = c7.restoreTarget(C, pm);
	assert.throws(() => checkInstallImage(area), IapError, "a stock application is not an install image");
	checkRestoreImage(area);
	const head = area.subarray(0, 0x8000);	// the whole area is 500 pages: its head shows the same
	const { pipe, got } = iapRadio();
	await iapOn(pipe).write(head, { restore: true });
	assert.ok(equal(Uint8Array.from(got.pages.flatMap((p) => [...p]).slice(0, head.length)), head));
	const bad = area.slice();
	bad[0] = 0xFF;
	assert.throws(() => checkRestoreImage(bad), IapError);
	await assert.rejects(iapOn(iapRadio().pipe).write(bad.subarray(0, 0x8000), { restore: true }), IapError);
});

// ---- one stub for every RAM span

test("the universal stub loads into every span a running DB25-D offers and runs from 0x30000", async (t) => {
	const stubs = stubImages(json("c7000-stub.json"));
	univFits(stubs.image, 0x30000, [0x30000, 0x20000]);
	univFits(stubs.image, 0x1D000, [0x1D000, 0x12000]);
	const { FakeC7000 } = await import("../static/js/flasher/fakec7000.js");
	const { Stub } = await import("../static/js/flasher/stub.js");
	const r = new FakeC7000({ progmem: randomFlash(0x200000), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200,
		board: "DB25-D", stubSpan: [0x1D000, 0x12000] });
	t.after(() => r.stop());
	r.progmem.set(new TextEncoder().encode("RadioFoundry"), 0xD000);
	r.stubEntry = true;
	r.powerOn();
	await new Promise((ok) => setTimeout(ok, 800));
	const s = new Stub(r.stream, { log: () => {} });
	await s.enterFromRFoundry(stubs.image);
	assert.equal(r.state, "stub");
	assert.ok(s.v6 && s.v6.run === 0x30000, "HELLO v6 reports the run address");
	await s.configure(C.boards.db25d.stub_board, C.led);
	assert.deepEqual(r.stubCfg, { board: 3, flags: 1 });
	assert.deepEqual([...r.configs[0].subarray(8, 10)], [45, 44], "power latch and key pins sent");
});

// ---- the stub on a 2 MB program flash

test("the stub reads the DB25-D's whole 2 MB program flash only once told its size", async (t) => {
	const { FakeC7000 } = await import("../static/js/flasher/fakec7000.js");
	const { Stub } = await import("../static/js/flasher/stub.js");
	const stubs = stubImages(json("c7000-stub.json"));
	const progmem = randomFlash(0x200000);
	const r = new FakeC7000({ progmem: progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 400, bootTime: 200 });
	t.after(() => r.stop());
	const s = new Stub(r.stream, { log: () => {} });
	setTimeout(() => r.powerOn(), 50);
	await s.waitForRom(5000);
	await s.loadThroughRom(stubs.image, stubs.load);
	await assert.rejects(s.dump("progmem", 0x1F0000, 0x1F2000), StubError);
	s.size.progmem = 0x200000;
	assert.ok(equal(await s.dump("progmem", 0x1F0000, 0x1F2000), progmem.subarray(0x1F0000, 0x1F2000)));
});

test("IAP: a loader without the board's first stage writes nothing", async () => {
	let sent = 0;
	await assert.rejects(new Iap(new Pipe(() => { sent++; })).write(new Uint8Array(0x2000)), /no first stage/);
	assert.equal(sent, 0);
});

test("IAP: an erase that reaches a protected region writes nothing", async () => {
	let sent = 0;
	const img = installImage();
	const cal = [{ mem: "progmem", start: 0xD000, end: 0xE000, what: "test (x.md 1)" }];
	await assert.rejects(new Iap(new Pipe(() => { sent++; }), { firstStage: FS, protect: cal }).write(img), /protected region/);
	assert.equal(sent, 0);
});

test("USMART: a build's own stub window and hijack word are used, and checked against what the stub may use", { timeout: 60000 }, async (t) => {
	assert.deepEqual(stubTarget(null), { window: STUB_WINDOW, hijack: HIJACK });
	for (const b of [...BUILDS, UNKNOWN_BUILD]) assert.ok(stubTarget(b), b.date);
	// the stub image fits every build's window, and an unproven build's window is never the default one, which is
	// inside their heap (hardware-db25d.md 7.1)
	const stubLen = stubImages(json("c7000-stub.json")).image.length;
	for (const b of [...BUILDS, UNKNOWN_BUILD]) assert.ok(stubLen <= stubTarget(b).window[1], b.date);
	for (const b of BUILDS.filter((x) => !x.stub)) assert.ok(b.window && b.window[0] > STUB_WINDOW[0], b.date);
	assert.throws(() => stubTarget({ window: [0x32000, 0x4000] }), /outside the RAM/, "over where the stub runs");
	assert.throws(() => stubTarget({ window: [0x56000, 0x4000] }), /outside the RAM/, "past the end of SRAM");
	assert.throws(() => stubTarget({ hijack: 0x18000900 }), /command table/);
	const { r } = await stockDb25d(t);
	const build = { ...BUILDS[0], window: [0x48000, 0x4000], hijack: 0x18000844 };
	r.build = build;
	const u = new Usmart(r.stream, { quiet: 2 });
	await u.loadStub(stubImages(json("c7000-stub.json")).image, { build, untried: true });
	await new Promise((ok) => setTimeout(ok, 100));
	assert.equal(r.state, "stub");
	const writes = r.frames.filter((f) => f.startsWith("write_addr("));
	assert.ok(writes.every((f) => /^write_addr\(0x(0004[89ab][0-9a-f]{3}|18000844),/.test(f)), "only the build's window and hijack word");
	assert.equal(r.ram.get(HIJACK), undefined, "the default hijack word untouched");
	// a write outside the target is refused before the port
	await assert.rejects(u.send("write_addr(0x00054000,0x00000001)"), /refused/);
});
