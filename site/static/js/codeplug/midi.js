// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A Standard MIDI File as a fox jingle (codeplug-settings.md 3.4, radio-features.md 3.8): the jingle is one tone at a
// time, up to FOX_STEPS steps of a frequency (0..SIG_HZ_MAX on the SIG_HZ_UNIT grid, 0 a pause) and a length (10 ms
// units, up to FOX_MS_MAX a step), FOX_CHAIN_MAX ms in all. A MIDI file is polyphonic, so one note is picked at each
// moment (the highest sounding, usually the melody), its timing rounded to 10 ms, the tune moved by octaves into the
// radio's range and cut where the jingle's limits run out. Written from the SMF 1.0 file format: header chunk, track
// chunks, variable-length delta times, running status, meta events (0x51 tempo, 0x03 track name, 0x2F end of track).

import { FOX_STEPS, FOX_MS_MAX, FOX_CHAIN_MAX, SIG_HZ_MAX, SIG_HZ_UNIT } from "./kv.js";

const MS_UNIT = 10;
const DRUMS = 9;		// MIDI channel 10, General MIDI percussion: no pitch to play
const LOW_HZ = 300;		// below this small speakers and the FM de-emphasis lose a tune: moved up by octaves

export class MidiError extends Error {
	constructor(code) {
		super(code);
		this.code = code;	// "not_midi", "smpte", "truncated", "no_notes"
	}
}

// The file's notes as {start, end (seconds), note (0..127), channel (0..15), track}, and the tracks' names
export function parseMidi(bytes) {
	const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
	const u32 = (i) => ((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]) >>> 0;
	const u16 = (i) => (b[i] << 8) | b[i + 1];
	const tag = (i) => String.fromCharCode(b[i], b[i + 1], b[i + 2], b[i + 3]);
	if (b.length < 14 || tag(0) !== "MThd") throw new MidiError("not_midi");
	const ntrk = u16(10), div = u16(12);
	if (div & 0x8000) throw new MidiError("smpte");
	const notes = [], tempos = [], names = {};
	let i = 8 + u32(4);
	const need = (n) => {
		if (n > b.length) throw new MidiError("truncated");
	};
	for (let t = 0; t < ntrk; t++) {
		for (;;) {	// unknown chunks are skipped
			need(i + 8);
			if (tag(i) === "MTrk") break;
			i += 8 + u32(i + 4);
		}
		const end = i + 8 + u32(i + 4);
		need(end);
		let j = i + 8, tick = 0, status = 0;
		const sounding = new Map();
		const vlq = () => {
			let v = 0, c;
			do {
				need(j + 1);
				c = b[j++];
				v = v * 128 + (c & 0x7F);
			} while (c & 0x80);
			return v;
		};
		while (j < end) {
			tick += vlq();
			need(j + 1);
			const c = b[j];
			if (c === 0xFF) {
				const kind = b[j + 1];
				j += 2;
				const len = vlq();
				need(j + len);
				if (kind === 0x51 && len === 3) tempos.push([tick, (b[j] << 16) | (b[j + 1] << 8) | b[j + 2]]);
				else if (kind === 0x03) names[t] = String.fromCharCode(...b.subarray(j, j + len)).trim();
				j += len;
				if (kind === 0x2F) break;
				continue;
			}
			if (c === 0xF0 || c === 0xF7) {
				j++;
				j += vlq();
				continue;
			}
			if (c & 0x80) {
				status = c;
				j++;
			}
			const hi = status & 0xF0, ch = status & 0x0F;
			if (hi === 0xC0 || hi === 0xD0) {
				j++;
				continue;
			}
			need(j + 2);
			const a1 = b[j], a2 = b[j + 1];
			j += 2;
			const key = ch * 128 + a1;
			if (hi === 0x90 && a2 > 0) {
				if (sounding.has(key)) notes.push([sounding.get(key), tick, a1, ch, t]);	// struck again while held
				sounding.set(key, tick);
			} else if (hi === 0x80 || hi === 0x90) {
				if (sounding.has(key) && tick > sounding.get(key)) notes.push([sounding.get(key), tick, a1, ch, t]);
				sounding.delete(key);
			}
		}
		for (const [key, start] of sounding) notes.push([start, tick, key % 128, Math.floor(key / 128), t]);
		i = end;
	}
	// Ticks to seconds over the tempo map (500000 us a quarter note, 120 bpm, until the first tempo event)
	tempos.sort((x, y) => x[0] - y[0]);
	const points = [[0, 0, 500000]];
	let s = 0, last = 0, us = 500000;
	for (const [tick, next] of tempos) {
		s += ((tick - last) * us) / div / 1e6;
		points.push([tick, s, next]);
		last = tick;
		us = next;
	}
	const sec = (tick) => {
		let p = points[0];
		for (const q of points) {
			if (q[0] > tick) break;
			p = q;
		}
		return p[1] + ((tick - p[0]) * p[2]) / div / 1e6;
	};
	return {
		notes: notes.map(([a, z, note, channel, track]) => ({ start: sec(a), end: sec(z), note, channel, track })),
		names,
	};
}

export const noteHz = (n) => 440 * 2 ** ((n - 69) / 12);

// [{start, end, note | null}]: one note at a time, the highest sounding (top), the lowest (bottom) or the latest
// started (last); null is a rest
export function melody(notes, pick = "top") {
	const edges = [...new Set(notes.flatMap((n) => [n.start, n.end]))].sort((x, y) => x - y);
	const out = [];
	for (let k = 0; k + 1 < edges.length; k++) {
		const a = edges[k], z = edges[k + 1];
		const on = notes.filter((n) => n.start <= a && n.end >= z);
		let cur = null;
		for (const n of on) {
			if (cur === null) cur = n;
			else if (pick === "top" ? n.note > cur.note || (n.note === cur.note && n.start > cur.start)
				: pick === "bottom" ? n.note < cur.note || (n.note === cur.note && n.start > cur.start)
				: n.start > cur.start || (n.start === cur.start && n.note > cur.note)) cur = n;
		}
		if (out.length && out[out.length - 1].src === cur) out[out.length - 1].end = z;
		else out.push({ start: a, end: z, src: cur });
	}
	return out.map((m) => ({ start: m.start, end: m.end, note: m.src === null ? null : m.src.note }));
}

// Octaves to add so the notes sit above LOW_HZ (when the top still fits) and at most SIG_HZ_MAX
function octaveShift(mel, transpose) {
	const ns = mel.filter((m) => m.note !== null).map((m) => m.note + transpose);
	if (!ns.length) return transpose;
	let lo = Math.min(...ns), hi = Math.max(...ns);
	while (noteHz(lo) < LOW_HZ && noteHz(hi + 12) <= SIG_HZ_MAX) {
		transpose += 12;
		lo += 12;
		hi += 12;
	}
	while (noteHz(hi) > SIG_HZ_MAX) {
		transpose -= 12;
		hi -= 12;
	}
	return transpose;
}

// [[hz, ms]] on the jingle's grid. Ends are rounded on the 10 ms grid from the start, so rounding never drifts; a note
// shorter than minMs joins the step before; a note struck again at the same pitch gets a restrikeMs rest so it is
// heard twice; a step longer than FOX_MS_MAX becomes several
function toSteps(mel, transpose, restrikeMs, minMs) {
	const steps = [];
	let prev = 0;
	for (const m of mel) {
		const end = Math.round((m.end * 1000) / MS_UNIT) * MS_UNIT, ms = end - prev;
		if (ms <= 0) continue;
		prev = end;
		let hz = 0;
		if (m.note !== null) {
			let f = noteHz(m.note + transpose);
			while (f > SIG_HZ_MAX + SIG_HZ_UNIT / 2) f /= 2;
			hz = Math.round(f / SIG_HZ_UNIT) * SIG_HZ_UNIT;
		}
		const last = steps[steps.length - 1];
		if (last && ms < minMs) {
			last[1] += ms;
			continue;
		}
		if (last && hz !== 0 && last[0] === hz && restrikeMs && last[1] > restrikeMs + MS_UNIT) {
			last[1] -= restrikeMs;
			steps.push([0, restrikeMs]);
		}
		const prior = steps[steps.length - 1];
		if (prior && prior[0] === 0 && hz === 0) {
			prior[1] += ms;
			continue;
		}
		steps.push([hz, ms]);
	}
	const out = [];
	for (const [hz, ms] of steps) {
		for (let left = ms; left > 0; left -= FOX_MS_MAX) out.push([hz, Math.min(left, FOX_MS_MAX)]);
	}
	while (out.length && out[out.length - 1][0] === 0) out.pop();
	return out;
}

// The file as a jingle: {hz[FOX_STEPS], ms[FOX_STEPS], steps, totalMs, cutMs (where the tune was cut, else null),
// shift (semitones moved)}. speed scales the tempo (2: twice as fast)
export function midiToJingle(bytes, { speed = 1, pick = "top", transpose = 0, autoOctave = true, restrikeMs = 30,
	minMs = 30, drums = false } = {}) {
	const { notes } = parseMidi(bytes);
	const kept = notes.filter((n) => drums || n.channel !== DRUMS);
	if (!kept.length) throw new MidiError("no_notes");
	const first = Math.min(...kept.map((n) => n.start));
	const scaled = kept.map((n) => ({ ...n, start: (n.start - first) / speed, end: (n.end - first) / speed }));
	const mel = melody(scaled, pick);
	const shift = autoOctave ? octaveShift(mel, transpose) : transpose;
	const all = toSteps(mel, shift, restrikeMs, minMs);
	const fit = [];
	let total = 0, cutMs = null;
	for (const [hz, ms] of all) {
		if (fit.length === FOX_STEPS || total >= FOX_CHAIN_MAX) {
			while (fit.length && fit[fit.length - 1][0] === 0) total -= fit.pop()[1];
			cutMs = total;
			break;
		}
		const take = Math.min(ms, FOX_CHAIN_MAX - total);
		fit.push([hz, take]);
		total += take;
	}
	if (!fit.length) throw new MidiError("no_notes");
	const pad = (xs) => [...xs, ...Array(FOX_STEPS - xs.length).fill(0)];
	return { hz: pad(fit.map((x) => x[0])), ms: pad(fit.map((x) => x[1])), steps: fit.length, totalMs: total, cutMs, shift };
}
