// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Stock TYT-family codeplugs (docs/spec/codeplug-stock-tyt.md): telling the layouts apart, reading the stock records,
// the conversion into the editing model, and the editor's and the Tools page's handling of such files. Every fixture
// is built here from the spec; no radio's data.

import { test } from "node:test";
import assert from "node:assert/strict";
import { classify, latinName } from "../static/js/codeplug/detect.js";
import { STOCK, detectStock, readStock, importStock, utf16, RDT_MD380, RDT_UV380 } from "../static/js/codeplug/stock_tyt.js";
import { empty, encode, decode, newChannel, newContact } from "../static/js/codeplug/model.js";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { putFreq, putTone } from "../static/js/codeplug/fields.js";
import { openFile } from "../static/js/editor/files.js";
import { makeBundle } from "../static/js/flasher/backup.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";

// --- Building stock codeplugs from the spec ---

function putText(b, off, chars, text) {
	b.fill(0, off, off + 2 * chars);
	for (let i = 0; i < Math.min(chars, text.length); i++) {
		b[off + 2 * i] = text.charCodeAt(i) & 0xFF;
		b[off + 2 * i + 1] = text.charCodeAt(i) >> 8;
	}
}
const put16 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = v >> 8;
};

class Stock {
	constructor(format, size = STOCK[format].size) {
		this.L = STOCK[format];
		this.b = new Uint8Array(size).fill(0xFF);
		const L = this.L;
		// unused records as the stock software leaves them (3.3-3.6)
		for (let i = 0; i < L.nChannels; i++) {
			const r = this.b.subarray(L.channels + i * 64, L.channels + (i + 1) * 64);
			r.fill(0, 0, 0x0F);
			r[0] = 0x43;
			r.fill(0, 0x20);
		}
		for (let i = 0; i < L.nContacts; i++) {
			const a = L.contacts + i * 36;
			this.b.set([0xFF, 0xFF, 0xFF, 0xC0], a);
			this.b.fill(0, a + 4, a + 36);
		}
		this.b.fill(0, L.lists, L.lists + L.nLists * 96);
		this.b.fill(0, L.zones, L.zones + L.nZones * 64);
		if (L.zoneExt) this.b.fill(0, L.zoneExt, L.zoneExt + L.nZones * 224);
	}

	general({ name = "", id = 0, boot1 = "", boot2 = "" }) {
		putText(this.b, 0x20B0, 16, name);
		this.b.set([id & 0xFF, (id >> 8) & 0xFF, id >> 16], 0x2084);
		putText(this.b, 0x2040, 10, boot1);
		putText(this.b, 0x2054, 10, boot2);
	}

	channel(n, c) {
		const r = this.b.subarray(this.L.channels + (n - 1) * 64, this.L.channels + n * 64);
		r.fill(0);
		r[0] = (c.mode === "dmr" ? 2 : 1) | (c.wide ? 0x08 : 0) | 0x60;
		r[1] = ((c.cc || 0) << 4) | ((c.ts === 2 ? 2 : 1) << 2) | (c.rxOnly ? 0x02 : 0);
		r[4] = (c.high ? 0x20 : 0) | (c.vox ? 0x10 : 0) | 0x04;
		put16(r, 6, c.contact || 0);
		r[8] = (c.tot || 0) / 15;
		r[0x0A] = c.scan || 0;
		r[0x0C] = c.list || 0;
		r[0x0F] = 0xFF;
		putFreq(r, 0x10, c.rx);
		putFreq(r, 0x14, c.tx ?? c.rx);
		putTone(r, 0x18, c.rxTone || "");
		putTone(r, 0x1A, c.txTone || "");
		putText(r, 0x20, 16, c.name);
	}

	contact(n, { name, id, type }) {
		const a = this.L.contacts + (n - 1) * 36;
		this.b.set([id & 0xFF, (id >> 8) & 0xFF, id >> 16, 0xC0 | { group: 1, private: 2, all: 3 }[type]], a);
		putText(this.b, a + 4, 16, name);
	}

	list(n, name, members) {
		const a = this.L.lists + (n - 1) * 96;
		putText(this.b, a, 16, name);
		members.forEach((x, i) => put16(this.b, a + 0x20 + 2 * i, x));
	}

	zone(n, name, A, B = []) {
		const a = this.L.zones + (n - 1) * 64, x = this.L.zoneExt + (n - 1) * 224;
		putText(this.b, a, 16, name);
		A.slice(0, 16).forEach((c, i) => put16(this.b, a + 0x20 + 2 * i, c));
		A.slice(16).forEach((c, i) => put16(this.b, x + 2 * i, c));
		B.forEach((c, i) => put16(this.b, x + 0x60 + 2 * i, c));
	}
}

function uv380() {
	const s = new Stock("tyt-uv380");
	s.general({ name: "N0CALL Radio", id: 3100123, boot1: "Hello", boot2: "World" });
	s.contact(1, { name: "TG 91", id: 91, type: "group" });
	s.contact(2, { name: "Echo", id: 9990, type: "private" });
	s.contact(3, { name: "All", id: 0xFFFFFF, type: "all" });
	s.contact(5, { name: "TG 3100", id: 3100, type: "group" });
	s.list(1, "Local", [1, 5]);
	s.list(3, "Echo list", [2, 4]);	// contact 4 is not in use
	s.channel(1, { name: "Repeater", mode: "dmr", rx: 439562500, tx: 431962500, cc: 7, ts: 2, contact: 1, list: 1, high: true, tot: 60 });
	s.channel(2, { name: "Simplex", mode: "fm", rx: 446006250, wide: true, rxTone: "67.0", txTone: "D754I", vox: true, tot: 0 });
	s.channel(4, { name: "Listen", mode: "dmr", rx: 145000000, rxOnly: true, cc: 1, contact: 3, list: 3, scan: 1 });
	const A = Array.from({ length: 20 }, (_, i) => [1, 2, 4][i % 3]);
	s.zone(1, "Home", A);
	s.zone(2, "Both", [1], [2, 4, 3]);	// channel 3 is not in use
	return s;
}

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

test("classify: an ODR codeplug wins over stock bytes it hasn't overwritten", () => {
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
	assert.deepEqual(notes, [{ code: "scan_lists", n: 1 }, { code: "shortened", n: 1 }]);
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
	assert.deepEqual(n, { bad_frequency: 1, too_many_channels: 6, too_many_rxLists: 4, too_many_contacts: 76 });
	assert.equal(m.channels.length, 1024);
	assert.equal(m.contacts.length, 1024);
	assert.equal(m.channels[0].contact.name, "C1090");
	assert.ok(m.contacts.some((c) => c.name === "C1080"));
	assert.deepEqual(m.zones[0].channels.map((c) => c.name), ["Ch1"]);
});

test("importStock: after ODR erased its settings area, the zones are gone and nothing reads as garbage", () => {
	const s = uv380();
	s.b.fill(0xFF, 0x11000, 0x1B000);
	s.b.fill(0x5A, 0x11000, 0x11100);	// a settings record ODR wrote there (settings-store.md 2)
	s.b.set([0x4F, 0x53, 0x45, 0x54], 0x11000);
	assert.equal(classify(s.b).layout, "tyt-uv380");
	const m = empty();
	const { counts, notes } = importStock(m, s.b, "tyt-uv380");
	assert.deepEqual(counts, { channels: 3, contacts: 4, rxLists: 2, zones: 0 });
	assert.ok(notes.some((x) => x.code === "settings_area"));
	assert.ok(!importStock(empty(), uv380().b, "tyt-uv380").notes.some((x) => x.code === "settings_area"));
});

// --- The editor's Open and the Tools page ---

test("openFile: a stock dump or backup converts; an ODR image opens; other layouts are foreign", async () => {
	const s = uv380();
	const dump = new Uint8Array(0x1000000).fill(0xFF);
	dump.set(s.b);
	const f = await openFile("dump.bin", dump);
	assert.equal(f.kind, "converted");
	assert.equal(f.from, "tyt-uv380");
	assert.deepEqual(f.counts, { channels: 3, contacts: 4, rxLists: 2, zones: 3 });
	assert.equal(decode(f.image).channels.length, 3);
	assert.equal(f.model.channels[0].name, "Repeater");

	const meta = { radio: { board: "dm1701", name: "Baofeng DM-1701" }, firmware_now: { kind: "odr" }, firmware_before: { kind: "stock" } };
	const zipped = await makeBundle(meta, [{ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: dump, complete: true }]);
	const b = await openFile("backup.zip", zipped);
	assert.deepEqual([b.kind, b.from, b.board], ["converted", "tyt-uv380", "dm1701"]);

	const md = new Stock("tyt-md380");
	md.channel(1, { name: "Ch", mode: "fm", rx: 146520000 });
	const rdt = new Uint8Array(RDT_MD380);
	rdt.set(md.b, 549);
	assert.deepEqual([(await openFile("x.rdt", rdt)).kind, (await openFile("x.bin", md.b)).kind], ["converted", "converted"]);

	assert.equal((await openFile("odr.bin", f.image.bytes)).kind, "image");
	assert.equal((await openFile("blank.bin", new Uint8Array(0x120000).fill(0xFF))).kind, "image");
	assert.deepEqual(await openFile("x.rdt", new Uint8Array(RDT_UV380)), { kind: "foreign", format: "tyt-uv380-rdt", board: null });
	let x = 7;
	const noise = new Uint8Array(0x120000).map(() => (x = (x * 1103515245 + 12345) >>> 0) >>> 24);
	assert.deepEqual(await openFile("noise.bin", noise), { kind: "foreign", format: "unknown", board: null });
});

test("Tools page: a stock dump and an MD-380 file convert to an ODR codeplug image and to qdmr YAML", async () => {
	const dump = new Uint8Array(0x1000000).fill(0xFF);
	dump.set(uv380().b);
	assert.equal(detect(dump, "spi.bin").id, "tyt-stock");
	const [toImg, toYaml] = convertersFor("tyt-stock");
	const img = await toImg.convert(dump, "spi.bin");
	assert.equal(img.name, "spi-odr.bin");
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

	const meta = { radio: { board: "dm1701", name: "Baofeng DM-1701" }, firmware_now: { kind: "odr" } };
	const zipped = await makeBundle(meta, [{ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: dump, complete: true }]);
	const fromBackup = await convertersFor("odr-backup")[0].convert(zipped);
	assert.equal(classify(fromBackup.data).layout, "openfw");
});
