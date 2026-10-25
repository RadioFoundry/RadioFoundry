// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher's modules, checked against rfoundry (fixtures from site/tests/fixtures.py) and run end to end
// against the simulated radio (fake.js). Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, sha256, crc32, u32le } from "../static/js/flasher/bytes.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import { makePlan, KEEP, MERGE, RESTORE, PlanError } from "../static/js/flasher/plan.js";
import { verify, segments, ManifestError } from "../static/js/flasher/manifest.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { Link, crc16, frame, parseFrames, probe } from "../static/js/flasher/link.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import { zip, unzip } from "../static/js/flasher/zip.js";
import { readRadio, makeBundle, openBundle, bundleName } from "../static/js/flasher/backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug } from "../static/js/flasher/restore.js";
import { Selection } from "../static/js/flasher/select.js";
import { decideCodec, DecisionError } from "../static/js/flasher/decide.js";
import { findKey, readBootKey } from "../static/js/flasher/bootkey.js";
import { codecSegments, readCodec } from "../static/js/flasher/codecsave.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
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

test("key derivation matches rfoundry", async () => {
	const key = await stockfw.deriveKey(stock, await sha256(fromHex(fx.key)), fb);
	assert.equal(toHex(key), fx.key);
	assert.equal(toHex(key), fx.derived_key);
});

// The MD-380 image is far less zero-dominated: the column vote misses hundreds of columns and only the byte-pair
// refinement finds the key (issue 119). Synthetic file from fixtures.py, the same key as rfoundry gives
test("key derivation refines a bad vote (MD-380-like file)", async () => {
	const m = fx.md380like;
	assert.ok(m.vote_wrong > 200, `the vote alone should be far off (${m.vote_wrong} columns)`);
	const key = await stockfw.deriveKey(bin("stock-md380like.bin"), await sha256(fromHex(m.key)), fb);
	assert.equal(toHex(key), m.key);
	assert.equal(toHex(key), m.derived_key);
});

// Opt-in: a real TYT updater file (never in the repository), e.g. RFOUNDRY_MD380_STOCK="MD-380-D2.32(AD).bin" site/tests/run.sh
test("key derivation from a real MD-380 updater file", { skip: !process.env.RFOUNDRY_MD380_STOCK && "set RFOUNDRY_MD380_STOCK" }, async () => {
	const st = await stockfw.stockFile("stock", new Uint8Array(readFileSync(process.env.RFOUNDRY_MD380_STOCK)), fb);
	const key = await stockfw.deriveKey(st.data, fb.boards.md380.key_sha256, fb);
	assert.equal(await sha256(key), fb.boards.md380.key_sha256);
});

test("stock file from a vendor zip: the one .bin with the container header", async () => {
	const plain = await stockfw.stockFile("x.bin", stock, fb);
	assert.equal(plain.data, stock);	// a .bin passes through
	const z = await zip([
		{ name: "Firmware 2.32/Upgrade.exe", data: "MZ" },
		{ name: "Firmware 2.32/MD-380-D2.32(AD).bin", data: stock },
		{ name: "__MACOSX/Firmware 2.32/._MD-380-D2.32(AD).bin", data: new Uint8Array(212) },
	]);
	const got = await stockfw.stockFile("TYT-Tytera-MD-380-FW-v232.zip", z, fb);
	assert.equal(got.name, "MD-380-D2.32(AD).bin");
	assert.deepEqual([...got.data.subarray(0, 64)], [...stock.subarray(0, 64)]);
	assert.equal(got.data.length, stock.length);
	await assert.rejects(stockfw.stockFile("none.zip", await zip([{ name: "readme.txt", data: "hi" }]), fb), stockfw.StockError);
	await assert.rejects(stockfw.stockFile("two.zip", await zip([{ name: "a.bin", data: stock }, { name: "b.bin", data: stock }]), fb),
		stockfw.StockError);
});

test("codec extraction checks every window", async () => {
	const { segments: segs } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.equal(segs.length, 3);	// code (moved), tables, descriptor
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

test("the codec is placed as stockfw.py places it: code moved, tables cut, descriptor", async () => {
	const { segments: codec } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.deepEqual(await Promise.all(codec.map(async ([a, d]) => [a, await sha256(d), d.length])), fx.placed);
	const [, , [da, desc]] = codec;
	assert.equal(da, 0x080FFF80);
	assert.equal(u32le(desc, 0), 0x43534544);
	assert.equal(u32le(desc, 124), crc32(desc.subarray(0, 124)));
	const prof = Object.values(profiles)[0];
	const noMap = { ...prof, map: null, windows: [[prof.windows[0][0], prof.windows[0][1], "0".repeat(64)], ...prof.windows.slice(1)] };
	await assert.rejects(stockfw.place(noMap, [[0, new Uint8Array(4)], [0, new Uint8Array(4)]], fb), stockfw.StockError);
});

test("a manifest with a second range (an image from before the codec moved) is refused", async () => {
	const two = JSON.parse(JSON.stringify(fx.manifest));
	two.ranges.push({ addr: "0x080A0000", size: 16 });
	await assert.rejects(verify(two, image, fb), ManifestError);
	assert.throws(() => makePlan(board, [[board.app_base, image], [0x080A0000, new Uint8Array(16)]], KEEP), PlanError);
});

test("manifest from rfoundry verifies; tampering is refused", async () => {
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

test("codec decision follows rfoundry's rules", () => {
	const m = fx.manifest;
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: board.codec.id } }).mode, KEEP);
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" }, haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" } }), DecisionError);
	// no stock firmware keeps the codec where RadioFoundry does: the stock file is needed
	assert.throws(() => decideCodec(board, m, { stockInstalled: "V02.06 CSV" }), DecisionError);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.06 CSV", haveStock: true }).mode, MERGE);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.03 CSV", haveStock: true }).mode, MERGE);
	// RadioFoundry from before the codec moved reports the old id: its codec is in flash this firmware overwrites
	assert.throws(() => decideCodec(board, m, { probe: { board: "DM-1701", codec: "dm1701-v0206-csv" } }), DecisionError);
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: "dm1701-v0206-csv" }, haveStock: true }).mode, MERGE);
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
	// the MD-380's DTMF: the hardware can, the code isn't written (BOARD_NOT_WRITTEN), so it isn't offered either
	const md380 = json("radios.json").radios.find((r) => r.id === "md380");
	const m = new Selection(md380, json("features.json").features, json("variants.json").variants);
	m.useVariant("full");
	assert.ok(md380.not_written.includes("DTMF") && !md380.unsupported.includes("DTMF"), "not written, not unsupported");
	assert.ok(m.notWritten("DTMF") && !m.supported("DTMF"));
	m.set("DTMF", true);
	assert.ok(!m.on.has("DTMF"), "not written for this radio");
});

test("codeplug formats", () => {
	assert.equal(detect(stock).id, "stock-firmware");
	assert.equal(detect(new Uint8Array(262709)).id, "tyt-rdt");
	assert.equal(convertersFor("tyt-rdt")[0].status, "ready");
	assert.equal(convertersFor("dfuse")[0].status, "planned");
});

// ---- end to end against the simulated radio

async function simulated(state) {
	const sim = await simulation(fb, "dm1701", 7);
	return { sim, radio: new FakeRadio(sim, { state, spiSize: 0x200000 }) };
}

test("first install on stock: the codec merged from the stock file, then back up through RadioFoundry", async () => {
	const { sim, radio } = await simulated("stock");
	const m = { ...fx.manifest };
	assert.throws(() => decideCodec(sim.board, m, { stockInstalled: "V02.06 CSV" }), DecisionError);
	const d = decideCodec(sim.board, m, { stockInstalled: "V02.06 CSV", haveStock: true });
	const { profile, segments: codec } = await stockfw.extract(sim.stock, sim.key, sim.fb);
	// the merge plans against the merged file's layout; with one stock file that is the board's windows
	assert.deepEqual(stockfw.layoutWindows(profile, sim.fb), sim.board.codec.windows);
	assert.deepEqual(codec.map(([a, d]) => [a, d.length]), stockfw.layoutWindows(profile, sim.fb));
	const plan = makePlan(sim.board, segments(m, image), d.mode, codec, stockfw.layoutWindows(profile, sim.fb));
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(plan, sim.key);
	assert.deepEqual(radio.violations, []);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + image.length), image);
	for (const [a, data] of sim.codecSegments) assert.deepEqual(radio.flash.subarray(a - 0x08000000, a - 0x08000000 + data.length), data);
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	assert.equal(p.codec, sim.board.codec.id);
	assert.equal(p.chip.rdp, 1);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const spi = parts.find((x) => x.what === "spi-flash");
	assert.ok(spi.complete);
	assert.deepEqual(spi.data, radio.spi);
	parts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: sim.stock, complete: true });
	const meta = { radio: { board: "dm1701", name: sim.board.name, chip: p.chip }, firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec },
		firmware_before: { kind: "stock", version: "V02.06 CSV" } };
	const bundle = await openBundle(await makeBundle(meta, parts));
	// the memory chip still holds the stock codeplug: written back to RadioFoundry it would be garbage, so it isn't offered
	assert.deepEqual(targets(bundle), ["rfoundry", "stock"]);
	assert.deepEqual(bundle.parts.get("internal-flash").data, radio.flash.subarray(0xC000));
});

test("after the backup, the stock data in RadioFoundry's settings area is claimed; a wrong token changes nothing", async () => {
	const { radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const before = await link.storageInfo();
	assert.equal(before.region, "foreign");
	assert.deepEqual(before.journal, { addr: 0x19000, size: 0x2000 });
	const good = link.chip.bind(link);
	link.chip = async () => ({ ...(await good()), token: 0x12345678 });
	await assert.rejects(link.storageClaim({ timeout: 600 }), /status 7/);
	assert.equal((await link.storageInfo()).region, "foreign");
	link.chip = good;
	assert.equal(await link.storageClaim(), true);
	assert.ok(radio.spi.subarray(0x11000, 0x1B000).every((x) => x === 0xFF));
	assert.ok(radio.events.includes("storage claimed"));
});

test("a read whose reply lost its tail is asked again", async () => {
	const { radio } = await simulated("rfoundry");
	const answer = radio.serialAnswer.bind(radio);
	let reads = 0;
	radio.serialAnswer = (req) => {
		const r = answer(req);
		return req[0] === 0x52 && ++reads === 3 ? r.subarray(0, 1000) : r;	// the radio gave up on a stalled host
	};
	const link = new Link(radio.serialStream(), { timeout: 50 });
	const r = await link.cpsDump(5, 0xC000, 5 * 2045, { chunk: 2045 });
	assert.ok(r.complete);
	assert.equal(reads, 6);
	assert.deepEqual(r.data, radio.flash.subarray(0xC000, 0xC000 + 5 * 2045));
});

test("a RadioFoundry radio gives its bootloader key: found in the bootloader by its hash, nowhere else", async () => {
	const { sim, radio } = await simulated("rfoundry");
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

test("big reads: 32 KB from RadioFoundry, and whatever an older build gives (2045 bytes)", async () => {
	const { radio } = await simulated("rfoundry");
	let reads = 0;
	const answer = radio.serialAnswer.bind(radio);
	radio.serialAnswer = (req) => { if (req[0] === 0x52) reads++; return answer(req); };
	const link = new Link(radio.serialStream());
	let r = await link.cpsDump(1, 0, 0x40000);
	assert.ok(r.complete);
	assert.equal(reads, 8);
	radio.readMax = 2045;
	reads = 0;
	r = await link.cpsDump(1, 0x1000, 0x10000);
	assert.ok(r.complete);
	assert.equal(reads, Math.ceil(0x10000 / 2045));
	assert.deepEqual(r.data, radio.spi.subarray(0x1000, 0x11000));
});

test("merge on a radio without a codec, then restore stock from the bundle's stock file", async () => {
	const { sim, radio } = await simulated("rfoundry-nocodec");
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

test("a backup made before the project was renamed still opens, as a RadioFoundry backup", async () => {
	const { sim, radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const bundle = await openBundle(await makeBundle({ format: "odr-backup/1", radio: { board: "dm1701", name: "x", chip: p.chip },
		firmware_now: { kind: "odr", git: p.git, codec: p.codec } }, parts));
	assert.equal(bundle.meta.firmware_now.kind, "rfoundry");
});

test("restore a RadioFoundry backup exactly, codeplug included", async () => {
	const { sim, radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const bundle = await openBundle(await makeBundle({ radio: { board: "dm1701", name: "x", chip: p.chip }, firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec } }, parts));
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

test("where an update erases the codec, it is read back from the radio and checked window by window", async () => {
	// No STM32 radio's update erases the codec since it moved; a board that can't keep it, with the MD-380's windows
	const board = { ...fb.boards.md380, keep_codec_supported: false };
	const flash = new Uint8Array(0x100000);
	for (let i = 0; i < flash.length; i++) flash[i] = (i * 131 + 7) & 0xFF;
	const windows = [];
	for (const [addr, length] of [[0x08050C44, 0x400], [0x0808FC64, 0x200], [0x080D0FC8, 0x24]]) {
		const o = addr - 0x08000000;
		if (!windows.length) flash.set([fb.codec_sig & 0xFF, (fb.codec_sig >>> 8) & 0xFF, (fb.codec_sig >>> 16) & 0xFF, fb.codec_sig >>> 24], o);
		windows.push([addr, length, await sha256(flash.subarray(o, o + length))]);
	}
	const fbx = { ...fb, profiles: { ["f".repeat(64)]: { board: "md380", codec_profile: board.codec.id, windows } } };
	const link = (mem, refuse = false) => ({ async readMem(addr, n) { if (refuse) throw new Error("refused"); return mem.slice(addr - 0x08000000, addr - 0x08000000 + n); } });
	const got = await readCodec(link(flash), board, fbx);
	assert.deepEqual(got.segments.map(([a]) => a), windows.map(([a]) => a));
	const bad = flash.slice();
	bad[0x0808FC64 - 0x08000000 + 3] ^= 1;
	assert.equal(await readCodec(link(bad), board, fbx), null);
	assert.equal(await readCodec(link(flash, true), board, fbx), null);
	// the decision then needs no stock file, and says where the codec comes from
	const m = { codec: { profile: board.codec.id } };
	assert.equal(decideCodec(board, m, { haveSaved: true }).source, "saved");
	assert.equal(decideCodec(board, m, { haveStock: true }).source, "stock");
	assert.throws(() => decideCodec(board, m, {}), DecisionError);
});

test("a radio that keeps its codec hands over a copy too, from where the merge placed it or where stock keeps it", async () => {
	const key = fromHex(fx.key);
	const { segments: placed } = await stockfw.extract(stock, key, fb, profiles);
	const prof = Object.values(profiles)[0];
	const fbx = { ...fb, profiles };
	const link = (mem) => ({ async readMem(addr, n) { return mem.slice(addr - 0x08000000, addr - 0x08000000 + n); } });
	// a radio on the new layout: the code in sector 11 after the tables
	const now = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, d] of placed) now.set(d, a - 0x08000000);
	const got = await readCodec(link(now), board, fbx);
	assert.deepEqual(got.segments.map(([a, d]) => [a, d.length]), stockfw.placedWindows(prof).map(([a, n]) => [a, n]));
	assert.deepEqual(await codecSegments(got.profile, got.segments, fbx), placed, "written as the stock file's merge writes it");
	// the stock layout (the windows where stock firmware keeps them): placed the same way
	const cont = stockfw.parseContainer(stock, fb);
	const old = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, n] of prof.windows) old.set(stockfw.decrypt(stock, stockfw.appFileOffset(cont, a, fb), n, key, fb), a - 0x08000000);
	const fromOld = await readCodec(link(old), board, fbx);
	assert.equal(fromOld.segments[0][0], prof.windows[0][0]);
	assert.deepEqual(await codecSegments(fromOld.profile, fromOld.segments, fbx), placed);
	now[placed[0][0] - 0x08000000 + 9] ^= 1;
	assert.equal(await readCodec(link(now), board, fbx), null, "a changed byte: no copy");
	// with a copy, a radio whose codec can't be kept or checked needs no stock file
	const m = { codec: { profile: board.codec.id } };
	const rfoundry = (codec) => ({ board: "DM-1701", codec });
	assert.equal(decideCodec(board, m, { probe: rfoundry(board.codec.id), haveSaved: true }).mode, KEEP, "a radio with its codec keeps it");
	assert.equal(decideCodec(board, m, { haveSaved: true }).source, "saved", "no radio answers");
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.06 CSV", haveSaved: true }).source, "saved", "back on stock firmware");
	assert.equal(decideCodec(board, m, { probe: rfoundry("absent"), haveSaved: true }).source, "saved", "RadioFoundry without its codec");
	assert.equal(decideCodec(board, m, { haveSaved: true, requested: KEEP }).mode, KEEP, "keep, as asked");
});

test("backup file names: the model and the local date, letters and digits only, always .zip", () => {
	const when = new Date(2026, 9, 6, 23, 5);
	assert.equal(bundleName({ radio: { board: "dm1801pro", name: "Baofeng DM-1801 Pro", model: "DM-1801 Pro" } }, when), "DM1801Pro_Backup_2026-10-06.zip");
	assert.equal(bundleName({ radio: { board: "dm1701" } }, when), "dm1701_Backup_2026-10-06.zip");
	assert.equal(bundleName({ radio: { board: "x", model: 'A/B: "C"' } }, when), "ABC_Backup_2026-10-06.zip");
});
