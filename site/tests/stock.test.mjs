// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stock TYT-family codeplugs (docs/spec/codeplug-stock-tyt.md): telling the layouts apart, reading the stock records,
// the conversion into the editing model, and the editor's and the Tools page's handling of such files. Every fixture
// is built here from the spec; no radio's data.

import { test } from "node:test";
import assert from "node:assert/strict";
import { classify, latinName, openfwEvidence } from "../static/js/codeplug/detect.js";
import { STOCK, detectStock, readStock, importStock, utf16, RDT_MD380, RDT_UV380 } from "../static/js/codeplug/stock_tyt.js";
import { empty, encode, decode, newChannel, newContact } from "../static/js/codeplug/model.js";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { openFile } from "../static/js/editor/files.js";
import { makeBundle, CODEPLUG_SPAN } from "../static/js/flasher/backup.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";
import { Stock, uv380, putText } from "./stockfix.mjs";

// --- Detection ---

test("utf16 and latin names: a valid name of each kind fails the other check", () => {
	const b = new Uint8Array(32);
	putText(b, 0, 16, "AB");
	assert.deepEqual(utf16(b, 0, 16), { text: "AB", ok: true });
	assert.equal(latinName(b, 0, 16).ok, false);
	const l = new Uint8Array(16).fill(0xFF);
	l.set([0x41, 0x42]);
	assert.deepEqual(latinName(l, 0, 16), { text: "AB", ok: true });
	assert.equal(utf16(l, 0, 8).ok, false);
});

test("classify: a stock MD-UV380 codeplug, an MD-380 image and .rdt, blank, unknown", () => {
	const s = uv380();
	assert.equal(classify(s.b).layout, "tyt-uv380");
	assert.equal(detectStock(s.b), "tyt-uv380");

	const m = new Stock("tyt-md380");
	m.channel(1, { name: "Ch", mode: "fm", rx: 146520000 });
	assert.equal(classify(m.b).layout, "tyt-md380");
	const rdt = new Uint8Array(RDT_MD380);
	rdt.set(m.b, 549);
	assert.equal(classify(rdt).layout, "tyt-md380");

	assert.equal(classify(new Uint8Array(0x1000000).fill(0xFF)).layout, "blank");
	assert.equal(classify(CodeplugImage.blank().bytes).layout, "blank");
	assert.equal(classify(new Uint8Array(RDT_UV380)).layout, "tyt-uv380-rdt");

	let x = 12345;
	const noise = new Uint8Array(0x1A0000).map(() => (x = (x * 1103515245 + 12345) >>> 0) >>> 24);
	assert.equal(classify(noise).layout, "unknown");
});

test("classify: a RadioFoundry codeplug wins over stock bytes it hasn't overwritten", () => {
	const m = empty();
	m.general.dmrId = 3100123;
	m.general.radioName = "N0CALL";
	m.contacts.push(newContact({ number: 1, name: "TG 91", id: 91 }));
	m.channels.push(newChannel({ number: 1, name: "Simplex" }));
	const img = encode(m).bytes;
	assert.equal(classify(img).layout, "openfw");
	const both = uv380().b;
	both.set(img);
	assert.equal(classify(both).layout, "openfw");
	// the stock dump itself has no open-firmware evidence: its erased general settings and UTF-16 names don't count
	assert.equal(classify(uv380().b).good, 3);
});

test("classify: back on stock after RadioFoundry, the stock codeplug wins over RadioFoundry's leftover records", () => {
	// RadioFoundry's codeplug with channels past the first bank, which lie beyond the 256 KB stock codeplug
	const m = empty();
	m.general.dmrId = 3100123;
	m.general.radioName = "N0CALL";
	for (let n = 1; n <= 140; n++) m.channels.push(newChannel({ number: n, name: `Ch ${n}` }));
	const img = encode(m).bytes.slice(0, CODEPLUG_SPAN);
	const md = new Stock("tyt-md380");
	md.channel(1, { name: "Simplex", mode: "fm", rx: 146520000 });
	// the stock software wrote its codeplug over the chip's first 256 KB, general settings included
	const back = img.slice();
	back.set(md.b.subarray(0x2000), 0x2000);
	back.fill(0xFF, 0, 0x2000);
	assert.ok(openfwEvidence(back).good > 0);	// RadioFoundry's later channels are still there
	assert.equal(classify(back).layout, "openfw");	// no stock DMR ID: nothing says the stock software wrote last
	md.general({ name: "N0CALL Radio", id: 3100123 });
	back.set(md.b.subarray(0x2000), 0x2000);
	assert.equal(classify(back).layout, "tyt-md380");
	// RadioFoundry's codeplug written again: its general settings are back and the stock ones gone
	const again = back.slice();
	again.set(img.subarray(0, 0x10000));
	assert.equal(classify(again).layout, "openfw");
});

// --- Records ---

test("readStock: channels, contacts, lists and zones as the spec lays them out", () => {
	const s = readStock(uv380().b, "tyt-uv380");
	assert.deepEqual(s.general, { radioName: "N0CALL Radio", dmrId: 3100123, bootLine1: "Hello", bootLine2: "World" });
	assert.deepEqual(s.channels.map((c) => c.index), [1, 2, 4]);
	const [r, f, l] = s.channels;
	assert.deepEqual([r.mode, r.rxHz, r.txHz, r.colorCode, r.timeslot, r.contact, r.rxList, r.high, r.tot, r.rxOnly],
		["dmr", 439562500, 431962500, 7, 2, 1, 1, true, 60, false]);
	assert.deepEqual([f.mode, f.bw, f.rxTone, f.txTone, f.vox, f.high, f.tot], ["fm", 25, "67.0", "D754I", true, false, 0]);
	assert.deepEqual([l.rxOnly, l.scanList, l.timeslot], [true, 1, 1]);
	assert.deepEqual(s.contacts.map((c) => [c.index, c.name, c.id, c.type]),
		[[1, "TG 91", 91, "group"], [2, "Echo", 9990, "private"], [3, "All", 0xFFFFFF, "all"], [5, "TG 3100", 3100, "group"]]);
	assert.deepEqual(s.rxLists, [{ index: 1, name: "Local", contacts: [1, 5] }, { index: 3, name: "Echo list", contacts: [2, 4] }]);
	assert.equal(s.zones[0].a.length, 20);
	assert.deepEqual(s.zones[1], { index: 2, name: "Both", a: [1], b: [2, 4, 3] });
});

// --- Conversion ---

test("importStock: the editing model, references kept, A/B zones split, notes", () => {
	const m = empty();
	const { counts, notes } = importStock(m, uv380().b, "tyt-uv380");
	assert.deepEqual(counts, { channels: 3, contacts: 4, rxLists: 2, zones: 3 });
	// the stock 0 is "off", which takes the radio's TX timeout here (Simplex and Listen)
	assert.deepEqual(notes, [{ code: "scan_lists", n: 1 }, { code: "tot_off", n: 2 }, { code: "shortened", n: 1 }]);
	assert.deepEqual([m.general.dmrId, m.general.radioName, m.general.bootLine1], [3100123, "N0CALL R", "Hello"]);
	const [r, f, l] = m.channels;
	assert.deepEqual([r.number, r.power, r.contact.name, r.rxList.name, r.colorCode, r.timeslot], [1, 9, "TG 91", "Local", 7, 2]);
	assert.deepEqual([f.number, f.power, f.bw, f.contact, f.rxList, f.vox], [2, 3, 25, null, null, true]);
	assert.deepEqual([l.number, l.contact.name, l.rxList.contacts.map((c) => c.name)], [3, "All", ["Echo"]]);
	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.length]), [["Home", 20], ["Both A", 1], ["Both B", 2]]);

	// through the open-firmware layout and back
	const d = decode(encode(m));
	assert.deepEqual(d.channels.map((c) => [c.name, c.rxHz, c.mode, c.rxTone, c.txTone, c.contact?.name ?? null]),
		[["Repeater", 439562500, "dmr", "", "", "TG 91"], ["Simplex", 446006250, "fm", "67.0", "D754I", null], ["Listen", 145000000, "dmr", "", "", "All"]]);
	assert.deepEqual(d.zones.map((z) => z.channels.map((c) => c.number)), [Array.from({ length: 20 }, (_, i) => [1, 2, 3][i % 3]), [1], [2, 3]]);
	assert.equal(classify(encode(m).bytes).layout, "openfw");
});

test("importStock: over the open-firmware limits, the records others refer to are kept first", () => {
	const s = new Stock("tyt-uv380");
	for (let n = 1; n <= 1100; n++) s.contact(n, { name: `C${n}`, id: n, type: "group" });
	for (let n = 1; n <= 1030; n++) s.channel(n, { name: `Ch${n}`, mode: "dmr", rx: 440000000, contact: n === 1 ? 1090 : 0 });
	s.channel(1031, { name: "Broken", mode: "fm", rx: 0 });
	for (let n = 1; n <= 80; n++) s.list(n, `L${n}`, [1080]);
	s.zone(1, "Z", [1, 1030]);
	const m = empty();
	const { notes } = importStock(m, s.b, "tyt-uv380");
	const n = Object.fromEntries(notes.map((x) => [x.code, x.n]));
	assert.deepEqual(n, { bad_frequency: 1, too_many_channels: 6, tot_off: 1024, too_many_rxLists: 4, too_many_contacts: 76 });
	assert.equal(m.channels.length, 1024);
	assert.equal(m.contacts.length, 1024);
	assert.equal(m.channels[0].contact.name, "C1090");
	assert.ok(m.contacts.some((c) => c.name === "C1080"));
	assert.deepEqual(m.zones[0].channels.map((c) => c.name), ["Ch1"]);
});

test("importStock: after RadioFoundry erased its settings area, the zones are gone and nothing reads as garbage", () => {
	const s = uv380();
	s.b.fill(0xFF, 0x11000, 0x1B000);
	s.b.fill(0x5A, 0x11000, 0x11100);	// a settings record RadioFoundry wrote there (settings-store.md 2)
	s.b.set([0x4F, 0x53, 0x45, 0x54], 0x11000);
	assert.equal(classify(s.b).layout, "tyt-uv380");
	const m = empty();
	const { counts, notes } = importStock(m, s.b, "tyt-uv380");
	assert.deepEqual(counts, { channels: 3, contacts: 4, rxLists: 2, zones: 0 });
	assert.ok(notes.some((x) => x.code === "settings_area"));
	assert.ok(!importStock(empty(), uv380().b, "tyt-uv380").notes.some((x) => x.code === "settings_area"));
});

// --- The editor's Open and the Tools page ---

test("openFile: a stock dump or backup converts; a RadioFoundry image opens; other layouts are foreign", async () => {
	const s = uv380();
	const dump = new Uint8Array(0x1000000).fill(0xFF);
	dump.set(s.b);
	const f = await openFile("dump.bin", dump);
	assert.equal(f.kind, "converted");
	assert.equal(f.from, "tyt-uv380");
	assert.deepEqual(f.counts, { channels: 3, contacts: 4, rxLists: 2, zones: 3 });
	assert.equal(decode(f.image).channels.length, 3);
	assert.equal(f.model.channels[0].name, "Repeater");

	const meta = { radio: { board: "dm1701", name: "Baofeng DM-1701" }, firmware_now: { kind: "rfoundry" }, firmware_before: { kind: "stock" } };
	const zipped = await makeBundle(meta, [{ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: dump, complete: true }]);
	const b = await openFile("backup.zip", zipped);
	assert.deepEqual([b.kind, b.from, b.board], ["converted", "tyt-uv380", "dm1701"]);
	// a firmware-only backup: no codeplug, and the report says what the backup held
	const fw = await makeBundle(meta, [{ file: "internal-flash.bin", what: "internal-flash", addr: 0x0800C000, data: new Uint8Array(64), complete: true }]);
	await assert.rejects(openFile("backup.zip", fw), (e) => e.code === "backup_no_codeplug" &&
		JSON.stringify(e.params) === JSON.stringify({ board: "dm1701", parts: ["internal-flash"], spi: 0 }));

	const md = new Stock("tyt-md380");
	md.channel(1, { name: "Ch", mode: "fm", rx: 146520000 });
	const rdt = new Uint8Array(RDT_MD380);
	rdt.set(md.b, 549);
	assert.deepEqual([(await openFile("x.rdt", rdt)).kind, (await openFile("x.bin", md.b)).kind], ["converted", "converted"]);

	assert.equal((await openFile("rfoundry.bin", f.image.bytes)).kind, "image");
	assert.equal((await openFile("blank.bin", new Uint8Array(0x120000).fill(0xFF))).kind, "image");
	assert.deepEqual(await openFile("x.rdt", new Uint8Array(RDT_UV380)), { kind: "foreign", format: "tyt-uv380-rdt", board: null });
	let x = 7;
	const noise = new Uint8Array(0x120000).map(() => (x = (x * 1103515245 + 12345) >>> 0) >>> 24);
	assert.deepEqual(await openFile("noise.bin", noise), { kind: "foreign", format: "unknown", board: null });
});

test("Tools page: a stock dump and an MD-380 file convert to a RadioFoundry codeplug image and to qdmr YAML", async () => {
	const dump = new Uint8Array(0x1000000).fill(0xFF);
	dump.set(uv380().b);
	assert.equal(detect(dump, "spi.bin").id, "tyt-stock");
	const [toImg, toYaml] = convertersFor("tyt-stock");
	const img = await toImg.convert(dump, "spi.bin");
	assert.equal(img.name, "spi-rfoundry.bin");
	assert.equal(classify(img.data).layout, "openfw");
	assert.deepEqual(decode(CodeplugImage.from(img.data)).channels.map((c) => c.name), ["Repeater", "Simplex", "Listen"]);
	const y = new TextDecoder().decode((await toYaml.convert(dump, "spi.bin")).data);
	assert.match(y, /name: Repeater/);

	const md = new Stock("tyt-md380");
	md.channel(1, { name: "Ch", mode: "fm", rx: 146520000 });
	assert.equal(detect(md.b, "x.bin").id, "tyt-raw");
	const raw = await convertersFor("tyt-raw")[0].convert(md.b, "x.bin");
	assert.equal(decode(CodeplugImage.from(raw.data)).channels[0].rxHz, 146520000);
	await assert.rejects(convertersFor("tyt-raw")[0].convert(new Uint8Array(0x40000), "x.bin"), /no stock codeplug/);
	assert.equal(convertersFor("tyt-uv380-rdt").length, 0);

	const meta = { radio: { board: "dm1701", name: "Baofeng DM-1701" }, firmware_now: { kind: "rfoundry" } };
	const zipped = await makeBundle(meta, [{ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: dump, complete: true }]);
	const fromBackup = await convertersFor("rfoundry-backup")[0].convert(zipped);
	assert.equal(classify(fromBackup.data).layout, "openfw");
});
