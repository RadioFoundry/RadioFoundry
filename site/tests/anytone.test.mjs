// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone update mode (anytone.js, backend anytoneui.js): frames and the host's bytes against rfoundry's anytone.py
// (fixtures from site/tests/fixtures.py), the identity answer, the variant checks (a picked variant that isn't the
// radio's, a variant without an image, a firmware set for another model), the vendor set and the boot block, a whole
// install and restore against the simulated radio (fakeanytone.js), NAKs and a radio that stops answering. The radio
// is found by its data, never by name (AGENTS.md rule 4). Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { webcrypto } from "node:crypto";

import { concat, equal, fromHex, toHex, sha256 } from "../static/js/flasher/bytes.js";
import * as at from "../static/js/flasher/anytone.js";
import { FakeAnytone } from "../static/js/flasher/fakeanytone.js";
import { backendOf } from "../static/js/flasher/backends.js";
import { missingApi } from "../static/js/flasher/manual.js";

globalThis.crypto ||= webcrypto;
const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const FB = json("flash-boards.json");
const RADIOS = json("radios.json").radios;
const fx = json("anytone.json");
const RADIO = RADIOS.find((r) => r.flash.backend === "anytone-update") || assert.fail("no anytone-update radio in radios.json");
const BOARD = FB.boards[RADIO.flash.board];
const models = BOARD.anytone_models;
const runs = (RADIO.variants || []).filter((v) => v.runs?.board === BOARD.id && models[v.code]).map((v) => v.code);
const noImage = (RADIO.variants || []).filter((v) => !v.runs && models[v.code]).map((v) => v.code);
const otherBoard = (RADIO.variants || []).filter((v) => v.runs && v.runs.board !== BOARD.id && models[v.code]);
const enc = (s) => new TextEncoder().encode(s);
const rand = (n, seed = 1) => {
	const a = new Uint8Array(n);
	let x = seed;
	for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; a[i] = x >>> 24; }
	return a;
};
// a radio on the fake: the update mode's protocol object, with short waits
const on = (opts = {}, uopts = {}) => {
	const fake = new FakeAnytone(opts);
	return { fake, u: new at.AnytoneUpdate(fake.stream, { timeout: 50, ...uopts }) };
};

test("anytone: a frame is the address, 32 bytes and the 16-bit sum, byte for byte as anytone.py builds it", () => {
	for (const [addr, chunk, want] of fx.frames) assert.equal(toHex(at.frame(addr, fromHex(chunk))), want);
	const f = at.frame(0x08006000, Uint8Array.of(1, 2));
	assert.deepEqual([f.length, f[0], f[39]], [40, 1, 6]);
	assert.equal(f[37] | (f[38] << 8), 0x00 + 0x60 + 0x00 + 0x08 + 1 + 2);
	assert.throws(() => at.frame(0, new Uint8Array(33)), RangeError);
	assert.equal(at.framesNeeded(33), 2);
});

test("anytone: the identity answer", () => {
	assert.deepEqual(at.parseIdentify(enc("IMODEL1\0\u0001V100\0\0\u0006")), { model: "MODEL1", hw: 1, version: "V100" });
	assert.deepEqual(Object.values(at.parseIdentify(enc("IMODEL2A\u0002V101\0\0\u0006"))).slice(0, 2), ["MODEL2A", 2]);
	for (const bad of ["", "X".repeat(16), "IMODEL1\0\u0001V100\0\0\u0015"]) assert.throws(() => at.parseIdentify(enc(bad)), at.AnytoneError);
});

test("anytone: the host sends exactly the bytes rfoundry's anytone.Radio sends, NAKed frames again", async () => {
	const fake = new FakeAnytone({ model: fx.model, nakEvery: 3 });
	const sent = [];
	const w = fake.stream.write;
	fake.stream.write = async (b) => { sent.push(b.slice()); return w(b); };
	const u = new at.AnytoneUpdate(fake.stream, { timeout: 50 });
	await u.hello();
	const id = await u.identify();
	assert.deepEqual([id.model, id.hw, id.version], fx.identify);
	await u.write(fx.addr, fromHex(fx.data));
	assert.equal(toHex(concat(...sent)), fx.host);
});

test("anytone: models map to the board's variants; a picked variant must be the radio's", () => {
	for (const [code, names] of Object.entries(models)) for (const n of names) assert.equal(at.variantOf(BOARD, n), code);
	assert.equal(at.variantOf(BOARD, "NOPE"), null);
	assert.ok(runs.length, "a variant that runs the board's image");
	const m = models[runs[0]][0];
	assert.deepEqual(at.judge({ board: BOARD, radio: RADIO, model: m, task: "install" }), { ok: true, code: runs[0], why: null, text: "" });
	assert.equal(at.judge({ board: BOARD, radio: RADIO, model: m, task: "install", chosen: runs[0] }).ok, true);
	assert.equal(at.judge({ board: BOARD, radio: RADIO, model: "NOPE", task: "install" }).why, "unknown");
	for (const other of Object.keys(models).filter((c) => c !== runs[0])) {
		const v = at.judge({ board: BOARD, radio: RADIO, model: m, task: "install", chosen: other });
		assert.deepEqual([v.ok, v.why, v.code], [false, "mismatch", runs[0]]);
	}
});

test("anytone: a variant without an image is refused for the install, its own firmware set restores", () => {
	for (const code of noImage) {
		const m = models[code][0];
		const v = at.judge({ board: BOARD, radio: RADIO, model: m, task: "install" });
		assert.deepEqual([v.ok, v.why, v.code], [false, "no_image", code]);
		assert.equal(at.judge({ board: BOARD, radio: RADIO, model: m, task: "restore", spiModels: models[code] }).ok, true);
		assert.equal(at.judge({ board: BOARD, radio: RADIO, model: m, task: "restore", spiModels: models[runs[0]] }).why, "wrong_set");
	}
});

test("anytone: a variant that takes another board's image is refused for the install, its own firmware set restores", () => {
	for (const variant of otherBoard) {
		const m = models[variant.code][0];
		const v = at.judge({ board: BOARD, radio: RADIO, model: m, task: "install" });
		assert.deepEqual([v.ok, v.why, v.code, v.board], [false, "other_board", variant.code, variant.runs.board]);
		assert.equal(at.judge({ board: BOARD, radio: RADIO, model: m, task: "restore", spiModels: models[variant.code] }).ok, true);
	}
});

// A vendor set as the manufacturer ships it: the .CDD (here 8 bytes of header, then the image), the .CDI entry, the .spi
function vendorSet(image, names, addr = BOARD.app_base) {
	const cdi = new Uint8Array(278);
	cdi.set(enc("fw"));
	const dv = new DataView(cdi.buffer);
	dv.setUint32(256, addr, true); dv.setUint32(260, image.length, true); dv.setUint32(264, 8, true);
	const spi = new Uint8Array(51).fill(0x20);
	spi.set([0x20, 0, 0, 0, 1, 0], 0);
	spi.set(new Uint8Array(8).fill(0), 6);
	spi.set(enc(names[0].padEnd(8)), 14);
	spi.set(enc((names[1] || "").padEnd(16)), 26);
	return [{ name: "FW.CDD", data: concat(new Uint8Array(8), image) }, { name: "FW.CDI", data: cdi }, { name: "fw.spi", data: spi }];
}

test("anytone: the vendor set gives the address, the image and the models; a bad set is explained", () => {
	const img = rand(3000);
	const st = at.stockSet(at.sortSet(vendorSet(img, models[runs[0]])));
	assert.equal(st.addr, BOARD.app_base);
	assert.ok(equal(st.image, img));
	assert.deepEqual(st.models, models[runs[0]].slice(0, 2));
	at.checkFits(BOARD, st.addr, st.image);
	assert.throws(() => at.checkFits(BOARD, BOARD.app_base - 0x1000, img), /application area/);
	assert.throws(() => at.stockSet(at.sortSet(vendorSet(img, ["X"]).slice(0, 2))), /needs its/);
	assert.throws(() => at.stockSet({ cdd: img, cdi: new Uint8Array(278 * 2), spi: new Uint8Array(51) }), /2 entries/);
});

test("anytone: the boot block is found by its hash, merged at the slot and split from a merged file", async () => {
	const blk = concat(fromHex(FB.modem_boot.prefix), rand(FB.modem_boot.len - 8, 5));
	const mb = { ...FB.modem_boot, sha256: await sha256(blk) };	// a stand-in: the real block is the manufacturer's
	const stock = concat(rand(100, 2), blk, rand(100, 3));
	assert.ok(equal(await at.findBootBlock(stock, mb), blk));
	assert.equal(await at.findBootBlock(stock, FB.modem_boot), null);
	const app = rand(5000, 4);
	const img = at.installImage(BOARD, app, blk);
	assert.equal(img.length, BOARD.codec_slot[0] - BOARD.app_base + blk.length);
	const back = await at.splitImage(BOARD, img, mb);
	assert.ok(equal(back.block, blk));
	assert.ok(equal(back.image, app.subarray(0, back.image.length)) && back.image.length <= app.length);
	assert.equal((await at.splitImage(BOARD, app, mb)).block, null);
	assert.throws(() => at.installImage(BOARD, new Uint8Array(BOARD.codec_slot[0] - BOARD.app_base + 1), blk), /runs into/);
});

test("anytone: a whole install and restore against the simulated radio, NAKs sent again", async () => {
	const blk = concat(fromHex(FB.modem_boot.prefix), rand(FB.modem_boot.len - 8, 5));
	const data = at.installImage(BOARD, rand(4000, 7), blk);
	const { fake, u } = on({ model: models[runs[0]][0], nakEvery: 9 });
	await u.hello();
	const id = await u.identify();
	assert.equal(at.judge({ board: BOARD, radio: RADIO, model: id.model, task: "install" }).ok, true);
	const seen = [];
	assert.equal(await u.write(BOARD.app_base, data, { onFrame: (i, n) => seen.push([i, n]) }), at.framesNeeded(data.length));
	assert.ok(fake.finished);
	assert.ok(equal(fake.image(), data));
	assert.deepEqual(seen.at(-1), [at.framesNeeded(data.length), at.framesNeeded(data.length)]);
	// the original firmware back, on each variant's radio
	for (const code of Object.keys(models)) {
		const st = at.stockSet(at.sortSet(vendorSet(rand(2000, 8), models[code])));
		const r = on({ model: models[code][0] });
		await r.u.hello();
		const m = (await r.u.identify()).model;
		assert.equal(at.judge({ board: BOARD, radio: RADIO, model: m, task: "restore", spiModels: st.models }).ok, true);
		await r.u.write(st.addr, st.image);
		assert.ok(equal(r.fake.image().subarray(0, st.image.length), st.image));
	}
});

test("anytone: a frame refused every time, a radio gone silent and a stop all end with the radio in update mode", async () => {
	const data = rand(1000);
	const nak = on({ nakEvery: 1 });
	await assert.rejects(nak.u.write(BOARD.app_base, data), /refused the frame .* 3 times \(NAK\)/);
	assert.equal(nak.fake.mem.size, 0);
	const silent = on({ silentAfter: 4 });
	await assert.rejects(silent.u.write(BOARD.app_base, data), /no answer/);
	assert.equal(silent.fake.finished, false);
	const ac = new AbortController();
	const stop = on();
	await assert.rejects(stop.u.write(BOARD.app_base, data, { signal: ac.signal, onFrame: (i) => { if (i === 5) ac.abort(); } }), /stopped after 5 of/);
	const mute = new at.AnytoneUpdate({ buf: new Uint8Array(0), clear() {}, take: () => new Uint8Array(0), async write() {} }, { timeout: 20 });
	await assert.rejects(mute.hello(), /update mode/);
	// an answer that comes late still counts (the stream's arrived() wakes the wait)
	const slow = on({ delay: 10 });
	await slow.u.hello();
	assert.equal(await slow.u.write(BOARD.app_base, rand(64)), 2);
});

test("anytone: the stock PC mode's reads and answers, byte for byte as anytone.py sends and builds them", async () => {
	const pg = fx.program;
	for (const [a, d, want] of pg.replies) {
		assert.equal(toHex(at.readReply(a, fromHex(d))), want);
		assert.equal(toHex(at.parseRead(a, fromHex(d).length, fromHex(want))), d);
	}
	assert.throws(() => at.readRequest(0, at.READ_MAX + 1), RangeError);
	const fake = new FakeAnytone({ memory: [[pg.memory[0], fromHex(pg.memory[1])]] });
	const sent = [];
	const write = fake.stream.write;
	fake.stream.write = async (b) => { sent.push(b); return write(b); };
	const p = new at.AnytoneProgram(fake.stream, { timeout: 50 });
	await p.hello();
	const steps = [];
	assert.equal(toHex(await p.read(pg.addr, pg.n, { onBytes: (d) => steps.push(d) })), pg.read);
	assert.ok(await p.end());
	assert.equal(toHex(concat(...sent)), pg.host);
	assert.deepEqual(steps, [16, 32, 40]);
	assert.ok(fake.ended);
});

test("anytone: a bad answer to a read is asked again, then refused; no PROGRAM answer means no stock firmware", async () => {
	const good = at.readReply(0x100, new Uint8Array(4));
	for (const [bad, why] of [[good.slice(0, -1), /malformed/], [Uint8Array.from(good, (b, i) => (i === 10 ? 0 : b)), /sum/],
		[at.readReply(0x110, new Uint8Array(4)), /answered/]]) assert.throws(() => at.parseRead(0x100, 4, bad), why);
	const fake = new FakeAnytone({ memory: [[0x100, Uint8Array.of(9, 8, 7, 6)]] });
	let n = 0;
	const write = fake.stream.write;
	fake.stream.write = async (b) => { if (b[0] === at.READ && n++ === 0) return; return write(b); };	// the first read is lost
	const p = new at.AnytoneProgram(fake.stream, { timeout: 20 });
	await p.hello();
	assert.deepEqual([...await p.read(0x100, 4)], [9, 8, 7, 6]);
	fake.stream.write = async () => {};
	await assert.rejects(p.read(0x100, 4), at.AnytoneError);
	await assert.rejects(new at.AnytoneProgram(fake.stream, { timeout: 20 }).hello(), /PROGRAM/);
});

test("anytone backend: install and restore steps, no backup, Web Serial for the channels and connect steps", async () => {
	await import("../static/js/flasher/anytoneui.js");
	const b = backendOf(RADIO);
	assert.equal(b.id, "anytone-update");
	assert.ok(b.available(RADIO, FB, "install"));
	assert.ok(!b.available(RADIO, FB, "backup"));
	assert.deepEqual(b.steps("install", { recovering: false }), ["radio", "build", "at_channels", "connect", "at_boot", "write", "done"]);
	assert.deepEqual(b.steps("install", { recovering: true }), ["build", "connect", "at_boot", "write", "done"]);
	assert.deepEqual(b.steps("restore", { recovering: false }), ["at_files", "connect", "restore"]);
	for (const s of ["at_channels", "connect", "at_boot", "write", "done", "at_files", "restore"]) assert.equal(typeof b.views[s], "function", s);
	assert.equal(missingApi(RADIO, "install", "connect", { usb: true, serial: false }), "serial");
	assert.equal(missingApi(RADIO, "install", "at_channels", { usb: true, serial: false }), "serial");
	assert.equal(missingApi(RADIO, "install", "connect", { usb: true, serial: true }), null);
});

test("anytone backend: every string its pages use exists", () => {
	const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));
	const src = readFileSync(new URL("../static/js/flasher/anytoneui.js", import.meta.url), "utf8");
	const keys = new Set([...src.matchAll(/"(flash\.[\w.]+)"/g)].map((m) => m[1]));
	for (const why of ["unknown", "mismatch", "no_image", "other_board", "wrong_set"]) keys.add(`flash.at.refuse_${why}`);
	for (const s of ["at_boot", "at_files", "at_done"]) keys.add(`flash.step.${s}`);
	for (const k of keys) assert.ok(en[k], k);
});
