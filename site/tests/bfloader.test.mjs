// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory update mode through the Baofeng updater (bfloader.js): the bytes it sends against tools/serial_loader.py's
// Flasher (fixtures from site/tests/fixtures.py), its framing, retries and timeouts against the JavaScript port of
// serial_loader's FakeBootloader (fakec7000.js FakeBfLoader), the install image the page writes that way, and the route
// offered on every HR_C7000 radio. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { concat, equal } from "../static/js/flasher/bytes.js";
import { BfLoader, BfError, xmodem, headerBlock, dataFrame, checkImage, blocksNeeded } from "../static/js/flasher/bfloader.js";
import { FakeBfLoader, FakeC7000, c7000Simulation, fakeC7000Options } from "../static/js/flasher/fakec7000.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { factoryRoute } from "../static/js/flasher/c7000ui.js";
import { factoryInstallImage } from "../static/js/flasher/iapui.js";
import { backendOf } from "../static/js/flasher/backends.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const FB = json("flash-boards.json");
const C = FB.c7000;
const RADIOS = json("radios.json").radios;
// one radio of each kind, found by what it is, not by name (AGENTS.md rule 4)
const ofKind = (f, what) => RADIOS.find(f) || assert.fail(`no ${what} in radios.json`);
const FILE_ID = ofKind((r) => r.flash.update_mode === "baofeng" && factoryRoute(r, FB)?.needs === "file", "Baofeng-loader radio with a stock file").id;
const BACKUP_ID = ofKind((r) => r.flash.update_mode === "baofeng" && factoryRoute(r, FB)?.needs === "backup", "Baofeng-loader radio without a stock file").id;
const IAP_ID = ofKind((r) => r.flash.update_mode === "iap", "IAP radio").id;
const fx = json("bfloader.json");
const APP_MAX = C.app_end - C.app_start;

// A cable to a radio in update mode: what the host writes goes to the fake loader (and is recorded), what it answers
// lands in the stream's buffer as a SerialStream's would
function bench(opts = {}) {
	const s = {
		buf: new Uint8Array(0), sent: [],
		clear() { s.buf = new Uint8Array(0); },
		take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
		async write(b) { s.sent.push(b.slice()); fake.feed(b); },
	};
	const arrive = (b) => { s.buf = concat(s.buf, b); };
	const fake = new FakeBfLoader({ id: fx.id, send: (b, ms = 0) => (ms ? setTimeout(() => arrive(b), ms) : arrive(b)), ...opts });
	return { s, fake };
}
// an application-like image (c7.target refuses one byte repeated)
const app = (n, seed = 1) => {
	const a = new Uint8Array(n);
	let x = seed;
	for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; a[i] = x >>> 24; }
	return a;
};
const loaderOn = (s, opts = {}) => new BfLoader(s, { id: fx.id, appMax: APP_MAX, stepTimeout: 200, poll: 50, ...opts });

test("Baofeng updater: the host sends exactly the bytes serial_loader.py's Flasher sends, NAKed blocks included", async () => {
	const raw = bin("bfloader_image.bin");
	const { s, fake } = bench({ corrupt: fx.corrupt, eraseMs: 30 });
	const l = loaderOn(s);
	assert.deepEqual(await l.listen(2000), { id: fx.id });
	const pages = [];
	const phases = [];
	assert.equal(await l.write(raw, { name: fx.name, onPage: (i, n) => pages.push([i, n]), onPhase: (p) => phases.push(p) }), fx.blocks);
	assert.ok(equal(concat(...s.sent), bin("bfloader_host.bin")), "byte for byte the Python tool's stream");
	assert.deepEqual(fake.errors, []);
	assert.equal(fake.naks, fx.naks);
	assert.deepEqual(fake.header, fx.header);
	assert.ok(equal(fake.image(), raw));
	assert.ok(fake.done);
	assert.deepEqual(phases, ["erase", "write"]);
	assert.deepEqual(pages.at(-1), [fx.blocks, fx.blocks]);
});

test("Baofeng updater: CRC-16/XMODEM, the header block and the block frame as 13.2 describes them", () => {
	assert.equal(xmodem(new TextEncoder().encode("123456789")), 0x31C3);
	assert.equal(xmodem(new Uint8Array(0)), 0);
	const hb = headerBlock("/x/rfoundry_x_codec.bin", 870000);
	assert.equal(hb.length, 128);
	assert.equal(new TextDecoder().decode(hb.subarray(0, 28)), "rfoundry_x_codec.bin\u0000870000\u0000");
	const f = dataFrame(256, Uint8Array.of(1, 2));
	assert.deepEqual([...f.subarray(0, 4)], [0x00, 0xFF, 1, 2], "block number wraps, its complement");
	assert.equal(f.length, 1028);
	assert.ok(f.subarray(4, 1026).every((b) => b === 0), "zero padding");
	assert.equal(blocksNeeded(1025), 2);
	for (const [bad, what] of [[new Uint8Array(0), "empty"], [new Uint8Array(APP_MAX + 1).fill(1), "too big"], [new Uint8Array(4096).fill(0xFF), "blank"]])
		assert.throws(() => checkImage(bad, APP_MAX), BfError, what);
	checkImage(new Uint8Array(APP_MAX).fill(1), APP_MAX);
});

test("Baofeng updater: a block refused every time stops the write and names it; nothing is sent before the checks", async () => {
	const { s, fake } = bench();
	fake.corrupt = new Set([2]);
	fake.corrupt.delete = () => true;	// damaged on every transfer
	const img = app(4096);
	await assert.rejects(loaderOn(s, { retries: 2 }).write(img), (e) => {
		assert.ok(e instanceof BfError);
		assert.match(e.message, /block 2: refused \(NAK\) after 3 tries/);
		assert.match(e.message, /erased/);
		return true;
	});
	const quiet = bench();
	await assert.rejects(loaderOn(quiet.s).write(new Uint8Array(4096).fill(0xFF)), /blank/);
	assert.equal(quiet.s.sent.length, 0);
});

test("Baofeng updater: silence, a wrong step 2 answer, another radio's loader and a slow erase each stop the write", async () => {
	const img = new Uint8Array(2048).fill(9);
	const silent = bench({ silent: true });
	await assert.rejects(loaderOn(silent.s).listen(300), /did not report update mode/);
	await assert.rejects(loaderOn(silent.s).write(img), /step 1: no answer/);
	assert.equal(await loaderOn(silent.s).probe(200), null);
	await assert.rejects(loaderOn(bench({ badStep2: true }).s).write(img), /step 2: the radio answered 4D 01 08, expected 4D 01 09/);
	// another radio's loader: an ID of the same length (step 2 announces it), or a made-up one
	const otherId = Object.values(C.boards).map((b) => b.bootloader_id).find((x) => x && x !== fx.id && x.length === fx.id.length)
		|| "X".repeat(fx.id.length);
	const other = bench({ id: otherId });
	await assert.rejects(loaderOn(other.s).listen(1000), new RegExp(`step 3 \\(model\\).*"${otherId}", not ${fx.id}: this is another radio`));
	assert.equal(other.fake.header, null, "nothing erased");
	const slow = bench({ eraseMs: 1500 });
	await assert.rejects(loaderOn(slow.s, { eraseTimeout: 200 }).write(img), /step 7 \(erase\): no answer/);
});

test("Baofeng updater: listening passes over other answers until the loader acknowledges; the probe stops before step 4", async () => {
	const { s, fake } = bench();
	// the boot ROM's greeting and a firmware answering something first, then the loader
	let others = 3;
	const feed = fake.feed.bind(fake);
	fake.feed = (b) => (others-- > 0 ? s.buf = concat(s.buf, Uint8Array.of(0x02, 0x24, 0x00, 0x03)) : feed(b));
	const lines = [];
	assert.deepEqual(await loaderOn(s, { log: (l) => lines.push(l) }).probe(2000), { id: fx.id });
	assert.equal(fake.state, 4, "steps 1-3 only");
	assert.equal(fake.header, null);
	assert.ok(lines.some((l) => /not the update mode \(02\)/.test(l)));
	const stop = new AbortController();
	stop.abort();
	await assert.rejects(loaderOn(bench({ silent: true }).s).listen(5000, { signal: stop.signal }), /stopped/);
});

test("Baofeng updater: steps 1-3 once per power-on; the instance that did them writes, a new one finds nothing", async () => {
	// the radio (hardware-dm32.md 13.2): after step 3 it ignores step 1 and waits for step 4 until switched off
	const { s, fake } = bench();
	const first = loaderOn(s);
	assert.deepEqual(await first.listen(1000), { id: fx.id });
	const sent = s.sent.length;
	await assert.rejects(loaderOn(s).listen(300), /did not report update mode/, "a second handshake goes unanswered");
	assert.ok(fake.ignored > 0 && fake.state === 4 && !fake.errors.length, "step 1 passed over, the loader still waits for step 4");
	assert.equal(await loaderOn(s).probe(200), null);
	s.clear();
	const n = s.sent.length;
	assert.deepEqual(await first.listen(1000), { id: fx.id }, "found already: nothing sent");
	assert.deepEqual(await first.hello(), { id: fx.id });
	assert.equal(s.sent.length, n);
	assert.ok(n > sent);
	const img = new Uint8Array(3000).map((_, i) => (i * 13 + 1) & 0xFF);
	assert.equal(await first.write(img), 3);
	assert.ok(fake.done && !fake.errors.length && equal(fake.image(), img));
});

// ---- the install the page writes that way

async function withFile() {
	const radio = RADIOS.find((r) => r.id === FILE_ID);
	const sim = await c7000Simulation(C, FILE_ID, 7000, radio);
	const got = await c7.locateCodec(sim.progmem, sim.c, FILE_ID);
	return { radio, sim, slot: c7.slotBytes(sim.c, FILE_ID, got) };
}

test("factory install image: the codec slot is always written, a kept slot is refused", async () => {
	const { sim, slot } = await withFile();
	const image = app(300 * 1024);
	const d = await c7.decide(sim.c, FILE_ID, { savedSlot: slot });
	const data = factoryInstallImage(sim.c, FILE_ID, image, d);
	const off = C.boards[FILE_ID].slot[0] - C.app_start;
	assert.ok(equal(data.subarray(0, image.length), image));
	assert.ok(data.subarray(image.length, off).every((b) => b === 0xFF), "0xFF up to the slot");
	assert.ok(equal(data.subarray(off, off + slot.length), slot), "the slot where RadioFoundry reads it");
	assert.ok(data.length <= off + slot.length + 3, "the erased tail is not sent");
	assert.throws(() => factoryInstallImage(sim.c, FILE_ID, image, { mode: "keep", slot: null }), /can't be kept/);
	assert.throws(() => factoryInstallImage(sim.c, FILE_ID, image, null), c7.CodecError);
	assert.equal(factoryInstallImage(sim.c, FILE_ID, image, { mode: "none" }).length, image.length, "without a codec, only when asked");
});

test("practice radio: switched on with its keys held it enters the updater, which erases the area and writes the install", { timeout: 60000 }, async (t) => {
	const { radio, sim, slot } = await withFile();
	const o = fakeC7000Options(radio, FB);
	assert.equal(o.loaderId, C.boards[radio.flash.board].bootloader_id);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), speed: 200, romWindow: 50, bootTime: 50, ...o });
	t.after(() => r.stop());
	const image = app(200 * 1024, 2);
	image.set(new TextEncoder().encode("RadioFoundry"), 0x1000);
	const data = factoryInstallImage(sim.c, FILE_ID, image, { mode: "write", slot });
	const l = new BfLoader(r.stream, { id: o.loaderId, appMax: APP_MAX, poll: 100 });
	const listening = l.listen(5000);
	setTimeout(() => r.powerOn({ keys: true }), 100);
	await listening;
	assert.equal(r.state, "bfloader");
	await l.write(data, { name: `rfoundry_${FILE_ID}.bin` });
	assert.ok(r.events.includes("erase"));
	assert.ok(equal(r.progmem.subarray(C.app_start, C.app_start + data.length), data));
	assert.ok(r.progmem.subarray(C.app_start + data.length + 1024).every((b) => b === 0xFF), "the rest of the area erased");
	assert.ok(equal(r.progmem.subarray(0, C.app_start), sim.progmem.subarray(0, C.app_start)), "the loader is never written");
	// the radio starts what was written by itself after the end of transfer, no power cycle (13.2)
	await new Promise((ok) => setTimeout(ok, 300));
	assert.ok(r.events.includes("restart"));
	assert.equal(r.state, "firmware");
	assert.equal(r.firmware(), "rfoundry");
	assert.notEqual(r.codecReport(), "absent", "RadioFoundry finds its codec slot");
	// without the keys the radio boots its application, and the updater never answers
	const db = RADIOS.find((x) => x.id === IAP_ID);
	assert.equal(fakeC7000Options(db, FB).loaderId, null, "an IAP radio has no Baofeng updater");
});

// ---- the route on every HR_C7000 radio

test("every HR_C7000 radio has a factory update mode with steps into it, offered in Troubleshooting", () => {
	const c7radios = RADIOS.filter((r) => r.flash.backend === "c7000-stub");
	assert.ok(c7radios.length >= 3);
	const b = backendOf(c7radios[0]);
	for (const r of c7radios) {
		const route = factoryRoute(r, FB);
		assert.ok(route, `${r.id}: a factory update mode`);
		assert.ok(["iap", "baofeng"].includes(r.flash.update_mode), r.id);
		assert.ok(r.flash.bootloader.length > 0, `${r.id}: the steps into update mode`);
		assert.ok(r.flash.bootloader_hold.length > 0, `${r.id}: the keys marked on the picture`);
		if (r.flash.update_mode === "baofeng") assert.ok(C.boards[r.id].bootloader_id, `${r.id}: the ID its loader answers`);
		const [x] = b.recovery.extra(r, FB);
		assert.equal(x.id, "updatemode", r.id);
		assert.equal(x.stuck, r.flash.update_mode === "iap", `${r.id}: only an IAP radio is stuck in update mode`);
	}
	// what the install needs for the codec: the stock file where one is known, else a program memory backup
	assert.equal(factoryRoute(RADIOS.find((r) => r.id === FILE_ID), FB).needs, "file");
	assert.equal(factoryRoute(RADIOS.find((r) => r.id === BACKUP_ID), FB).needs, "backup");
	assert.equal(factoryRoute({ flash: { update_mode: null, board: "x" } }, FB), null);
	const text = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url)));
	for (const k of ["offer_file", "offer_backup", "offer_restore", "codec_file", "codec_backup"]) assert.ok(text[`flash.factory.${k}`], k);
	for (const k of ["file", "backup"]) assert.ok(text[`flash.help.fix.factory_text_${k}`], k);
});

// The cable page's connect step is one panel for every task. Every power cycle sends the answer to the boot ROM's
// greeting ahead (the slow cable mode, no longer a choice), and a ROM load that fails offers the factory update mode
// (c7000ui.js romFailed). The tasks that write take the factory mode's way in; the backup can't (nothing can be read
// through update mode). A task that stopped passing it would leave a radio with a damaged application without the way
// that works
test("the connect step always sends ahead and offers the factory update mode to the install and the restore", () => {
	const src = readFileSync(new URL("../static/js/flasher/c7000ui.js", import.meta.url), "utf8");
	const connects = [...src.matchAll(/(?<!function )connectPanel\(([^\n]*)\)/g)].map((m) => m[1]);
	assert.equal(connects.filter((x) => /onUpdateMode/.test(x)).length, 2, "install and restore pass the factory way in");
	assert.equal(connects.filter((x) => !/onUpdateMode/.test(x)).length, 1, "only the backup goes without: nothing can be read in update mode");
	// every wait for the boot ROM sends the answer ahead
	const waits = [...src.matchAll(/waitForRom\(([^)]*)\)/g)].map((m) => m[1]);
	assert.ok(waits.length >= 2 && waits.every((w) => /repeat: ROM_REPEAT/.test(w)), waits.join(" | "));
	const failed = src.slice(src.indexOf("const romFailed"), src.indexOf("const factoryWay"));
	assert.match(failed, /factoryWay\(\)/);
	assert.match(failed, /factoryButton\(/);
	assert.match(failed, /slowNote\(\)/, "users who knew the slow cable mode are told where it went");
	const text = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url)));
	assert.ok(text["flash.factory.offer_restore"] && text["flash.c7.connect.slow_title"]);
});

test("a program memory backup gives the codec where the radio has no stock file", async () => {
	const sim = await c7000Simulation(C, BACKUP_ID);
	const got = await c7.codecFromFile(sim.progmem, sim.c, BACKUP_ID);
	assert.equal(got.codeAddr, sim.codec.codeAt);
	const d = await c7.decide(sim.c, BACKUP_ID, { fileCodec: got });
	assert.equal(d.mode, "write");
	const data = factoryInstallImage(sim.c, BACKUP_ID, app(4096, 3), d);
	assert.ok(await c7.parseSlot(sim.c, BACKUP_ID, data.subarray(C.boards[BACKUP_ID].slot[0] - C.app_start)));
});

test("a protected region in the area the loader erases refuses the write before anything is sent", async () => {
	let sent = 0;
	const stream = { buf: new Uint8Array(0), clear() {}, take: () => new Uint8Array(0), write: async () => { sent++; } };
	const cal = [{ mem: "progmem", start: C.app_end - 0x1000, end: C.app_end, what: "test (x.md 1)" }];
	const l = new BfLoader(stream, { id: "X", appMax: C.app_end - C.app_start, appStart: C.app_start, protect: cal });
	await assert.rejects(l.write(new Uint8Array(4096).fill(1)), /protected region/);
	assert.equal(sent, 0);
});
