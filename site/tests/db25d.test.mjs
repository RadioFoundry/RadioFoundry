// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DB25-D pieces of the web flasher: the codec carved from a plain stock file against dm32_codec.py (fixtures from
// site/tests/fixtures.py), the USMART read and the IAP write against simulated radios, and the stub choice by RAM span.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

import { equal, sha256, be32, concat } from "../static/js/flasher/bytes.js";
import { stubImages, stubFits, StubError } from "../static/js/flasher/stub.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { Usmart, usmartDump, usmartOrder, Iap, iapPlan, checkInstallImage, trimTail, erasePacket, DB25DError } from "../static/js/flasher/db25d.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;
const fx = json("db25d.json");

test("the DB25-D's data in the flasher tables", () => {
	const b = C.boards.db25d;
	assert.equal(b.progmem_size, 0x200000);
	assert.equal(b.data_flash, false);
	assert.equal(b.power_on_stub, 0x30000);
	assert.deepEqual(b.slot, [0xE8000, 0x100000]);
	const ps = C.profiles.filter((p) => p.board === "db25d");
	assert.equal(ps.length, 3);
	assert.ok(ps.every((p) => p.stock_header === 0 && p.stock_file));
	assert.equal(C.boards.dm32.data_flash, true);
});

test("codec carved from a plain stock file is byte-identical to dm32_codec.py", async () => {
	const base = C.profiles.find((p) => p.board === "db25d" && p.tag === 2501);
	const c = { ...C, profiles: [{ ...base, stock_file: { ...base.stock_file, sha256: fx.sha256 }, code_sha256: null, tables_sha256: null }] };
	const plain = bin("db25d_stock.bin");
	const got = await c7.codecFromFile(plain, c, "db25d");
	assert.equal(got.profile.tag, 2501);
	const image = bin("db25d_image.bin"), merged = bin("db25d_merged.bin");
	const [start, data] = c7.target(c, "db25d", image, c7.slotBytes(c, "db25d", got));
	assert.equal(start, 0xC000);
	assert.ok(equal(data.subarray(0, merged.length), merged), "image, padding and slot match dm32_codec.merge()");
	assert.ok(data.subarray(merged.length).every((b) => b === 0xFF));
	await assert.rejects(c7.codecFromFile(plain.slice(1), c, "db25d"), c7.CodecError);
	await assert.rejects(c7.codecFromFile(plain, c, "dm32"), c7.CodecError, "another radio's file");
});

test("a real stock file gives the slot dm32_codec.py gives", { skip: !fx.real && "set ODR_DB25D_STOCK when making the fixtures" }, async () => {
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

function consoleRadio(flash, { drop = 0, flaky = 0 } = {}) {
	let dropped = 0, n = 0;
	const sent = [];
	const pipe = new Pipe((b, p) => {
		const text = new TextDecoder().decode(b);
		sent.push(text);
		const m = /^@read_addr\(0x([0-9a-f]{8})\)\r\n$/.exec(text);
		if (!m) return;
		if (dropped < drop) { dropped++; return; }
		const addr = parseInt(m[1], 16);
		let v = new DataView(flash.buffer).getUint32(addr - 0x03000000, true);
		if (flaky && addr >= 0x0300C000 && addr < 0x0300D000 && ++n % flaky === 0) v = (v ^ 1) >>> 0;	// the first 4 KB are read twice
		p.push(`read_addr(0x${m[1]})=0x${v.toString(16).toUpperCase()};\r\n`);
	});
	return { pipe, sent };
}

const randomFlash = (size) => {
	const f = new Uint8Array(size);
	let x = 12345;
	for (let i = 0; i < size; i++) { x = (x * 1103515245 + 12345) >>> 0; f[i] = x >>> 24; }
	return f;
};

test("USMART: probe, and only '@' with read_addr lines is ever sent", async () => {
	const flash = randomFlash(0x200000);
	const { pipe, sent } = consoleRadio(flash);
	const u = new Usmart(pipe);
	assert.equal(await u.probe(), true);
	assert.deepEqual(sent, ["@read_addr(0x0300c000)\r\n"]);
	await assert.rejects(u.send("write_addr(0x03000000,0)"), DB25DError);
	assert.equal(await new Usmart(new Pipe(() => {})).probe(200), false, "nothing answers");
});

test("USMART: the read order starts with the application's first 4 KB and covers every word once", () => {
	const o = usmartOrder(0x20000);
	assert.equal(o[0], 0x0300C000);
	assert.equal(o[1023], 0x0300CFFC);
	assert.equal(o.length, 0x20000 / 4);
	assert.equal(new Set(o).size, o.length);
});

test("USMART: a read with a lost reply and flaky head words is exact, and a stopped read resumes", { timeout: 60000 }, async () => {
	const flash = randomFlash(0x200000);
	const limit = 0x10000;
	const { pipe } = consoleRadio(flash, { drop: 1, flaky: 7 });
	const u = new Usmart(pipe);
	const ac = new AbortController();
	let state = null;
	await assert.rejects(usmartDump(u, { limit, signal: ac.signal, onProgress: (d) => { if (d >= 3000) ac.abort(); },
		onCheckpoint: (done, image) => { state = { done, image: image.slice() }; } }), DB25DError);
	assert.ok(state.done >= 3000 && state.done < limit / 4);
	const image = await usmartDump(u, { limit, state });
	assert.ok(equal(image, flash.subarray(0, limit)));
});

// ---- IAP

function iapRadio({ corrupt = false } = {}) {
	const got = { erase: null, pages: [] };
	let phase = "banner", want = 0, sum = 0;
	const pipe = new Pipe((b, p) => {
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
	assert.throws(() => checkInstallImage(img.subarray(0, 4096)), DB25DError);
	assert.throws(() => checkInstallImage(Uint8Array.of(...img.subarray(0, 4096), 0, 0, 0, 0, 1)), DB25DError);
	const bad = img.slice(); bad[0] = 0;
	assert.throws(() => checkInstallImage(bad), DB25DError);
	assert.equal(trimTail(concat(img, new Uint8Array(5000).fill(0xFF))).length, (img.length + 3) & ~3);
});

test("IAP: banner, erase, every page acknowledged, checksum compared", async () => {
	const img = installImage();
	const { pipe, got } = iapRadio();
	const iap = new Iap(pipe);
	setTimeout(() => pipe.push("FLASH_TC58 ok\r\nIAPReady"), 30);
	await iap.waitBanner(2000);
	const seen = [];
	await iap.write(img, { onPage: (i, n) => seen.push(i / n) });
	assert.equal(got.pages.length, Math.floor(img.length / 2048) + 1);
	assert.equal(seen.at(-1), 1);
	assert.ok(equal(Uint8Array.from(got.pages.flatMap((p) => [...p]).slice(0, img.length)), img));
	await assert.rejects(new Iap(new Pipe(() => {})).waitBanner(150), DB25DError);
});

test("IAP: a checksum that differs is reported, and silence after the erase stops before any data", async () => {
	const img = installImage();
	const { pipe } = iapRadio({ corrupt: true });
	await assert.rejects(new Iap(pipe).write(img), /checksum/);
	const quiet = new Pipe(() => {});
	await assert.rejects(new Iap(quiet).write(img, { eraseTimeout: 200 }), /erase/);
});

// ---- stubs by RAM span

test("the stub for a running DB25-D is chosen by the base ODR reports and refused for another span", async () => {
	const stubs = stubImages(json("c7000-stub.json"));
	const a = stubs.forBase(0x30000), b = stubs.forBase(0x1D000), d = stubs.forBase(0x17000);
	assert.ok(a && b && d);
	assert.equal(stubs.forBase(0x20000), null);
	assert.equal(await sha256(a.image), stubs.byBase[0x30000].sha);
	stubFits(a.image, 0x30000, 0x20000);
	stubFits(b.image, 0x1D000, 0x12000);
	stubFits(d.image, 0x17000, 0x18000);
	assert.throws(() => stubFits(a.image, 0x1D000, 0x12000), StubError);
	assert.throws(() => stubFits(b.image, 0x30000, 0x20000), StubError);
	assert.throws(() => stubFits(new Uint8Array(100), 0x30000, 0x20000), StubError);
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
	await s.loadThroughRom(stubs.rom, stubs.romLoad);
	await assert.rejects(s.dump("progmem", 0x1F0000, 0x1F2000), StubError);
	s.size.progmem = 0x200000;
	assert.ok(equal(await s.dump("progmem", 0x1F0000, 0x1F2000), progmem.subarray(0x1F0000, 0x1F2000)));
});
