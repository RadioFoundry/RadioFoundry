// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CHIRP CSV in the codeplug editor (static/js/codeplug/chirp.js): the mapping to the model, what is left out, and a
// round trip through the export. The sample rows are made up.

import { test } from "node:test";
import assert from "node:assert/strict";

import { empty } from "../static/js/codeplug/model.js";
import { importChirp, exportChirp, isChirp, ChirpError } from "../static/js/codeplug/chirp.js";

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
