// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The sgl-hid backend's pieces of the web flasher against odrflash (fixtures from site/tests/fixtures.py): the update file read,
// written and repacked, the codec placed and merged, and every frame the updater sends, byte for byte as updatefile.py,
// k22fcodec.py and hidboot.py make them. Then the updater over the WebHID link against the model loader, and the
// refusals (wrong model, a bad sum, an image too large, a stock build the page can't take the codec from).

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

import { sha256, toHex, crc32, u32le } from "../static/js/flasher/bytes.js";
import * as g from "../static/js/flasher/hidboot.js";
import * as gc from "../static/js/flasher/k22fcodec.js";
import { FakeHidLoader, FakeHid, synthetic, makeUpdateFile, hidSimulation } from "../static/js/flasher/fakehid.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const FB = json("flash-boards.json");
const G = FB.hid;
const fx = json("hid.json");
// the fixture's synthetic build as one the page supports, with the tag the fixture used
const GS = { ...G, specs: { synthetic: fx.spec }, placed: { ...G.placed, synthetic: { tag: fx.tag } } };
const noSleep = { sleep: async () => {} };

test("the backend's data in the flasher tables matches hidboot.js", () => {
	assert.ok(G.boards.length > 0);
	for (const b of G.boards) assert.equal(FB.boards[b].family, "usb-hid-sgl", b);
	assert.deepEqual(G.update, { period: g.PERIOD, info_len: g.INFO_LEN, body_at: g.BODY_AT, flash_base: g.FLASH_BASE });
	assert.deepEqual(G.loader, { vid: g.VID, pid: g.PID, report: g.REPORT, step_timeout_ms: g.STEP_TIMEOUT, erase_timeout_ms: g.ERASE_TIMEOUT });
	assert.equal(G.flash_base, g.FLASH_BASE);
	// every supported build: 171 branches, and the code region the layout gives is the length its pack pins
	assert.ok(Object.keys(G.specs).length > 0);
	assert.deepEqual(Object.keys(G.specs).sort(), Object.keys(G.placed).sort());
	for (const [name, s] of Object.entries(G.specs)) {
		assert.equal(s.branches.length, 171, name);
		const { end } = gc.layout(G, s);
		assert.equal(end - G.code_base, G.placed[name].code_len, name);
		assert.ok(end <= s.tab.flash && s.tab.flash >= G.table_lo && s.tab.flash + s.tab.len <= G.table_hi, name);
	}
});

test("the update file reads as updatefile.py reads it", async () => {
	const u = new g.UpdateFile(bin("hid_donor.sgl"));
	const i = fx.info;
	assert.deepEqual([u.start, u.length, u.addrWidth, u.lenWidth, u.blockSize, u.useFprog, u.sendTag, u.room, u.pad, u.infoAt],
		[i.start, i.length, i.addr_width, i.len_width, i.block_size, i.use_fprog, i.send_tag, i.room, i.pad, i.info_at]);
	assert.deepEqual([toHex(u.company), toHex(u.model), toHex(u.version), toHex(u.tag)], [i.company, i.model, i.version, i.tag]);
	assert.equal(u.label(), i.label);
	const flat = g.unpack(u);
	assert.equal(await sha256(flat), fx.unpacked_sha256);
	assert.equal(await sha256(flat.subarray(0, 0x7B800 - 0x4000)), fx.stock_sha256);
});

test("codec placement and merge are byte-identical to k22fcodec.py", async () => {
	const flat = g.unpack(bin("hid_donor.sgl"));
	assert.equal((await gc.findSpec(GS, flat)).profile, "synthetic");
	const pk = await gc.pack(GS, flat, fx.spec);
	const p = fx.placed;
	assert.deepEqual([pk.mainNew, pk.lowNew, pk.code.length, crc32(pk.code), crc32(pk.tab)], [p.main_new, p.low_new, p.code_len, p.code_crc, p.tab_crc]);
	assert.equal(toHex(gc.descriptor(GS, pk, fx.tag)), p.descriptor);
	const merged = gc.merge(GS, bin("hid_app.bin"), pk);
	assert.equal(merged.length, fx.merged_len);
	assert.equal(await sha256(merged), fx.merged_sha256);
	assert.equal(u32le(merged, G.desc_addr - G.flash_base), G.desc_magic);
});

test("the update file the page sends is byte-identical to odrflash's, with and without the codec", async () => {
	const donor = await gc.readDonor(GS, bin("hid_donor.sgl"));
	const out = await gc.updateFile(GS, donor, bin("hid_app.bin"));
	assert.equal(out.profile, "synthetic");
	assert.equal(await sha256(out.file), fx.out_sha256);
	assert.deepEqual(out.file, bin("hid_out.sgl"));
	const keep = await gc.updateFile(GS, donor, bin("hid_app.bin"), { codec: "keep" });
	assert.equal(keep.profile, null);
	assert.equal(await sha256(keep.file), fx.keep_sha256);
	// the result is a valid update file whose image is what was merged
	assert.deepEqual(g.unpack(out.file).subarray(0, out.image.length), out.image);
	// the donor's own image packed back gives the donor
	assert.deepEqual(g.pack(g.unpack(donor.file), donor.file), bin("hid_donor.sgl"));
});

test("the updater sends the frames hidboot.py sends, and the model loader receives the body", async () => {
	const file = new g.UpdateFile(bin("hid_out.sgl"));
	const fake = new FakeHidLoader(file);
	const frames = [];
	const rec = { write: (r) => { frames.push(r); fake.write(r); }, read: () => fake.read() };
	const seen = [];
	await new g.Updater(rec, noSleep).flash(file, { progress: (d) => seen.push(d) });
	assert.equal(frames.length, fx.frames.count);
	assert.deepEqual(frames.slice(0, 12).map(toHex), fx.frames.head);
	assert.deepEqual(frames.slice(-3).map(toHex), fx.frames.tail);
	const all = new Uint8Array(frames.length * g.REPORT);
	frames.forEach((f, i) => all.set(f, i * g.REPORT));
	assert.equal(await sha256(all), fx.frames.sha256);
	assert.deepEqual(fake.received(), file.body.subarray(0, file.length));
	assert.equal(seen.at(-1), file.length);
});

test("over the WebHID link: the radio chosen, the install, the link closed", async () => {
	const file = new g.UpdateFile(bin("hid_out.sgl"));
	const hid = new FakeHid(new FakeHidLoader(file));
	const link = await g.HidLink.request(hid);
	assert.ok(hid.device.opened);
	await new g.Updater(link, noSleep).flash(file);
	assert.deepEqual(hid.loader.received(), file.body.subarray(0, file.length));
	await link.close();
	assert.ok(!hid.device.opened);
	// nothing in update mode: the browser's chooser comes back empty
	await assert.rejects(g.HidLink.request(new FakeHid(null, { present: false })), (e) => e.code === "no_device");
	await assert.rejects(g.HidLink.request(null), (e) => e.code === "no_hid");
	// a radio that never answers: the step times out
	const silent = { write: () => {}, read: async () => null };
	await assert.rejects(new g.Updater(silent, noSleep).flash(file), (e) => e.code === "no_answer");
});

test("refusals: wrong model, a damaged block, a damaged file, not in update mode", async () => {
	const file = new g.UpdateFile(bin("hid_out.sgl"));
	const wrong = new FakeHidLoader(file, { reject: file.model });
	await assert.rejects(new g.Updater(wrong, noSleep).flash(file), (e) => e.code === "refused" && /model/.test(e.message));
	assert.equal(wrong.mem.size, 0);
	// a block damaged on the way: the loader's running sum differs from the host's, and it refuses the first checksum
	const fake = new FakeHidLoader(file);
	let blocks = 0;
	const damaging = { read: () => fake.read(), write: (r) => {
		const p = g.parse(r);
		if (fake.state === "data" && p[0] !== 0x45 && blocks++ === 3) p[p.length - 1] ^= 1;
		fake.write(g.frame(p));
	} };
	await assert.rejects(new g.Updater(damaging, noSleep).flash(file), (e) => e.code === "nak" && /checksum after 0x400/.test(e.message));
	const notReady = { write: () => {}, read: async () => g.frame(new TextEncoder().encode("HELLO")) };
	await assert.rejects(new g.Updater(notReady, noSleep).flash(file), (e) => e.code === "no_update_mode");
	// a file whose sum check doesn't match its contents is refused before anything is sent
	const damaged = bin("hid_donor.sgl").slice();
	damaged[damaged.length - 40] ^= 1;
	assert.throws(() => new g.UpdateFile(damaged), (e) => e.code === "sum");
	assert.throws(() => new g.UpdateFile(new TextEncoder().encode("nope").slice(0, 4)), (e) => e.code === "not_sgl");
	const other = bin("hid_donor.sgl").slice();
	other[0] = 0x58;
	assert.throws(() => new g.UpdateFile(other), (e) => e.code === "not_sgl");
});

test("refusals: an image too large, an unsupported stock build", async () => {
	const donor = await gc.readDonor(GS, bin("hid_donor.sgl"));
	// the application reaching into the codec region
	const big = new Uint8Array(G.code_base - G.flash_base + 1);
	await assert.rejects(gc.updateFile(GS, donor, big), (e) => e.code === "app_too_large");
	// without the codec: more than the file holds
	await assert.rejects(gc.updateFile(GS, donor, new Uint8Array(donor.file.room + 1).fill(1), { codec: "keep" }), (e) => e.code === "too_large");
	assert.throws(() => g.pack(new Uint8Array(donor.file.data.length), donor.file), (e) => e.code === "too_large");
	// the real specs don't know the synthetic build
	const plain = await gc.readDonor(G, bin("hid_donor.sgl"));
	assert.equal(plain.spec, null);
	await assert.rejects(gc.updateFile(G, plain, bin("hid_app.bin")), (e) => e.code === "unsupported" && /gd77-v4\.3\.9/.test(e.message));
	// a stock file that differs from the spec in one byte
	const flat = donor.flat.slice();
	flat[0x54500 - 0x4000] ^= 1;
	assert.equal(await gc.findSpec(GS, flat), null);
	await assert.rejects(gc.pack(GS, flat, fx.spec), (e) => e.code === "window");
	await assert.rejects(gc.pack(GS, donor.flat, { ...fx.spec, words: [[0x54100, 0x54503]] }), (e) => e.code === "word");
	await assert.rejects(gc.pack(GS, donor.flat, { ...fx.spec, branches: [...fx.spec.branches, [0x54300, 0x4500]] }), (e) => e.code === "branch");
	await assert.rejects(gc.pack(GS, donor.flat, fx.spec, 0x67800), (e) => e.code === "overlap");
});

test("an image merged by make keeps only its application part", () => {
	const app = new Uint8Array(0x100).fill(7);
	const merged = new Uint8Array(G.desc_addr + G.desc_len - G.flash_base).fill(0xFF);
	merged.set(app);
	new DataView(merged.buffer).setUint32(G.desc_addr - G.flash_base, G.desc_magic, true);
	assert.deepEqual(gc.ownImage(G, merged), app);
	assert.equal(gc.ownImage(G, app), app);
});

test("the page's practice radio: a synthetic file the page merges and installs", async () => {
	const sim = await hidSimulation(G);
	const donor = await gc.readDonor(sim.hid, sim.donor);
	assert.equal(donor.spec.profile, "synthetic");
	const out = await gc.updateFile(sim.hid, donor, new Uint8Array(4000).fill(0x11));
	const file = new g.UpdateFile(out.file);
	const hid = new FakeHid(new FakeHidLoader(file));
	await new g.Updater(await g.HidLink.request(hid), noSleep).flash(file);
	assert.deepEqual(hid.loader.received(), file.body.subarray(0, file.length));
	// the JS builders agree with the Python ones in shape: the same spec layout
	const { spec } = await synthetic();
	assert.deepEqual([spec.main, spec.low, spec.tab].map((w) => [w.flash, w.len]), [fx.spec.main, fx.spec.low, fx.spec.tab].map((w) => [w.flash, w.len]));
	assert.equal(new g.UpdateFile(makeUpdateFile(new Uint8Array(10))).label(), "SG-XR-100 XR-100 V1.00.01");
});

const real = fx.real;
test("a real stock update file gives what odrflash gives (ODR_SGL_STOCK)", { skip: !real || !existsSync(real?.path || "") }, async () => {
	const donor = await gc.readDonor(G, new Uint8Array(readFileSync(real.path)));
	assert.equal(donor.file.label(), real.label);
	assert.equal(donor.spec?.profile ?? null, real.profile);
	const out = await gc.updateFile(G, donor, bin("hid_app.bin"), { codec: real.profile ? "auto" : "keep" });
	assert.equal(await sha256(out.file), real.out_sha256);
});
