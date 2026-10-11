// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The web flasher's modules, checked against odrflash (fixtures from site/tests/fixtures.py) and run end to end
// against the simulated radio (fake.js). Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, sha256, crc32 } from "../static/js/flasher/bytes.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import { makePlan, KEEP, MERGE, RESTORE, PlanError } from "../static/js/flasher/plan.js";
import { verify, segments, ManifestError } from "../static/js/flasher/manifest.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { Link, crc16, frame, parseFrames, probe } from "../static/js/flasher/link.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import { zip, unzip } from "../static/js/flasher/zip.js";
import { readRadio, makeBundle, openBundle } from "../static/js/flasher/backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug } from "../static/js/flasher/restore.js";
import { Selection } from "../static/js/flasher/select.js";
import { decideCodec, DecisionError } from "../static/js/flasher/decide.js";
import { findKey, readBootKey } from "../static/js/flasher/bootkey.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const fx = json("fixtures.json");
const fb = json("flash-boards.json");
const stock = bin("stock.bin");
const image = bin("image.bin");
const board = fb.boards.dm1701;
const profiles = { [fx.stock_sha256]: fx.profile };
const noWait = { wait: async () => {} };

test("CRC-16/CCITT-FALSE check value and frames", () => {
	assert.equal(crc16(new TextEncoder().encode("123456789")), 0x29B1);
	const f = frame(0x02, 0x41, new Uint8Array([1, 2, 3]));
	const [p] = parseFrames(f);
	assert.equal(p.cmd, 0x02);
	assert.deepEqual([...p.payload], [1, 2, 3]);
});

test("crc32 check value", () => {
	assert.equal(crc32(new TextEncoder().encode("123456789")), 0xCBF43926);
});

test("key derivation matches odrflash", async () => {
	const key = await stockfw.deriveKey(stock, await sha256(fromHex(fx.key)), fb);
	assert.equal(toHex(key), fx.key);
	assert.equal(toHex(key), fx.derived_key);
});

test("codec extraction checks every window", async () => {
	const { segments: segs } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.equal(segs.length, 2);
	const bad = fromHex(fx.key);
	bad[5] ^= 1;
	await assert.rejects(stockfw.extract(stock, bad, fb, profiles), stockfw.StockError);
});

test("keep and merge plans match plan.py", async () => {
	const { segments: codec } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.deepEqual(makePlan(board, [[board.app_base, image]], KEEP).describe(), fx.keep);
	assert.deepEqual(makePlan(board, [[board.app_base, image]], MERGE, codec).describe(), fx.merge);
	assert.throws(() => makePlan(board, [[board.app_base, image]], MERGE), PlanError);
	assert.throws(() => makePlan(board, [[0x08074000, image]], KEEP), PlanError);
});

test("manifest from odrflash verifies; tampering is refused", async () => {
	assert.equal((await verify(fx.manifest, image, fb)).id, "dm1701");
	await assert.rejects(verify(fx.manifest, image, fb, "md380"), ManifestError);
	const bad = image.slice();
	bad[100] ^= 1;
	await assert.rejects(verify(fx.manifest, bad, fb), ManifestError);
});

test("zip round trip", async () => {
	const big = new Uint8Array(100000).fill(0xFF);
	const z = await zip([{ name: "a.txt", data: "hello" }, { name: "b.bin", data: big }]);
	const m = await unzip(z);
	assert.equal(new TextDecoder().decode(m.get("a.txt")), "hello");
	assert.equal(m.get("b.bin").length, 100000);
	assert.ok(z.length < 2000, "0xFF fill compresses");
});

test("codec decision follows odrflash's rules", () => {
	const m = fx.manifest;
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: board.codec.id } }).mode, KEEP);
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" }, haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" } }), DecisionError);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.06 CSV" }).mode, KEEP);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.03 CSV", haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(board, m, {}), DecisionError);
	assert.throws(() => decideCodec(fb.boards.md380, m, {}), DecisionError);
});

test("a la carte selection keeps dependencies and finds prebuilt variants", () => {
	const radio = json("radios.json").radios.find((r) => r.id === "dm1701");
	const s = new Selection(radio, json("features.json").features, json("variants.json").variants);
	s.useVariant("full");
	assert.equal(s.matches(), "full");
	s.set("DMR", false);
	assert.ok(!s.on.has("DMR_SMS") && !s.on.has("HOTSPOT"), "children follow DMR off");
	assert.equal(s.matches(), null);
	assert.match(s.makeCommand(), /CONFIG_DMR=n/);
	s.set("DMR_SMS", true);
	assert.ok(s.on.has("DMR"), "a child turns its parent back on");
	s.useVariant("amateur");
	assert.equal(s.matches(), "amateur");
	assert.ok(!s.on.has("DMR_PRIVACY"));
	s.set("FONT_XL", true);
	assert.ok(!s.on.has("FONT_XL"), "unsupported on this radio");
});

test("codeplug formats", () => {
	assert.equal(detect(stock).id, "stock-firmware");
	assert.equal(detect(new Uint8Array(262709)).id, "tyt-rdt");
	assert.equal(convertersFor("tyt-rdt")[0].status, "planned");
});

// ---- end to end against the simulated radio

async function simulated(state) {
	const sim = await simulation(fb, "dm1701", 7);
	return { sim, radio: new FakeRadio(sim, { state, spiSize: 0x200000 }) };
}

test("first install on stock: keep the codec, then back up through ODR", async () => {
	const { sim, radio } = await simulated("stock");
	const m = { ...fx.manifest };
	const d = decideCodec(sim.board, m, { stockInstalled: "V02.06 CSV" });
	const plan = makePlan(sim.board, segments(m, image), d.mode);
	const codecBefore = radio.flash.slice(0x74714, 0x74714 + 64);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(plan, sim.key);
	assert.deepEqual(radio.violations, []);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + image.length), image);
	assert.deepEqual(radio.flash.subarray(0x74714, 0x74714 + 64), codecBefore, "codec sectors untouched");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	assert.equal(p.codec, sim.board.codec.id);
	assert.equal(p.chip.rdp, 1);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const spi = parts.find((x) => x.what === "spi-flash");
	assert.ok(spi.complete);
	assert.deepEqual(spi.data, radio.spi);
	parts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: sim.stock, complete: true });
	const meta = { radio: { board: "dm1701", name: sim.board.name, chip: p.chip }, firmware_now: { kind: "odr", git: p.git, codec: p.codec },
		firmware_before: { kind: "stock", version: "V02.06 CSV" } };
	const bundle = await openBundle(await makeBundle(meta, parts));
	assert.deepEqual(targets(bundle), ["odr", "stock"]);
	assert.deepEqual(bundle.parts.get("internal-flash").data, radio.flash.subarray(0xC000));
});

test("a read whose reply lost its tail is asked again", async () => {
	const { radio } = await simulated("odr");
	const answer = radio.serialAnswer.bind(radio);
	let reads = 0;
	radio.serialAnswer = (req) => {
		const r = answer(req);
		return req[0] === 0x52 && ++reads === 3 ? r.subarray(0, 1000) : r;	// the radio gave up on a stalled host
	};
	const link = new Link(radio.serialStream(), { timeout: 50 });
	const r = await link.cpsDump(5, 0xC000, 5 * 2045);
	assert.ok(r.complete);
	assert.equal(reads, 6);
	assert.deepEqual(r.data, radio.flash.subarray(0xC000, 0xC000 + 5 * 2045));
});

test("an ODR radio gives its bootloader key: found in the bootloader by its hash, nowhere else", async () => {
	const { sim, radio } = await simulated("odr");
	const link = new Link(radio.serialStream());
	const key = await readBootKey(link, sim.board, sim.fb.key_len);
	assert.deepEqual(key, sim.key);
	// a key at an odd offset is found too; a hash that matches nothing gives nothing
	const region = new Uint8Array(0xC000);
	region.set(sim.key, 0x1235);
	assert.deepEqual(await findKey(region, sim.fb.key_len, sim.board.key_sha256), sim.key);
	assert.equal(await readBootKey(link, { ...sim.board, key_sha256: "00".repeat(32) }, sim.fb.key_len), null);
	// firmware without READ_MEM (or a radio that refuses it): no key, no error
	const answer = radio.serialAnswer.bind(radio);
	radio.serialAnswer = (req) => (req[0] === 0xA5 && req[2] === 0x0C ? frame(0x8C, req[3], new Uint8Array([1])) : answer(req));
	assert.equal(await readBootKey(new Link(radio.serialStream(), { timeout: 50 }), sim.board, sim.fb.key_len), null);
	// the key then flashes the radio with no stock file anywhere
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(sim.board, segments(fx.manifest, image), KEEP), key);
	assert.deepEqual(radio.violations, []);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + image.length), image);
});

test("merge on a radio without a codec, then restore stock from the bundle's stock file", async () => {
	const { sim, radio } = await simulated("odr-nocodec");
	radio.flash.fill(0xFF, 0x60000, 0x100000);		// no codec anywhere
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	assert.equal(p.codec, "absent");
	const key = await stockfw.deriveKey(sim.stock, sim.board.key_sha256, sim.fb);
	const { segments: codec } = await stockfw.extract(sim.stock, key, sim.fb);
	const d = decideCodec(sim.board, fx.manifest, { probe: p, haveStock: true });
	assert.equal(d.mode, MERGE);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(sim.board, segments(fx.manifest, image), MERGE, codec), key);
	assert.equal((await probe(new Link(radio.serialStream()))).codec, sim.board.codec.id);
	// back to stock: the whole stock application, codec included, through a restore plan
	const plan = planStock(sim.stock, key, sim.board, sim.fb);
	assert.equal(plan.mode, RESTORE);
	radio.enterBootloader();
	radio.afterExit = "stock";
	await new Dfu(radio.dfuTransport(), noWait).run(plan, key);
	const [, app] = stockfw.stockApplication(sim.stock, key, sim.board, sim.fb);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + app.length), app);
	assert.deepEqual(radio.violations, []);
});

test("restore an ODR backup exactly, codeplug included", async () => {
	const { sim, radio } = await simulated("odr");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const bundle = await openBundle(await makeBundle({ radio: { board: "dm1701", name: "x", chip: p.chip }, firmware_now: { kind: "odr", git: p.git, codec: p.codec } }, parts));
	const flashBefore = radio.flash.slice(), spiBefore = radio.spi.slice();
	// the radio changes: a new firmware and an edited codeplug
	radio.flash.fill(0x00, 0xC000, 0x20000);
	radio.spi.fill(0x11, 0x3000, 0x5000);
	radio.spi.fill(0x22, 0x9B000, 0x9C000);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(planInternal(bundle, sim.board), sim.key);
	assert.deepEqual(radio.flash, flashBefore);
	const sectors = codeplugSectors(bundle.parts.get("spi-flash").data, radio.spi);
	assert.deepEqual(sectors.map(([a]) => a), [0x3000, 0x4000, 0x9B000]);
	assert.deepEqual(await writeCodeplug(new Link(radio.serialStream()), sectors), []);
	assert.deepEqual(radio.spi, spiBefore);
});
