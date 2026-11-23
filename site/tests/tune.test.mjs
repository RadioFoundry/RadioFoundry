// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning: the page's arithmetic (tune-math.js), the record and the debug link payloads (tune-radio.js) against
// the firmware's layouts (docs/spec/radio-tune.md, tests/host/radioTuneTest.c).

import assert from "node:assert/strict";
import test from "node:test";

import { bandInfo, db, grade, mhzText, neighborSlope, parseMhz, parseReading, parseWatts, snapKhz, suggestDrive,
	trimHz, wattsName } from "../static/js/tune-math.js";
import { BATT, KIND, MODE, RECORD_MAX, TuneRadio, battPct, battRange, battScale, cleanName, crc32, encodeRecord, gridKhz,
	inForce, owner, parseCore, parseRecord, toWords, valueAt, word } from "../static/js/tune-radio.js";

const le32 = (n) => [n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, (n >>> 24) & 0xFF];
const le16 = (n) => [n & 0xFF, (n >> 8) & 0xFF];
const hex = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("");

test("band names and default tuning frequencies follow the band", () => {
	assert.deepEqual(bandInfo(136000, 174000), { name: "2 m / VHF", ham: "2 m", khz: 146000 });
	assert.deepEqual(bandInfo(400000, 480000), { name: "70 cm / UHF", ham: "70 cm", khz: 445000 });
	assert.deepEqual(bandInfo(902000, 928000), { name: "33 cm / 900", ham: "33 cm", khz: 915000 });
	assert.equal(bandInfo(219000, 225000).name, "1.25 m / 220");
	// No amateur band inside: the region and the middle on the 100 kHz grid
	const b = bandInfo(450000, 470000);
	assert.equal(b.name, "UHF");
	assert.equal(b.ham, null);
	assert.equal(b.khz, 460000);
	assert.equal(snapKhz(146006), 146000);
	assert.equal(snapKhz(446060), 446100);
	assert.equal(gridKhz(446049), 446000);
});

test("frequencies and watts as people type them", () => {
	assert.equal(mhzText(146000), "146.000");
	assert.equal(mhzText(446012.5), "446.0125");
	assert.equal(parseMhz("146.52"), 146520);
	assert.equal(parseMhz("446,0125"), 446013);
	assert.equal(parseMhz("abc"), null);
	assert.equal(parseWatts("5W"), 5);
	assert.equal(parseWatts("2.5W"), 2.5);
	assert.equal(parseWatts("500mW"), 0.5);
	assert.equal(parseWatts("Low"), null);
	assert.equal(parseReading("4.8"), 4.8);
	assert.equal(parseReading("500 mW"), 0.5);
	assert.equal(parseReading("0,5"), 0.5);
	assert.equal(parseReading(""), null);
	assert.equal(parseReading("-1"), null);
	assert.equal(wattsName(0.05), "50mW");
	assert.equal(wattsName(2.5), "2.5W");
	assert.equal(wattsName(25), "25W");
	// The oscillator trim in Hz at a frequency: 1 ppm is 446 Hz at 446 MHz
	assert.equal(trimHz(1000, 446000), 446);
	assert.equal(trimHz(-2500, 146000), -365);
});

test("the reading's grade: 0.5 dB on target, 1 dB close (2 dB under 1 W)", () => {
	assert.ok(Math.abs(db(10, 5) - 3.0103) < 1e-3);
	assert.equal(grade(5.2, 5), "good");
	assert.equal(grade(5.9, 5), "near");
	assert.equal(grade(7, 5), "off");
	assert.equal(grade(0.7, 0.5), "near");	// 1.46 dB under 1 W
	assert.equal(grade(0, 5), null);
	assert.equal(grade(5, null), null);
});

test("suggestions: secant on ln(P), the neighbors' slope, P ∝ drive²; inside the limits", () => {
	const lim = { lo: 100, hi: 3000 };
	// P = drive² / 1e6 * k: two readings give the exact drive for 4 W on a log-linear stretch
	const two = suggestDrive([{ drive: 1000, watts: 2 }, { drive: 1400, watts: 4.5 }], 4, lim);
	assert.ok(two > 1300 && two < 1400, `two readings: ${two}`);
	// One reading, no neighbors: P ∝ D², 2 W at 1000 -> 4 W at 1414
	assert.equal(suggestDrive([{ drive: 1000, watts: 2 }], 4, lim), 1414);
	// One reading with the neighbors' slope
	const slope = neighborSlope([{ factory: 800, watts: 1 }, { factory: 1200, watts: 2.5 }, { factory: 2600, watts: 5 }], 1);
	assert.ok(slope > 0);
	const one = suggestDrive([{ drive: 1200, watts: 2 }], 2.5, { ...lim, slope });
	assert.ok(one > 1200 && one < 1400, `one reading: ${one}`);
	// Clamped
	assert.equal(suggestDrive([{ drive: 2900, watts: 4 }], 8, lim), 3000);
	assert.equal(suggestDrive([{ drive: 200, watts: 1 }], 0.01, lim), 100);
	assert.equal(suggestDrive([], 4, lim), null);
	assert.equal(suggestDrive([{ drive: 1000, watts: 2 }], null, lim), null);
	assert.equal(neighborSlope([{ factory: 800, watts: null }, { factory: 900, watts: 1 }], 0), null);
});

test("the CRC and the owner are the radio's (storage_crc32, zlib's CRC-32)", () => {
	assert.equal(crc32(new TextEncoder().encode("123456789")), 0xCBF43926);
	const a = new TextEncoder().encode("1234"), b = new TextEncoder().encode("56789");
	assert.equal(crc32(b, crc32(a)), 0xCBF43926, "chains");
	// board.name, then the chip UID as INFO_CHIP gives it: uid[0..2] little endian (radioTuneTest.c: 0x1234, 7, 9)
	assert.equal(owner("board-a", "341200000700000009000000"), 0x8FC02E97);
	assert.equal(owner("board-a", ""), crc32(new TextEncoder().encode("board-a")));
	assert.notEqual(owner("board-a", "999900000700000009000000"), owner("board-a", "341200000700000009000000"));
});

test("a record encodes as the radio reads it, and parses back", () => {
	const own = 0x8FC02E97;
	const rec = encodeRecord(1, own, [{ kind: KIND.DRIVE, sel: 2, khz: 446000, value: -5, factory: 1500 }]);
	// magic, seq 1, version 1, 1 entry, reserved, owner; drive level 2, 446000/25 = 0x45B0, -5, factory 1500; CRC; commit
	assert.equal(hex(rec), "5246544e0100000001010000972ec08f" + "0200b045fbffdc05" + "8e3495d6" + "00000000");
	const all = [
		{ kind: KIND.DRIVE, sel: 4, khz: 146000, value: 120, factory: 2600 },
		{ kind: KIND.DEV, sel: MODE.FM, khz: 446000, value: -35, factory: 0 },
		{ kind: KIND.FREQ, sel: MODE.DMR, khz: 446000, value: -812, factory: 0 },
		{ kind: KIND.NAME, sel: 1, band: 1, name: "4W" },
	];
	const r = parseRecord(encodeRecord(77, own, all), own);
	assert.equal(r.seq, 77);
	assert.deepEqual(r.entries, all);
	// Another radio's, cut short, a flipped bit, sequence 0: not a record
	assert.equal(parseRecord(encodeRecord(77, own, all), own + 1), null);
	const cut = encodeRecord(77, own, all);
	cut.fill(0xFF, cut.length - 4);
	assert.equal(parseRecord(cut, own), null, "no commit word");
	const flip = encodeRecord(77, own, all);
	flip[20] ^= 1;
	assert.equal(parseRecord(flip, own), null);
	assert.equal(parseRecord(encodeRecord(0, own, all), own), null);
	assert.ok(encodeRecord(1, own, []).length <= RECORD_MAX);
	assert.equal(cleanName("  Hiégh power "), "High p");
	assert.equal(cleanName("5W\n"), "5W");
});

test("the copy in force: the newer one, the sequence compared across its wrap", () => {
	assert.equal(inForce({ seq: 5 }, { seq: 6 }), 1);
	assert.equal(inForce({ seq: 6 }, { seq: 5 }), 0);
	assert.equal(inForce({ seq: 0xFFFFFFFF }, { seq: 1 }), 1, "wrap");
	assert.equal(inForce(null, { seq: 3 }), 1);
	assert.equal(inForce({ seq: 3 }, null), 0);
	assert.equal(inForce(null, null), -1);
});

test("RAM words are RADIO_TUNE_WORD's; stale drive trims and names stay out", () => {
	// RADIO_TUNE_WORD(146000, 2, 1, -5): 1460 | 2 << 14 | 1 << 16 | -5 << 19
	assert.equal(word(146000, KIND.FREQ, 1, -5), 0xFFD985B4);
	assert.equal(word(446000, KIND.DRIVE, 3, 4095), (4460 | 3 << 16 | 4095 << 19) >>> 0);
	const es = [
		{ kind: KIND.DRIVE, sel: 0, khz: 146000, value: 10, factory: 400 },
		{ kind: KIND.DRIVE, sel: 1, khz: 146000, value: 10, factory: 800 },
		{ kind: KIND.DEV, sel: 0, khz: 146000, value: 30, factory: 0 },
		{ kind: KIND.NAME, sel: 0, band: 0, name: "1W" },
		{ kind: KIND.FREQ, sel: 0, khz: 146000, value: 5000, factory: 0 },
	];
	const factory = (khz, level) => (level === 0 ? 400 : 810);	// level 1's factory drive moved: its trim is stale
	assert.deepEqual(toWords(es, factory), [word(146000, 0, 0, 10), word(146000, 1, 0, 30)]);
});

test("battery trims: record entries with frequency 0, RAM words of kind 3", () => {
	const es = [
		{ kind: KIND.BATT, sel: BATT.GAIN, khz: 0, value: 21, factory: 0 },
		{ kind: KIND.BATT, sel: BATT.FULL, khz: 0, value: -150, factory: 0 },
		{ kind: KIND.BATT, sel: 5, khz: 0, value: 10, factory: 0 },		// no such battery trim
	];
	// RADIO_TUNE_WORD(0, RADIO_TUNE_W_BATT, RADIO_TUNE_BATT_GAIN, 21): 3 << 14 | 21 << 19
	assert.deepEqual(toWords(es), [0x00A8C000, (3 << 14 | 2 << 16 | -150 << 19) >>> 0]);
	const rec = encodeRecord(2, 0x1234, es.slice(0, 2));
	assert.equal(hex(rec.subarray(16, 24)), "400000001500" + "0000");
	assert.deepEqual(parseRecord(rec, 0x1234).entries, es.slice(0, 2));
});

test("the battery charge as the radio works it out (batt_pct, batt_scale)", () => {
	// The same points as radioFeaturesTest.c's test_battery_module
	assert.deepEqual([6300, 6400, 6700, 7400, 8100, 8200, 9000].map((mv) => battPct(mv, 6400, 8200)), [0, 0, 3, 42, 94, 100, 100]);
	const r = battRange(6400, 8200, 400, 0);
	assert.equal(battPct(7200, r.empty, r.full), 7);
	assert.deepEqual(battRange(6400, 8200, 0, -200), { empty: 6400, full: 8000, ok: true });
	assert.deepEqual(battRange(6400, 8200, 1000, -900), { empty: 6400, full: 8200, ok: false }, "too close: the board's own");
	assert.deepEqual([battScale(8000, 25), battScale(8000, -50), battScale(7999, 1), battScale(7999, -1)], [8200, 7600, 8006, 7992]);
});

test("a trim across a band: linear between trims, flat past them, the band's own only", () => {
	const es = [
		{ kind: KIND.DRIVE, sel: 1, khz: 140000, value: -20 },
		{ kind: KIND.DRIVE, sel: 1, khz: 170000, value: 40 },
		{ kind: KIND.DRIVE, sel: 1, khz: 446000, value: 99 },
		{ kind: KIND.DRIVE, sel: 2, khz: 146000, value: 7 },
	];
	assert.equal(valueAt(es, KIND.DRIVE, 1, 130000, 136000, 174000), -20);
	assert.equal(valueAt(es, KIND.DRIVE, 1, 155000, 136000, 174000), 10);
	assert.equal(valueAt(es, KIND.DRIVE, 1, 174000, 136000, 174000), 40);
	assert.equal(valueAt(es, KIND.DRIVE, 1, 440000, 400000, 480000), 99);
	assert.equal(valueAt(es, KIND.DEV, 1, 146000, 136000, 174000), 0);
});

test("the core view's tuning facts", () => {
	const c = parseCore({ tune: "3/20/1/6/1/3/1", bands: "136000000-174000000,400000000-480000000,174000000-240000000",
		power_count: "3", power: "1", power_names: "1W/2.5W/5W", tx_drive: "400/900/2600", rx_hz: "146000000",
		tx_hz: "146000000", mode: "fm", vfo: "1", state: "rx", tx_deny: "none" });
	assert.equal(c.built, true);
	assert.deepEqual([c.words, c.maxWords, c.names, c.maxNames, c.devModes, c.liveDrive], [3, 20, 1, 6, 1, true]);
	assert.deepEqual(c.bands.map((b) => [b.index, b.lo, b.hi, b.tx]),
		[[0, 136000, 174000, true], [1, 400000, 480000, true], [2, 174000, 240000, false]]);
	assert.deepEqual(c.levelNames, ["1W", "2.5W", "5W"]);
	assert.deepEqual(c.drives, [400, 900, 2600]);
	assert.equal(parseCore({}).built, false, "a firmware without tuning");
	assert.equal(parseCore({ tune: "0/20/0/6/0/1" }).liveDrive, false, "no live field: re-key");
	assert.deepEqual(parseCore({ tune: "0/20/0/6/0/1/0/7950/6400/8200" }).battery, { raw: 7950, empty: 6400, full: 8200 });
	assert.equal(parseCore({ tune: "0/20/0/6/0/1/0/0/0/0" }).battery, null, "no battery");
	assert.equal(c.battery, null, "a firmware without the battery fields");
});

test("the battery: the reading before the gain from the core view, the radio's voltage and charge", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	assert.equal(await radio.battery(), null, "no battery fields");
	link.v.tune = "0/20/0/6/1/3/0/7950/6400/8200";
	assert.deepEqual(await radio.battery(), { raw: 7950, empty: 6400, full: 8200, mv: 8120, pct: 95, level: "ok" });
	assert.ok(link.sent.some((s) => s[0] === 0x09 && s[1] === 7), "the features view");
});

// A radio on the debug link: the core and channel views, the RADIO ops, PTT, and STORAGE on a two-copy tune area
const AREA = 0x1C000, UNIT = 0x1000;
class FakeLink {
	constructor() {
		this.sent = [];
		this.v = { vfo: "0", rx_hz: "461462500", tx_hz: "461462500", mode: "dmr", power: "0", state: "rx",
			tx_deny: "none", tune: "0/20/0/6/1/3/0", power_count: "3", tx_drive: "400/900/2600" };
		this.ch = { "ch.tx_tone": "ctcss:885", "ch.rx_tone": "none:0" };
		this.s9 = { ref_trim: "-" };
		this.f = { battery: "ok", battery_mv: "8120", battery_pct: "95" };
		this.copies = [new Uint8Array(UNIT).fill(0xFF), new Uint8Array(UNIT).fill(0xFF)];
		this.writes = [];
		this.words = [];
		this.busyOnce = false;
		this.foreign = false;	// the radio's check (TUNE_WRITE at offset 0) finds other data in the tune area
	}
	async request(cmd, p) {
		this.sent.push([cmd, ...p]);
		const kv = (o) => new Uint8Array([0, 0, 0, 0, 0, ...new TextEncoder().encode(Object.entries(o).map(([k, v]) => `${k}=${v}`).join("\n"))]);
		const u32 = (o) => (p[o] | p[o + 1] << 8 | p[o + 2] << 16 | p[o + 3] << 24) >>> 0;
		if (cmd === 0x09) return kv(p[0] === 1 ? this.ch : p[0] === 9 ? this.s9 : p[0] === 7 ? this.f : this.v);
		if (cmd === 0x0A) {
			if (this.busyOnce) {
				this.busyOnce = false;
				throw Object.assign(new Error("busy"), { status: 3 });
			}
			const op = p[0];
			if (op === 4) this.v.vfo = String(p[1]);
			if (op === 5) this.v.rx_hz = this.v.tx_hz = String(u32(1));
			if (op === 31) this.v.mode = ["fm", "dmr"][p[1]];
			if (op === 7) this.v.power = String(+this.v.power + (p[1] === 1 ? 1 : -1));
			if (op === 44) {
				const [first, total] = [p[1], p[2]];
				this.words.length = Math.min(this.words.length, first);
				for (let i = 0; 3 + i * 4 < p.length; i++) this.words[first + i] = u32(3 + i * 4);
				this.words.length = total;
				this.v.tune = `${total}/20/0/6/1/3/0`;
			}
			return new Uint8Array(0);
		}
		if (cmd === 0x07) { this.v.state = p[0] ? "tx" : "rx"; return new Uint8Array([0, 0]); }
		if (cmd === 0x0F) {
			if (p[0] === 0) {
				const r = new Uint8Array(34);
				r.set(le32(AREA), 26);
				r.set(le32(UNIT), 30);
				return r;
			}
			if (p[0] === 1) {
				const addr = u32(1), n = p[5] | p[6] << 8, copy = addr >= AREA + UNIT ? 1 : 0;
				const off = addr - AREA - copy * UNIT;
				return new Uint8Array([...le32(addr), ...this.copies[copy].subarray(off, off + n)]);
			}
			if (p[0] === 3) {
				const copy = p[1], off = p[2] | p[3] << 8, data = p.slice(4);
				if (off === 0 && this.foreign) throw Object.assign(new Error("refused"), { status: 7, data: new Uint8Array([2]) });
				this.writes.push([copy, off, data.length]);
				if (off === 0) this.copies[copy].fill(0xFF);
				this.copies[copy].set(data, off);
				return new Uint8Array(0);
			}
			if (p[0] === 4) return new Uint8Array([3]);
		}
		return new Uint8Array(0);
	}
}

test("a save writes the copy not in force first, the commit word last, reads each back, then loads", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	const own = owner("board-a", "341200000700000009000000");
	const area = await radio.area();
	assert.deepEqual(area, { addr: AREA, unit: UNIT });
	link.copies[0].set(encodeRecord(5, own, []));	// copy A in force
	let c = await radio.copies(area, own);
	assert.deepEqual([c.at, c.next], [0, 6]);
	const entries = Array.from({ length: 10 }, (_, i) => ({ kind: KIND.DRIVE, sel: i % 5, khz: 146000 + i * 100, value: i, factory: 400 }));
	const rec = encodeRecord(c.next, own, entries);
	assert.equal(await radio.save(area, c.at, rec), 3);
	assert.deepEqual(link.sent[link.sent.length - 1], [0x0F, 4], "TUNE_LOAD last");
	assert.equal(link.writes[0][0], 1, "copy B first");
	const b = link.writes.filter((w) => w[0] === 1);
	assert.deepEqual(b.map((w) => w[1]), [0, 60, rec.length - 4], "60-byte chunks, then the commit word");
	assert.equal(b[b.length - 1][2], 4);
	c = await radio.copies(area, own);
	assert.equal(c.record.seq, 6, "both copies at 6: either is the record");
	assert.deepEqual(c.record.entries, entries);
	assert.deepEqual(link.copies[0].subarray(0, rec.length), rec, "both copies hold the record");

	// A copy that reads back wrong stops the save before the other copy is touched
	const bad = new FakeLink();
	const orig = bad.request.bind(bad);
	bad.request = async (cmd, p) => {
		const r = await orig(cmd, p);
		if (cmd === 0x0F && p[0] === 1 && r.length > 20) r[20] ^= 1;
		return r;
	};
	await assert.rejects(new TuneRadio(bad).save(area, 0, rec), (e) => e.tuneStep === "verify");
	assert.ok(bad.writes.every((w) => w[0] === 1), "copy A untouched");

	// Other data in the tune area: the radio refuses the first write, nothing is written and nothing loads
	const other = new FakeLink();
	other.foreign = true;
	await assert.rejects(new TuneRadio(other).save(area, -1, rec), (e) => e.tuneStep === "foreign");
	assert.deepEqual(other.writes, []);
	assert.ok(!other.sent.some((s) => s[0] === 0x0F && s[1] === 4), "no TUNE_LOAD");
});

test("live words go in chunks of 15; each request's total is what was written so far", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	const words = Array.from({ length: 20 }, (_, i) => word(146000 + i * 100, KIND.DRIVE, 1, i));
	link.busyOnce = true;	// the request rate limit: tried again
	await radio.setWords(words);
	const sets = link.sent.filter((s) => s[0] === 0x0A && s[1] === 44);
	assert.deepEqual(sets.map((s) => [s[2], s[3], (s.length - 4) / 4]), [[0, 15, 15], [0, 15, 15], [15, 20, 5]]);
	assert.deepEqual(link.words, words);
	await radio.setWords([]);
	assert.deepEqual(link.words, []);
});

test("a test transmission: the VFO on the frequency, the mode, no tone, the TX limit, the level, PTT with a time-out", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	const c = await radio.setup(146000, MODE.FM);
	assert.deepEqual([link.v.vfo, link.v.mode, link.v.rx_hz, link.v.tx_hz], ["1", "fm", "146000000", "146000000"]);
	assert.deepEqual(c.drives, [400, 900, 2600]);
	assert.ok(link.sent.some((s) => s[0] === 0x0A && s[1] === 10 && s[2] === 3), "tones off");
	assert.ok(link.sent.some((s) => s[0] === 0x0A && s[1] === 35 && s[3] === 1 && s[4] === (146000000 & 0xFF)), "TX limit");
	await radio.setLevel(2);
	assert.equal(link.v.power, "2");
	await radio.keyUp(4000);
	assert.deepEqual(link.sent[link.sent.length - 1], [0x07, 1, ...le16(4000)]);
	await radio.keyUp(60000);
	assert.deepEqual(link.sent[link.sent.length - 1], [0x07, 1, ...le16(10000)], "the radio's own time-out stays at 10 s");
	await radio.txDrive(1234);
	assert.deepEqual(link.sent[link.sent.length - 1], [0x0A, 36, ...le16(1234)]);

	// DMR for the DMR carrier: the mode changes, no tone command
	const n = link.sent.length;
	await radio.setup(446000, MODE.DMR);
	assert.equal(link.v.mode, "dmr");
	assert.ok(!link.sent.slice(n).some((s) => s[0] === 0x0A && s[1] === 10));

	// Restore: no PTT, no live drive, no TX limit, the VFO's tones, mode and frequency, then back to the channel
	await radio.restore();
	assert.deepEqual([link.v.state, link.v.vfo, link.v.rx_hz, link.v.mode], ["rx", "0", "461462500", "dmr"]);
	assert.ok(link.sent.some((s) => s[0] === 0x0A && s[1] === 36 && s[2] === 0 && s[3] === 0), "live drive off");
	assert.ok(link.sent.some((s) => s[0] === 0x0A && s[1] === 10 && s[2] === 1 && s[3] === 1 && s[4] === (885 & 0xFF)), "TX tone back");
});

test("the oscillator: SETTING 35 in 100 ppb steps, none on a radio without the hook", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	assert.equal(await radio.refTrim(), null);
	link.s9.ref_trim = "-1200";
	assert.equal(await radio.refTrim(), -1200);
	assert.equal(await radio.setRefTrim(-1234), -1200);
	assert.deepEqual(link.sent[link.sent.length - 1], [0x0A, 24, 35, ...le16(-1200 & 0xFFFF)]);
	assert.equal(await radio.setRefTrim(50000), 10000);
});
