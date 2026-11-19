// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CHIRP CSV in the codeplug editor (static/js/codeplug/chirp.js): the mapping to the model, what is left out or can't
// be read, and a round trip through the export; and the CHIRP image reader (chirp_img.js). The samples are made up.

import { test } from "node:test";
import assert from "node:assert/strict";

import { empty } from "../static/js/codeplug/model.js";
import { importChirp, exportChirp, isChirp, ChirpError } from "../static/js/codeplug/chirp.js";
import { importChirpImg, isChirpImg } from "../static/js/codeplug/chirp_img.js";

const HEAD = "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE";
const SAMPLE = `${HEAD}
0,Simplex,146.520000,,0.600000,,88.5,88.5,023,NN,023,Tone->Tone,FM,5.00,,5.0W,,,,,
1,Rptr Up,147.060000,+,0.600000,Tone,100.0,88.5,023,NN,023,Tone->Tone,FM,5.00,,50W,,,,,
2,Rptr Dn,449.000000,-,5.000000,TSQL,88.5,123.0,023,NN,023,Tone->Tone,NFM,12.50,S,Low,,,,,
3,Split,146.000000,split,147.500000,,88.5,88.5,023,NN,023,Tone->Tone,FM,5.00,,,,,,,
4,Digital,146.940000,,0.600000,,88.5,88.5,023,NN,023,Tone->Tone,DV,5.00,,,,,,,
5,Dcs,146.700000,,0.600000,DTCS,88.5,88.5,047,NR,023,Tone->Tone,FM,5.00,,,,,,,
6,Cross,146.710000,,0.600000,Cross,100.0,88.5,023,NN,023,Tone->DTCS,FM,5.00,,,,,,,
`;

test("a CHIRP header is recognized and others are not", () => {
	assert.ok(isChirp(SAMPLE));
	assert.ok(!isChirp("Name,Frequency\nx,1\n"));
	assert.throws(() => importChirp(empty(), "a,b\n1,2\n"), ChirpError);
});

test("import maps frequencies, offsets, tones, bandwidth and skips, and counts what it leaves out", () => {
	const m = empty();
	const { counts, notes } = importChirp(m, SAMPLE, "Local");
	assert.equal(counts.channels, 6);
	assert.deepEqual(notes, [{ code: "mode", n: 1 }]);
	const [a, b, c, d, e, f] = m.channels;
	assert.deepEqual([a.number, a.name, a.rxHz, a.txHz, a.bw, a.rxTone, a.txTone], [1, "Simplex", 146520000, 146520000, 25, "", ""]);
	assert.equal(b.txHz, 147660000);
	assert.equal(b.txTone, "100.0");
	assert.equal(b.rxTone, "");
	assert.equal(b.power, 9);
	assert.equal(c.txHz, 444000000);
	assert.deepEqual([c.rxTone, c.txTone, c.bw, c.allSkip, c.power], ["123.0", "123.0", 12.5, true, 3]);
	assert.equal(d.txHz, 147500000);
	assert.deepEqual([e.txTone, e.rxTone], ["D047N", "D047I"]);
	assert.deepEqual([f.txTone, f.rxTone], ["100.0", "D023N"]);
	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.length]), [["Local", 6]]);
});

test("import replaces the channels and zones, and splits zones at 80 channels", () => {
	const m = empty();
	importChirp(m, SAMPLE);
	const rows = Array.from({ length: 100 }, (_, i) => `${i},C${i},146.${String(i).padStart(3, "0")}000,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,`);
	importChirp(m, `${HEAD}\n${rows.join("\n")}\n`, "Big");
	assert.equal(m.channels.length, 100);
	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.length]), [["Big", 80], ["Big 2", 20]]);
});

test("long names are shortened and noted", () => {
	const m = empty();
	const { notes } = importChirp(m, `${HEAD}\n0,A very long channel name,146.52,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,\n`);
	assert.equal(m.channels[0].name, "A very long chan");
	assert.deepEqual(notes, [{ code: "shortened", n: 1 }]);
});

test("export then import keeps what CHIRP can hold, and leaves DMR channels out", () => {
	const m = empty();
	importChirp(m, SAMPLE);
	m.channels.push({ ...m.channels[0], number: 7, name: "Digital", mode: "dmr" });
	const { text, notes } = exportChirp(m);
	assert.deepEqual(notes, [{ code: "dmr", n: 1 }]);
	const back = empty();
	assert.ok(isChirp(text));
	importChirp(back, text);
	const keys = ["name", "rxHz", "txHz", "bw", "rxTone", "txTone", "allSkip", "power"];
	assert.deepEqual(back.channels.map((c) => keys.map((k) => c[k])), m.channels.slice(0, 6).map((c) => keys.map((k) => c[k])));
});

// A made-up image in the layout chirp_img.js documents: memory, then the footer naming the radio
function image(model = "UV-5R", rclass = "BaofengUV5RGeneric") {
	const b = new Uint8Array(0x194A + 12 + 200).fill(0xFF);
	const put = (n, rx, tx, rxTone, txTone, f2, f3, name) => {
		const o = 8 + 16 * n;
		const bcd = (hz, at) => {
			let v = String(hz / 10).padStart(8, "0");
			for (let i = 0; i < 4; i++) b[o + at + i] = parseInt(v.slice(6 - 2 * i, 8 - 2 * i), 16);
		};
		bcd(rx, 0);
		bcd(tx, 4);
		b.set([rxTone & 255, rxTone >> 8, txTone & 255, txTone >> 8, 0, 0, f2, f3], o + 8);
		for (let i = 0; i < 16; i++) b[0x1008 + 16 * n + i] = i < name.length ? name.charCodeAt(i) : 0xFF;
	};
	put(0, 146520000, 146520000, 0, 0, 0, 0x44, "SIMPLEX");
	put(1, 443600000, 448600000, 1035, 1035, 0, 0x44, "RPTR");	// TSQL, +5 MHz
	put(5, 146940000, 146340000, 0, 885, 1, 0x00, "NARROW");	// tone, -0.6 MHz, low power, narrow, skipped
	put(9, 147060000, 147060000, 65, 65, 0, 0x40, "DCS");	// code 411, skipped
	b.set([0x63, 0x68, 0x69, 0x72, 0x70, 0xEE, 0x69, 0x6D, 0x67, 0x00, 0x01], 0x194A);
	const json = Buffer.from(JSON.stringify({ rclass, vendor: "Baofeng", model })).toString("base64");
	b.set(new TextEncoder().encode(json), 0x194A + 11);
	return b.subarray(0, 0x194A + 11 + json.length);
}

test("an image of the supported radio imports its channels", () => {
	const m = empty();
	assert.ok(isChirpImg(image()));
	const { counts, notes } = importChirpImg(m, image(), "Img");
	assert.deepEqual([counts.channels, notes], [4, []]);
	const [a, b, c, d] = m.channels;
	assert.deepEqual([a.name, a.rxHz, a.txHz, a.rxTone, a.txTone, a.bw, a.allSkip], ["SIMPLEX", 146520000, 146520000, "", "", 25, false]);
	assert.deepEqual([b.txHz, b.rxTone, b.txTone], [448600000, "103.5", "103.5"]);
	assert.deepEqual([c.txHz, c.rxTone, c.txTone, c.bw, c.allSkip, c.power], [146340000, "", "88.5", 12.5, true, 3]);
	assert.deepEqual([d.rxTone, d.txTone, d.allSkip], ["D411N", "D411N", true]);
});

test("an image of another radio is refused by name", () => {
	assert.throws(() => importChirpImg(empty(), image("UV-82", "BaofengUV82Radio")), (e) => e.code === "img_model" && e.params.model === "Baofeng UV-82");
	assert.throws(() => importChirpImg(empty(), new Uint8Array(100)), (e) => e.code === "not_chirp");
});

test("a file with nothing to import is refused and changes nothing", () => {
	const m = empty();
	importChirp(m, SAMPLE);
	const before = JSON.stringify([m.channels, m.zones]);
	assert.throws(() => importChirp(m, `${HEAD}\n`), (e) => e.code === "empty");
	const dv = `${HEAD}\n0,D-Star,146.94,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,DV,5,,,,,,,\n`;
	assert.throws(() => importChirp(m, dv), (e) => e.code === "empty" && e.params.notes[0].code === "mode");
	const blank = image();
	blank.fill(0xFF, 0, 0x1808);
	assert.throws(() => importChirpImg(m, blank), (e) => e.code === "empty");
	assert.equal(JSON.stringify([m.channels, m.zones]), before);
});

test("reverse tone squelch is counted as a tone the radio can't hold", () => {
	const rows = ["DTCS-R", "TSQL-R"].map((t, i) => `${i},R${i},146.5${i},,0.6,${t},88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,`);
	const m = empty();
	const { counts, notes } = importChirp(m, `${HEAD}\n${rows.join("\n")}\n`);
	assert.equal(counts.channels, 2);
	assert.deepEqual(notes, [{ code: "bad_tone", n: 2 }]);
	assert.deepEqual(m.channels.map((c) => [c.rxTone, c.txTone]), [["", ""], ["", ""]]);
});

test("rows that can't be read are counted apart from other modes", () => {
	const ok = "0,Simplex,146.52,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,";
	const short = "1,Short,146.53,,0.6";
	const unclosed = '2,"Open quote,146.54,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,\n3,Lost,146.55,,0.6,,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,';
	const m = empty();
	const { counts, notes } = importChirp(m, `${HEAD}\n${ok}\n${short}\n${unclosed}\n`);
	assert.equal(counts.channels, 1);
	assert.deepEqual(notes, [{ code: "unreadable", n: 2 }]);
});

// Bytes with a footer naming model at offset at, the memory before it all 0xFF
function footerOnly(at, model, rclass) {
	const json = Buffer.from(JSON.stringify({ rclass, vendor: "Baofeng", model })).toString("base64");
	const b = new Uint8Array(at + 11 + json.length).fill(0xFF);
	b.set([0x63, 0x68, 0x69, 0x72, 0x70, 0xEE, 0x69, 0x6D, 0x67, 0x00, 0x01], at);
	b.set(new TextEncoder().encode(json), at + 11);
	return b;
}

test("a larger image of another radio is refused by name; a cut short one by its size, another class by its class", () => {
	const big = footerOnly(0x4000, "UV-82", "BaofengUV82Radio");
	assert.ok(isChirpImg(big));
	assert.throws(() => importChirpImg(empty(), big), (e) => e.code === "img_model" && e.params.model === "Baofeng UV-82");
	assert.throws(() => importChirpImg(empty(), footerOnly(0x1000, "UV-5R", "BaofengUV5RGeneric")), (e) => e.code === "img_size");
	assert.throws(() => importChirpImg(empty(), image("UV-5R", "DynamicRadioAlias")), (e) => e.code === "img_class" && e.params.rclass === "DynamicRadioAlias");
});

test("an image channel with no TX frequency is receive only, not simplex", () => {
	const b = image();
	b.fill(0xFF, 8 + 4, 8 + 8);	// channel 0's TX frequency
	const m = empty();
	importChirpImg(m, b);
	const c = m.channels[0];
	assert.deepEqual([c.name, c.rxOnly, c.txHz], ["SIMPLEX", true, 146520000]);
	assert.equal(m.channels[1].rxOnly, false);
});

test("a repeater row without a usable transmit frequency is receive only, not simplex", () => {
	const row = (n, name, duplex, offset) =>
		`${n},${name},146.94,${duplex},${offset},,88.5,88.5,023,NN,023,Tone->Tone,FM,5,,,,,,,`;
	const rows = [row(0, "NoOff", "-", ""), row(1, "Below0", "-", "200.0"), row(2, "Split0", "split", "0"),
		row(3, "Good", "-", "0.6")];
	const m = empty();
	const { notes } = importChirp(m, `${HEAD}\n${rows.join("\n")}\n`);
	assert.deepEqual(m.channels.map((c) => [c.name, c.rxOnly, c.txHz]), [["NoOff", true, 146940000],
		["Below0", true, 146940000], ["Split0", true, 146940000], ["Good", false, 146340000]]);
	assert.deepEqual(notes, [{ code: "bad_frequency", n: 3 }]);
});

test("an image channel whose TX bytes aren't a frequency is receive only", () => {
	const b = image();
	b.fill(0x00, 8 + 4, 8 + 8);	// channel 0's TX frequency all zero, not 0xFF
	const m = empty();
	importChirpImg(m, b);
	assert.deepEqual([m.channels[0].rxOnly, m.channels[0].txHz], [true, 146520000]);
});
