// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock DM-32UV codeplug (docs/spec/dm32-stock-codeplug.md): the page map, the records, detection, the conversion
// into the editing model, and the editor's and the Tools page's handling of such files. Every fixture is built here
// from the spec; no radio's data.

import { test } from "node:test";
import assert from "node:assert/strict";
import { classify } from "../static/js/codeplug/detect.js";
import { DM32_STOCK, pageMap, detectStock, readStock, importStock, dm32Tone } from "../static/js/codeplug/stock_baofeng.js";
import { empty, decode } from "../static/js/codeplug/model.js";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { putFreq } from "../static/js/codeplug/fields.js";
import { openFile } from "../static/js/editor/files.js";
import { makeBundle } from "../static/js/flasher/backup.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";

const AREA = 0xC9000;
const put16 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = v >> 8;
};
const put24 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = (v >> 8) & 0xFF;
	b[o + 2] = v >> 16;
};
const ascii = (b, o, n, text) => {
	b.fill(0, o, o + n);
	for (let i = 0; i < text.length; i++) b[o + i] = text.charCodeAt(i);
};

// A data flash read: logical blocks placed on pages in a shuffled order, with a given-up page and the calibration
class Dm32 {
	constructor() {
		this.blocks = new Map();
		for (const id of [0x02, 0x04, 0x0F, 0x12, 0x42, 0x44, 0x5C, 0x67]) this.blocks.set(id, new Uint8Array(0x1000).fill(id === 0x02 ? 0x5A : 0));
		this.blocks.get(0x67).fill(0xFF, 1, 0x10);
	}
	blk(id) {
		return this.blocks.get(id);
	}
	channel(n, { name, rx, tx = rx, dmr = false, power = 2, wide = false, scan = 0, ts = 1, cc = 1, list = 0, contact = 0, rxTone = 0xFFFF, txTone = 0xFFFF }) {
		const b = this.blk(0x12), r = 0x10 + 0x30 * (n - 1);
		put16(b, 0, Math.max(b[0] | (b[1] << 8), n));
		ascii(b, r, 16, name);
		putFreq(b, r + 0x10, rx);
		putFreq(b, r + 0x14, tx);
		b[r + 0x18] = (dmr ? 0x10 : 0) | (power << 1);
		b[r + 0x19] = (wide ? 0x80 : 0) | (scan << 2);
		b[r + 0x1D] = (ts === 2 ? 0x10 : 0) | cc;
		b[r + 0x1F] = list;
		put16(b, r + 0x21, rxTone);
		put16(b, r + 0x23, txTone);
		const t = this.blk(0x42);
		t[2 * (n - 1)] = dmr ? 1 : 0;
		t[2 * (n - 1) + 1] = contact;
	}
	contact(n, { name, id, group = true }) {
		const b = this.blk(0x44), r = 0x18 * (n - 1);
		ascii(b, r + 2, 16, name);
		put24(b, r + 0x13, id);
		b[r + 0x16] = group ? 4 : 3;
	}
	list(n, name, ids) {
		const b = this.blk(0x0F), r = 0x10 + 0x6D * (n - 1);
		b[0] |= 1 << (n - 1);
		ascii(b, r + 1, 11, name);
		ids.forEach((id, k) => put24(b, r + 0x0C + 3 * k, id));
	}
	zone(n, name, chans) {
		const b = this.blk(0x5C), r = 0x10 + 0x91 * (n - 1);
		b[0] = Math.max(b[0], n);
		ascii(b, r, 16, name);
		b[r + 0x10] = chans.length;
		chans.forEach((c, k) => put16(b, r + 0x11 + 2 * k, c));
	}
	radioId(n, id, name) {
		const b = this.blk(0x67), r = 0x10 + 0x10 * (n - 1);
		b[0] = Math.max(b[0] === 0xFF ? 0 : b[0], n);
		put24(b, r, id);
		ascii(b, r + 3, 13, name);
	}
	boot(l1, l2) {
		ascii(this.blk(0x04), 1, 14, l1);
		ascii(this.blk(0x04), 0x0F, 14, l2);
	}
	// the read: pages from 0x1000, blocks in reverse order, a given-up copy of channels with stale bytes first
	read(size = AREA) {
		const out = new Uint8Array(size).fill(0xFF);
		const stale = new Uint8Array(0x1000);
		ascii(stale, 0x10, 16, "Old");
		stale[0xFFF] = 0x00;
		out.set(stale, 0x1000);
		let a = 0x2000;
		for (const [id, data] of [...this.blocks].reverse()) {
			out.set(data, a);
			out[a + 0xFFF] = id;
			a += 0x1000;
		}
		return out;
	}
}

function sample() {
	const s = new Dm32();
	s.boot("CALL", "Name");
	s.radioId(1, 3100123, "N0CALL-7");
	s.radioId(2, 3100124, "N0CALL-9");
	s.contact(1, { name: "Local", id: 2 });
	s.contact(2, { name: "TAC 311", id: 311 });
	// contact 3 unused: numbers past it still count
	s.contact(4, { name: "Friend", id: 3100999, group: false });
	s.list(1, "FAV", [2, 311, 4000]);
	s.list(2, "Empty", []);
	s.channel(1, { name: "Repeater", rx: 146940000, tx: 146340000, wide: true, rxTone: 0x1035, txTone: 0x1035, scan: 1 });
	s.channel(2, { name: "DMR TS2", rx: 444937500, tx: 449937500, dmr: true, ts: 2, cc: 3, list: 1, contact: 2, power: 1 });
	s.channel(3, { name: "DMR TS1", rx: 446075000, dmr: true, ts: 1, cc: 1, contact: 4, power: 0 });
	s.channel(4, { name: "GMRS", rx: 462675000, tx: 467675000, rxTone: 0x8114, txTone: 0x8054 });
	s.zone(1, "Main", [1, 2, 3]);
	s.zone(2, "Analog", [4, 1]);
	return s;
}

// --- Pages ---

test("pageMap: one page per block number; a page given up or erased holds none; a block on two pages is not this layout", () => {
	const b = sample().read();
	const m = pageMap(b);
	assert.deepEqual([...m.keys()].sort((x, y) => x - y), [0x02, 0x04, 0x0F, 0x12, 0x42, 0x44, 0x5C, 0x67]);
	assert.equal(m.get(0x12)[0x10], "R".charCodeAt(0));
	const dup = b.slice();
	dup[0x1FFF] = 0x12;
	assert.equal(pageMap(dup), null);
	assert.equal(pageMap(new Uint8Array(AREA).fill(0xFF)), null);
});

test("dm32Tone: none, CTCSS, DCS; a DCS digit above 7 is not a tone", () => {
	const t = (v) => dm32Tone(new Uint8Array([v & 0xFF, v >> 8]), 0);
	assert.deepEqual([t(0xFFFF), t(0), t(0x1035), t(0x0670), t(0x8114), t(0x8017), t(0x8054), t(0x9999)],
		["", "", "103.5", "67.0", "D276N", "D023N", null, null]);
});

// --- Records ---

test("readStock: channels, TX contacts, contacts, lists, zones, radio ID and boot text as the spec lays them out", () => {
	const s = readStock(sample().read());
	assert.deepEqual(s.general, { dmrId: 3100123, radioName: "N0CALL-7", bootLine1: "CALL", bootLine2: "Name" });
	assert.deepEqual(s.channels.map((c) => [c.name, c.mode, c.rxHz, c.txHz, c.power, c.bw, c.timeslot, c.colorCode, c.rxList, c.contact, c.rxTone, c.txTone, c.scanList]), [
		["Repeater", "fm", 146940000, 146340000, 9, 25, 1, 1, 0, 0, "103.5", "103.5", 1],
		["DMR TS2", "dmr", 444937500, 449937500, 6, 12.5, 2, 3, 1, 2, "", "", 0],
		["DMR TS1", "dmr", 446075000, 446075000, 3, 12.5, 1, 1, 0, 4, "", "", 0],
		["GMRS", "fm", 462675000, 467675000, 9, 12.5, 1, 1, 0, 0, "D276N", null, 0],
	]);
	assert.deepEqual(s.contacts.map((c) => [c.index, c.name, c.type, c.id]), [[1, "Local", "group", 2], [2, "TAC 311", "group", 311], [4, "Friend", "private", 3100999]]);
	assert.deepEqual(s.rxLists.map((l) => [l.index, l.name, l.ids]), [[1, "FAV", [2, 311, 4000]], [2, "Empty", []]]);
	assert.deepEqual(s.zones.map((z) => [z.name, z.a]), [["Main", [1, 2, 3]], ["Analog", [4, 1]]]);
});

test("readStock: text after a name's end is ignored, an unprintable name drops the record", () => {
	const f = sample();
	const b = f.blk(0x44);
	b.set([0x00, 0x73, 0x20, 0x31], 2 + "Local".length);	// old text after the end
	f.blk(0x12)[0x10 + 0x30 * 3] = 0x07;
	const s = readStock(f.read());
	assert.equal(s.contacts[0].name, "Local");
	assert.deepEqual(s.channels.map((c) => c.name), ["Repeater", "DMR TS2", "DMR TS1"]);
});

// --- Detection ---

test("detectStock and classify: a stock read, a whole data flash, blank, a RadioFoundry image", () => {
	const b = sample().read();
	assert.equal(detectStock(b), DM32_STOCK);
	assert.equal(classify(b).layout, DM32_STOCK);
	const full = new Uint8Array(0x1000000).fill(0xFF);
	full.set(b);
	assert.equal(classify(full).layout, DM32_STOCK);
	assert.equal(detectStock(new Uint8Array(AREA).fill(0xFF)), null);
	const f = new Dm32();	// no channels
	assert.equal(detectStock(f.read()), null);
	// mostly broken channels: not a codeplug
	const bad = sample();
	for (let n = 1; n <= 4; n++) bad.blk(0x12).fill(0, 0x10 + 0x30 * (n - 1) + 0x10, 0x10 + 0x30 * (n - 1) + 0x18);
	assert.equal(detectStock(bad.read()), null);
});

// --- Conversion ---

test("importStock: the editing model with references kept; list members by ID; DCS out of range noted", () => {
	const m = empty();
	const { counts, notes } = importStock(m, sample().read());
	// lists: FAV has members, Empty has none and no channel uses it. TG 4000 has no contact: one is made
	assert.deepEqual(counts, { channels: 4, contacts: 4, rxLists: 1, zones: 2 });
	assert.deepEqual(notes, [{ code: "scan_lists", n: 1 }, { code: "bad_tone", n: 1 }]);
	assert.deepEqual(m.contacts.map((c) => [c.name, c.id, c.type]), [["Local", 2, "group"], ["TAC 311", 311, "group"], ["Friend", 3100999, "private"], ["4000", 4000, "group"]]);
	assert.deepEqual(m.rxLists[0].contacts.map((c) => c.name), ["Local", "TAC 311", "4000"]);
	const [rep, ts2, ts1, gmrs] = m.channels;
	assert.deepEqual([rep.rxTone, rep.bw, rep.contact, rep.power], ["103.5", 25, null, 9]);
	assert.deepEqual([ts2.timeslot, ts2.colorCode, ts2.contact.name, ts2.rxList.name, ts2.power], [2, 3, "TAC 311", "FAV", 6]);
	assert.deepEqual([ts1.contact.name, ts1.rxList, ts1.power], ["Friend", null, 3]);
	assert.deepEqual([gmrs.rxTone, gmrs.txTone], ["D276N", ""]);
	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.map((c) => c.name)]), [["Main", ["Repeater", "DMR TS2", "DMR TS1"]], ["Analog", ["GMRS", "Repeater"]]]);
	assert.deepEqual([m.general.dmrId, m.general.radioName, m.general.bootLine1, m.general.bootLine2], [3100123, "N0CALL-7", "CALL", "Name"]);
});

test("importStock: a channel count past the first block is reported, the first block's channels kept", () => {
	const f = sample();
	put16(f.blk(0x12), 0, 100);
	for (let n = 5; n <= 84; n++) f.channel(n, { name: `Ch ${n}`, rx: 146000000 + n * 12500 });
	put16(f.blk(0x12), 0, 100);
	const m = empty();
	const { counts, notes } = importStock(m, f.read());
	assert.equal(counts.channels, 84);
	assert.deepEqual(notes.find((x) => x.code === "too_many_channels"), { code: "too_many_channels", n: 16 });
});

// --- Editor and Tools page ---

test("openFile: a data flash read and a stock backup convert; a backup taken on RadioFoundry doesn't", async () => {
	const b = sample().read();
	const f = await openFile("data-memory.bin", b);
	assert.deepEqual([f.kind, f.from], ["converted", DM32_STOCK]);
	assert.deepEqual(decode(f.image).channels.map((c) => c.name), ["Repeater", "DMR TS2", "DMR TS1", "GMRS"]);

	const meta = (kind) => ({ radio: { board: "dm32", name: "Baofeng DM-32UV" }, firmware_now: { kind } });
	for (const [file, what] of [["data-memory.bin", "c7000-data"], ["original-channels.bin", "c7000-data-start"]]) {
		const zipped = await makeBundle(meta("stock"), [{ file, what, addr: 0, data: b, complete: true }]);
		const z = await openFile("backup.zip", zipped);
		assert.deepEqual([z.kind, z.from, z.board], ["converted", DM32_STOCK, "dm32"]);
	}
	const rf = await makeBundle(meta("rfoundry"), [{ file: "data-memory.bin", what: "c7000-data", addr: 0, data: b, complete: true }]);
	await assert.rejects(openFile("backup.zip", rf), (e) => e.code === "backup_no_codeplug");
});

test("Tools page: a data flash read converts to a RadioFoundry codeplug image and to qdmr YAML; so does a stock backup", async () => {
	const b = sample().read();
	assert.equal(detect(b, "data-memory.bin").id, "dm32-stock");
	const full = new Uint8Array(0x1000000).fill(0xFF);
	full.set(b);
	assert.equal(detect(full, "data-memory.bin").id, "dm32-stock");
	const [toImg, toYaml] = convertersFor("dm32-stock");
	const img = await toImg.convert(b, "data-memory.bin");
	assert.equal(img.name, "data-memory-rfoundry.bin");
	assert.equal(classify(img.data).layout, "openfw");
	assert.equal(decode(CodeplugImage.from(img.data)).channels[1].contact.name, "TAC 311");
	assert.match(new TextDecoder().decode((await toYaml.convert(b, "x.bin")).data), /name: DMR TS2/);
	await assert.rejects(toImg.convert(new Uint8Array(AREA).fill(0xFF), "x.bin"), /no stock codeplug/);

	const zipped = await makeBundle({ radio: { board: "dm32", name: "Baofeng DM-32UV" }, firmware_now: { kind: "stock" } },
		[{ file: "original-channels.bin", what: "c7000-data-start", addr: 0, data: b, complete: true }]);
	const fromBackup = await convertersFor("rfoundry-backup")[0].convert(zipped);
	assert.equal(fromBackup.name, "codeplug-dm32.bin");
	assert.equal(classify(fromBackup.data).layout, "openfw");
});
