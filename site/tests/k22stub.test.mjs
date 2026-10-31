// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The K22F cable route (k22stub.js on f4stub.js's client) against stub/k22stub.py (fixtures from site/tests/fixtures.py)
// and the simulated radio (fakehid.js FakeHidRFoundry): the board record and HELLO byte for byte, CONFIG accepted and
// refused, no write before CONFIG, the hand-over from RadioFoundry, installs that erase what k22stub.py erases and leave the
// flash it leaves, the sector shared with the codec, changed sectors only, a failed verify, a pulled cable, the restart
// and the SPI flash floor. Nothing here has run on a radio. Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { createHash } from "node:crypto";

import { fromHex, toHex } from "../static/js/flasher/bytes.js";
import { f4Image, F4Stub, MEM_SPI } from "../static/js/flasher/f4stub.js";
import { k22Chip, k22Board, k22Stub, k22Segments, k22Install, ownedFor } from "../static/js/flasher/k22stub.js";
import { FakeHidRFoundry } from "../static/js/flasher/fakehid.js";
import { Link, probe } from "../static/js/flasher/link.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const sha = (b) => createHash("sha256").update(b).digest("hex");
const ST = json("flash-boards.json").hid.stub;
const stubJson = json("k22-stub.json");
const image = f4Image(stubJson);
const fx = json("k22.json");
const quiet = () => {};
const BOARD = fx.board;			// the board the fixtures install on (the stub's first, as FakeHidRFoundry's default)
const B = ST.boards[BOARD];
const sector = (radio, i) => radio.flash.subarray(i * ST.sector, (i + 1) * ST.sector);

async function enter(radio, opts = {}) {
	const s = k22Stub(radio.serialStream(), ST, BOARD, { log: quiet, reopen: async () => radio.serialStream(), ...opts });
	await s.enterFromRFoundry(image, () => {}, { entry: stubJson.run });
	return s;
}

// The fake's stub answers a raw request with these bytes (status, body hex)
function ask(radio, op, payload = new Uint8Array(0)) {
	let out = new Uint8Array(0);
	radio.stubFeed(new Uint8Array([0x02, op, payload.length >> 8, payload.length & 0xFF, payload.reduce((a, x) => (a + x) & 0xFF, 0), 0x03, ...payload]),
		(b) => { out = new Uint8Array([...out, ...b]); });
	return { status: out[1], body: toHex(out.subarray(6)) };
}

function app(seed, n) {
	const d = new Uint8Array(n);
	for (let i = 0; i < n; i++) d[i] = (i * seed + (i >> 9)) & 0xFF;
	d.set([0xF0, 0xFE, 0x00, 0x20], 0);	// 0x2000FEF0
	return d;
}

test("the K22F data in the flasher tables is k22stub.py's", () => {
	assert.equal(B.record, fx.record);
	assert.equal(B.record.length, ST.record_len * 2);
	assert.equal(stubJson.run, ST.run);
	assert.equal(stubJson.protocol, ST.protocol);
	assert.equal(sha(image), stubJson.sha256);
	assert.ok(image.length <= 8192, "fits RadioFoundry's staging buffer");
	assert.ok(new TextDecoder("latin1").decode(image).includes(ST.name), "the stub's HELLO name is in the image");
	const chip = k22Chip(ST);
	for (const [a, ok] of Object.entries(fx.sp)) assert.equal(chip.spOk(Number(a)), ok, a);
	assert.equal(chip.sectors(ST.flash_kb).length, 256);
	assert.equal(k22Board(ST, B.debuglink_names[0]), BOARD);
	assert.equal(k22Board(ST, "XR-100"), null);
});

test("the simulated stub answers HELLO and CONFIG byte for byte as k22stub.py's fake", async () => {
	const radio = new FakeHidRFoundry(ST);
	radio.mode = "stub";
	const h = fx.hello;
	assert.deepEqual(ask(radio, 0x01), h.before);
	const bad = fromHex(B.record);
	bad.set([0, 0, 0x20, 0], 4);
	assert.deepEqual(ask(radio, 0x14, bad), h.refused);
	assert.deepEqual(ask(radio, 0x10, new Uint8Array([9, ...fromHex(ST.magic.toString(16))])), h.erase_before);
	assert.deepEqual(ask(radio, 0x14, fromHex(B.record)), h.config);
	assert.deepEqual(ask(radio, 0x01), h.after);
});

test("hand-over: the stub adopts RadioFoundry's port or comes back on a new one; unconfigured until CONFIG", async () => {
	for (const adopt of [true, false]) {
		const radio = new FakeHidRFoundry(ST, { adopt });
		const s = await enter(radio);
		assert.equal(s.info.flashKb, 512);
		assert.equal(s.info.version, 4);
		assert.equal(s.info.adopted, adopt);
		assert.equal(s.configured, false);
		assert.equal(s.info.spiSize, 0, "no SPI flash before a record");
		assert.deepEqual(radio.events, ["stub (from RadioFoundry)"]);
	}
	const radio = new FakeHidRFoundry(ST, { stubEntry: false });
	await assert.rejects(enter(radio), /CONFIG_STUB_ENTRY/);
	assert.equal(radio.mode, "rfoundry");
	// the STM32F4 client refuses the K22F stub's HELLO
	const r2 = new FakeHidRFoundry(ST);
	const f4 = new F4Stub(r2.serialStream(), { log: quiet });
	await assert.rejects(f4.enterFromRFoundry(image, () => {}, { entry: stubJson.run }), /does not answer/);
});

test("CONFIG: accepted with its echo checked, refused when the stub won't take it, and no write before it", async () => {
	const radio = new FakeHidRFoundry(ST);
	const s = await enter(radio);
	await assert.rejects(s.erase(9), /not configured/, "the page refuses before sending");
	await assert.rejects(s.writeImage(B.owned, [[ST.app_base, app(3, 5000)]]), /not configured/);
	assert.ok(!radio.events.some((e) => /^(erase|program)/.test(e)));
	s.configured = true;	// past the page's own check: the stub refuses with status 0x11
	await assert.rejects(s.erase(9), (e) => e.status === 0x11 && /no board configuration/.test(e.message));
	assert.equal(s.configured, false);
	await s.configure();
	assert.ok(s.configured);
	assert.equal(toHex(s.info.record), B.record);
	assert.equal(s.info.spiSize, 0x200000);
	assert.deepEqual(radio.events.slice(-1), ["config"]);
	await s.configure();
	assert.equal(radio.events.filter((e) => e === "config").length, 1, "an echoed record is not sent again");
	// a record the stub refuses (a different application base): nothing written
	const r2 = new FakeHidRFoundry(ST);
	const bad = { ...B, record: B.record.slice(0, 8) + "00002000" + B.record.slice(16) };
	const s2 = await enter(r2, { board: bad });
	s2.board = bad;
	await assert.rejects(s2.configure(), /refused the .+'s board record/);
	assert.equal(s2.configured, false);
	// a stub that takes a record but echoes another one
	const r3 = new FakeHidRFoundry(ST);
	const s3 = await enter(r3);
	r3.hello = () => new Uint8Array([...FakeHidRFoundry.prototype.hello.call(r3).slice(0, -1), 0x01]);
	await assert.rejects(s3.configure(), /did not take/);
});

test("installs erase what k22stub.py erases and leave the flash it leaves", async () => {
	const radio = new FakeHidRFoundry(ST);
	radio.flash.set(bin("k22_flash.bin"));
	const s = await enter(radio);
	for (const [i, want] of fx.installs.entries()) {
		const data = bin(want.image);
		const segs = k22Segments(B, ST.app_base, data);
		assert.deepEqual(segs.map(([a, d]) => [a, d.length, sha(d)]), want.segments, want.image);
		const before = radio.events.length;
		const kept = [];
		const r = await k22Install(s, data, { onKept: async (sec) => kept.push(sec) });
		const erased = radio.events.slice(before).filter((e) => e.startsWith("erase")).map((e) => Number(e.slice(6)));
		assert.deepEqual(erased, want.erased, want.image);
		assert.deepEqual([r.written, r.same], [want.written, want.same], want.image);
		assert.equal(sha(radio.flash), want.flash_sha256, want.image);
		if (i === 0) {
			// the application alone: sector 171 holds its end and the codec's start, read back and kept
			assert.deepEqual(kept.map((k) => k.index), [171]);
			assert.equal(sha(kept[0].kept), sha(bin("k22_flash.bin").subarray(171 * ST.sector, 172 * ST.sector)));
		}
	}
});

test("the application alone keeps the codec; the same image again writes nothing; a change writes only its sectors", async () => {
	const radio = new FakeHidRFoundry(ST);
	const codec = radio.flash.slice(B.codec[0], B.app_end);
	const loader = radio.flash.slice(0, ST.app_base), last = sector(radio, 255).slice();
	const s = await enter(radio);
	const img = app(3, 300000);
	await k22Install(s, img);
	assert.deepEqual(radio.flash.subarray(ST.app_base, ST.app_base + img.length), img);
	assert.deepEqual(radio.flash.slice(B.codec[0], B.app_end), codec, "the codec area untouched");
	assert.deepEqual(radio.flash.slice(0, ST.app_base), loader, "the loader untouched");
	assert.deepEqual(sector(radio, 255), last, "the last sector untouched");
	assert.ok(radio.flash.subarray(ST.app_base + img.length, B.codec[0]).every((x) => x === 0xFF), "the old tail erased");
	const n = radio.events.length;
	assert.deepEqual(await k22Install(s, img), { written: 0, same: 164 });
	assert.equal(radio.events.length, n, "nothing erased or programmed");
	const img2 = img.slice();
	img2.fill(0x5A, 100000, 101000);
	const r = await k22Install(s, img2);
	assert.deepEqual(radio.events.slice(n).filter((e) => e.startsWith("erase")), ["erase 8", "erase 56", "erase 57"]);
	assert.equal(r.written, 3);
	assert.deepEqual(radio.flash.subarray(ST.app_base, ST.app_base + img2.length), img2);
	// an application that reaches into the codec area, one without a stack pointer, one below the loader's end
	await assert.rejects(k22Install(s, app(3, B.codec[0] - ST.app_base + 0x800)), /runs to/);
	await assert.rejects(s.writeImage(B.owned, [[ST.app_base, app(3, 0x52000)]]), /outside the flash the firmware owns/);
	await assert.rejects(k22Install(s, new Uint8Array(64)), /stack pointer/);
	await assert.rejects(s.writeImage(B.owned, [[0x2000, app(3, 64)]]), /outside the flash/);
	const merged = new Uint8Array(B.codec_image_end - ST.app_base - 0x800).fill(0xFF);
	merged.set(img);
	await assert.rejects(k22Install(s, merged), /runs to/, "an image ending inside the codec area");
	assert.deepEqual(ownedFor(B, k22Segments(B, ST.app_base, img)), B.owned);
});

test("a failed verify aborts before the commit: the loader finds no application", async () => {
	const radio = new FakeHidRFoundry(ST, { badAt: 0x20123 });
	const s = await enter(radio);
	await assert.rejects(k22Install(s, app(5, 300000)), /verify before the commit: sector\(s\) 64, 65 differ/);
	assert.equal(radio.word(ST.app_base), 0xFFFFFFFF, "not committed");
	assert.ok(!radio.events.includes("reset"));
	radio.powerOn();
	assert.equal(radio.mode, "loader", "the radio starts in its loader, where update mode is");
});

test("a pulled cable leaves nothing committed; a second run completes; the restart starts RadioFoundry", async () => {
	const radio = new FakeHidRFoundry(ST, { cutAfter: 10 });
	let s = await enter(radio);
	const img = app(7, 300000);
	await assert.rejects(k22Install(s, img), /lost/);
	assert.equal(radio.word(ST.app_base), 0xFFFFFFFF);
	radio.cutAfter = null;
	s = k22Stub(radio.serialStream(), ST, BOARD, { log: quiet });
	assert.ok(await s.attach(), "the stub still answers");
	await k22Install(s, img);
	assert.deepEqual(radio.flash.subarray(ST.app_base, ST.app_base + img.length), img);
	await s.resetChip();
	assert.equal(radio.mode, "rfoundry");
	assert.equal(radio.events.at(-1), "reset");
	const p = await probe(new Link(radio.serialStream()));
	assert.equal(p.board, B.debuglink_names[0]);
});

test("SPI flash: the radio's first megabyte is never written", async () => {
	const radio = new FakeHidRFoundry(ST);
	const s = await enter(radio);
	await s.configure();
	const all = await s.readFlash(0, 0x200000, { mem: MEM_SPI });
	const low = all.slice();
	low.fill(0, 0x3000, 0x3010);
	await assert.rejects(s.spiWrite(low, 0), /never written/);
	assert.deepEqual(radio.spi, all);
	const upper = all.slice(0x100000);
	upper.fill(0, 0x11000, 0x11020);
	assert.deepEqual(await s.spiWrite(upper, 0x100000), { written: 1, same: 255 });
	assert.deepEqual(radio.spi.subarray(0x100000), upper);
});
