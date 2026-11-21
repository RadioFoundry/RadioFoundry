// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning: the page's arithmetic (tune-math.js) and its debug link payloads (tune-radio.js) against the firmware's
// layouts (docs/spec/debuglink.md 3.31, tests/host/powerTuneTest.c).

import assert from "node:assert/strict";
import test from "node:test";

import { bandInfo, db, grade, mhzText, neighborSlope, offsetAt, parseMhz, parseReading, parseWatts, snapKhz,
	suggestDrive, wattsName } from "../static/js/tune-math.js";
import { TuneRadio, cleanName, encodeLabel, encodePoint, parseInfo, parseLimits, parseRecord } from "../static/js/tune-radio.js";

const le32 = (n) => [n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, n >>> 24];
const le16 = (n) => [n & 0xFF, (n >> 8) & 0xFF];
const named = (s) => [s.length, ...new TextEncoder().encode(s)];

test("band names and default tuning frequencies follow the band", () => {
	assert.deepEqual(bandInfo(136000, 174000), { name: "2 m / VHF", ham: "2 m", khz: 146000 });
	assert.deepEqual(bandInfo(400000, 480000), { name: "70 cm / UHF", ham: "70 cm", khz: 445000 });
	assert.deepEqual(bandInfo(902000, 928000), { name: "33 cm / 900", ham: "33 cm", khz: 915000 });
	assert.equal(bandInfo(219000, 225000).name, "1.25 m / 220");
	// No amateur band inside: the region and the middle on a 12.5 kHz step
	const b = bandInfo(450000, 470000);
	assert.equal(b.name, "UHF");
	assert.equal(b.ham, null);
	assert.equal(b.khz, 460000);
	assert.equal(snapKhz(146006), 146000);
	assert.equal(snapKhz(446010), 446012.5);
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

test("the offset across a band: linear between points, flat past them (the radio's rule)", () => {
	const pts = [{ khz: 140000, delta: -20 }, { khz: 170000, delta: 40 }];
	assert.equal(offsetAt(pts, 130000), -20);
	assert.equal(offsetAt(pts, 155000), 10);
	assert.equal(offsetAt(pts, 174000), 40);
	assert.equal(offsetAt([], 146000), 0);
});

test("INFO, LIMITS and READ replies decode as the firmware lays them out", () => {
	const info = new Uint8Array([1, 0b101, 3, ...le32(7), 20, 8, 6, 2, 2,
		...le32(136000), ...le32(174000), ...le32(400000), ...le32(480000),
		...named("1W"), ...named("5W"), ...named("1W"), ...named("4W")]);
	const i = parseInfo(info);
	assert.deepEqual([i.version, i.store, i.floor, i.preview, i.copies, i.seq], [1, true, false, true, 3, 7]);
	assert.deepEqual([i.maxPoints, i.maxLabels, i.nameLen, i.levels], [20, 8, 6, 2]);
	assert.deepEqual(i.bands, [{ lo: 136000, hi: 174000, names: ["1W", "5W"] }, { lo: 400000, hi: 480000, names: ["1W", "4W"] }]);

	const lim = new Uint8Array([2, ...le16(1200), ...le16(1210), ...le16(1200), ...le16(2600),
		...le16(2600), ...le16(2580), ...le16(1200), ...le16(2600), ...named("1W"), ...named("4.5W")]);
	assert.deepEqual(parseLimits(lim), [{ factory: 1200, tuned: 1210, lo: 1200, hi: 2600, name: "1W" },
		{ factory: 2600, tuned: 2580, lo: 1200, hi: 2600, name: "4.5W" }]);

	const rec = new Uint8Array(20 + 16 + 20);
	rec.set([...le32(0x54504652), ...le32(9), 1, 1, 1, 2]);
	rec.set([...le32(146000), 1, 0, ...le16(2600), ...le16(-20 & 0xFFFF), 0, 0, ...le32(4500)], 20);
	rec.set([...le32(146000), 1, 1, ...le16(2600), ...le32(4500), ...new TextEncoder().encode("4.5W")], 36);
	const r = parseRecord(rec);
	assert.equal(r.seq, 9);
	assert.deepEqual(r.points, [{ khz: 146000, level: 1, state: "applied", factory: 2600, delta: -20, mw: 4500 }]);
	assert.deepEqual(r.labels, [{ khz: 146000, level: 1, state: "stale", factory: 2600, mw: 4500, name: "4.5W" }]);
	assert.equal(parseRecord(new Uint8Array(0)), null);
});

test("edits encode as the firmware reads them", () => {
	// 446000 kHz = 30 CE 06 00 (powerTuneTest.c)
	assert.deepEqual([...encodePoint(446000, 2, -5, 1500)], [4, 0x30, 0xCE, 0x06, 0x00, 2, 0xFB, 0xFF, ...le32(1500)]);
	assert.deepEqual([...encodeLabel(146000, 1, "4W", 4000)], [6, ...le32(146000), 1, ...le32(4000), 0x34, 0x57]);
	assert.equal(encodeLabel(146000, 1, "", 0).length, 10);
	assert.equal(cleanName("  Hiégh power "), "High p");
	assert.equal(cleanName("5W\n"), "5W");
});

// A radio on the debug link for the test transmission: the core view, the RADIO ops, PTT
class FakeLink {
	constructor() {
		this.sent = [];
		this.v = { vfo: "0", rx_hz: "461462500", mode: "dmr", power: "0", state: "rx", tx_deny: "none" };
		this.ch = { "ch.tx_tone": "ctcss:885", "ch.rx_tone": "none:0" };
	}
	async request(cmd, p) {
		this.sent.push([cmd, ...p]);
		const kv = (o) => new Uint8Array([0, 0, 0, 0, 0, ...new TextEncoder().encode(Object.entries(o).map(([k, v]) => `${k}=${v}`).join("\n"))]);
		if (cmd === 0x09) return kv(p[0] === 1 ? this.ch : this.v);
		if (cmd === 0x0A) {
			const op = p[0];
			if (op === 4) this.v.vfo = String(p[1]);
			if (op === 5) this.v.rx_hz = String(p[1] | p[2] << 8 | p[3] << 16 | (p[4] << 24 >>> 0));
			if (op === 31 && p[1] === 0) this.v.mode = "fm";
			if (op === 7) this.v.power = String(+this.v.power + (p[1] === 1 ? 1 : -1));
			return new Uint8Array(0);
		}
		if (cmd === 0x07) { this.v.state = p[0] ? "tx" : "rx"; return new Uint8Array([0, 0]); }
		return new Uint8Array(0);
	}
}

test("a test transmission sets the VFO, FM, no tone, the TX limit, the level, a preview; then clears both", async () => {
	const link = new FakeLink();
	const radio = new TuneRadio(link);
	const ticks = [];
	await radio.key(146000, 2, 15, 50, (left) => ticks.push(left));
	assert.equal(link.v.vfo, "1");
	assert.equal(link.v.mode, "fm");
	assert.equal(link.v.rx_hz, "146000000");
	assert.equal(link.v.power, "2");
	const ops = link.sent.filter((s) => s[0] === 0x0A).map((s) => s[1]);
	assert.ok(ops.includes(10), "tones off");
	const limits = link.sent.filter((s) => s[0] === 0x0A && s[1] === 35);
	assert.deepEqual(limits[0], [0x0A, 35, 0, 1, ...le32(146000000)]);
	assert.deepEqual(limits[limits.length - 1], [0x0A, 35, 0, 0], "the TX limit is off after the transmission");
	const previews = link.sent.filter((s) => s[0] === 0x27);
	assert.deepEqual(previews[0], [0x27, 3, 2, 15, 0]);
	assert.deepEqual(previews[previews.length - 1], [0x27, 3, 0xFF, 0, 0], "the preview is off after the transmission");
	const ptt = link.sent.filter((s) => s[0] === 0x07);
	assert.equal(ptt[0][1], 1);
	assert.equal(ptt[ptt.length - 1][1], 0);
	assert.equal(ticks[ticks.length - 1], 0);

	// Restore: the VFO's tones, mode and frequency, then back to the channel
	await radio.restore();
	assert.equal(link.v.vfo, "0");
	assert.equal(link.v.rx_hz, "461462500");
	assert.ok(link.sent.some((s) => s[0] === 0x0A && s[1] === 10 && s[2] === 1 && s[3] === 1 && s[4] === (885 & 0xFF)), "TX tone back");
});

test("a radio that doesn't transmit is reported with its reason", async () => {
	const link = new FakeLink();
	link.request = (orig => async (cmd, p) => {
		if (cmd === 0x07) { link.v.state = "rx"; link.v.tx_deny = p[0] ? "band" : "none"; return new Uint8Array([0, 0]); }
		return orig.call(link, cmd, p);
	})(link.request);
	const radio = new TuneRadio(link);
	await assert.rejects(radio.key(146000, 0, 0, 20), (e) => e.tuneStep === "tx" && e.deny === "band");
});
