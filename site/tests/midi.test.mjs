// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A MIDI file as the fox jingle in the Codeplug Editor (static/js/codeplug/midi.js): the file format, the tempo map, one
// voice from chords, the radio's range and grid, and the jingle's limits.
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { parseMidi, midiToJingle, MidiError } from "../static/js/codeplug/midi.js";
import { FOX_STEPS, FOX_CHAIN_MAX, FOX_MS_MAX } from "../static/js/codeplug/kv.js";

const PPQ = 480;
const vlq = (n) => {
	const out = [n & 0x7F];
	for (n >>= 7; n; n >>= 7) out.unshift((n & 0x7F) | 0x80);
	return out;
};
const u32 = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
const ascii = (s) => [...s].map((c) => c.charCodeAt(0));
// A track chunk from [delta, ...event bytes] pairs, ended with End of Track
const track = (evs) => {
	const body = [...evs.flatMap(([d, ...e]) => [...vlq(d), ...e]), 0, 0xFF, 0x2F, 0];
	return [...ascii("MTrk"), ...u32(body.length), ...body];
};
const file = (tracks, { fmt = 1, div = PPQ } = {}) =>
	Uint8Array.from([...ascii("MThd"), ...u32(6), 0, fmt, 0, tracks.length, div >> 8, div & 255, ...tracks.flat()]);
const tempo = (us) => [0xFF, 0x51, 3, (us >> 16) & 255, (us >> 8) & 255, us & 255];
const name = (s) => [0xFF, 0x03, s.length, ...ascii(s)];

// Format 1: a tempo track (120 bpm, 240 bpm from beat 4), a melody (C4 E4 G4 G4 with running status and note-on
// velocity 0 as note-off, then a C5 + E5 chord for two beats) and a drum track
const SONG = file([
	track([[0, ...name("Tempo")], [0, ...tempo(500000)], [PPQ * 4, ...tempo(250000)]]),
	track([[0, ...name("Melody")],
		...[60, 64, 67, 67].flatMap((n) => [[0, 0x90, n, 100], [PPQ, n, 0]]),
		[0, 0x90, 72, 100], [0, 76, 100], [PPQ * 2, 0x80, 72, 64], [0, 0x80, 76, 64]]),
	track([[0, ...name("Drums")], [0, 0x99, 36, 100], [PPQ, 0x89, 36, 0]]),
]);

const steps = (j) => j.hz.slice(0, j.steps).map((hz, i) => [hz, j.ms[i]]);

test("the file's notes in seconds over its tempo map, the tracks' names", () => {
	const { notes, names } = parseMidi(SONG);
	assert.deepEqual(names, { 0: "Tempo", 1: "Melody", 2: "Drums" });
	assert.equal(notes.length, 7);
	const g = notes.filter((n) => n.note === 67).sort((a, b) => a.start - b.start);
	assert.deepEqual(g.map((n) => [n.start, n.end]), [[1, 1.5], [1.5, 2]], "120 bpm: a beat is 0.5 s");
	const top = notes.find((n) => n.note === 76);
	assert.deepEqual([top.start, top.end], [2, 2.5], "240 bpm from beat 4: two beats are 0.5 s");
	assert.equal(notes.find((n) => n.channel === 9).track, 2);
});

test("a song as the jingle: one voice, an octave up into range, drums left out, a repeated note struck twice", () => {
	const j = midiToJingle(SONG);
	assert.equal(j.shift, 12, "C4 (262 Hz) is below 300 Hz: an octave up");
	assert.deepEqual(steps(j), [[520, 500], [660, 500], [780, 470], [0, 30], [780, 500], [1320, 500]],
		"the same steps as tools' fox_midi.py; the chord plays its top note");
	assert.equal(j.totalMs, 2500);
	assert.equal(j.cutMs, null);
	assert.equal(j.hz.length, FOX_STEPS);
	assert.equal(j.ms.length, FOX_STEPS);
	assert.ok(j.ms.slice(j.steps).every((m) => m === 0) && j.hz.slice(j.steps).every((h) => h === 0));
});

test("speed, the other voices, no octave move", () => {
	assert.deepEqual(steps(midiToJingle(SONG, { speed: 2 })).slice(0, 2), [[520, 250], [660, 250]]);
	assert.equal(steps(midiToJingle(SONG, { pick: "bottom" }))[5][0], 1050, "the chord's lower note, C6 after the octave");
	assert.equal(steps(midiToJingle(SONG, { autoOctave: false }))[0][0], 260, "C4 as it is");
	assert.equal(steps(midiToJingle(SONG, { drums: true, pick: "bottom", autoOctave: false }))[0][0], 70,
		"with the drums, the kick (C2) is the lowest note at the start");
});

test("the jingle's limits: cut at 10 s or 32 steps, a long note split over steps, frequencies over 3400 Hz halved", () => {
	const slow = midiToJingle(SONG, { speed: 0.2 });
	assert.equal(slow.totalMs, FOX_CHAIN_MAX);
	assert.equal(slow.cutMs, FOX_CHAIN_MAX);
	// 40 short notes: only 32 steps fit
	const many = file([track([...Array(40).keys()].flatMap((k) => [[0, 0x90, 72 + (k % 5), 90], [PPQ / 4, 0x80, 72 + (k % 5), 0]]))]);
	const m = midiToJingle(many);
	assert.equal(m.steps, FOX_STEPS);
	assert.equal(m.cutMs, 4000, "32 eighths at 120 bpm, 125 ms each: rounded from the start, so no drift");
	// a 6 s note: three steps of at most 2550 ms
	const long = midiToJingle(file([track([[0, 0x90, 81, 90], [PPQ * 12, 0x80, 81, 0]])]));
	assert.deepEqual(steps(long), [[880, FOX_MS_MAX], [880, FOX_MS_MAX], [880, 900]]);
	// C8 (4186 Hz) is over the radio's top even after the octave move: halved to C7
	const high = midiToJingle(file([track([[0, 0x90, 108, 90], [PPQ, 0x80, 108, 0]])]), { autoOctave: false });
	assert.deepEqual(steps(high), [[2090, 500]]);
	assert.ok(m.hz.every((h) => h % 10 === 0 && h <= 3400) && m.ms.every((x) => x % 10 === 0));
});

test("files it can't read", () => {
	const code = (bytes) => {
		try {
			midiToJingle(bytes);
		} catch (e) {
			assert.ok(e instanceof MidiError);
			return e.code;
		}
		return "read";
	};
	assert.equal(code(Uint8Array.from(ascii("RIFF0000WAVEfmt "))), "not_midi");
	assert.equal(code(file([track([[0, 0x90, 60, 90], [100, 0x80, 60, 0]])], { div: 0xE728 })), "smpte");
	assert.equal(code(SONG.subarray(0, 60)), "truncated");
	assert.equal(code(file([track([[0, 0x99, 36, 100], [PPQ, 0x89, 36, 0]])])), "no_notes", "drums only");
	assert.equal(code(file([track([[0, ...tempo(500000)]])])), "no_notes");
});
