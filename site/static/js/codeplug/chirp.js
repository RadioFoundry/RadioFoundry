// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CHIRP's CSV channel list (Location, Name, Frequency, Duplex, Offset, Tone, ...) into the editing model (model.js).
// An import replaces the codeplug's channels and zones with the file's FM channels as one change, as the qdmr
// import does; rows it can't hold are counted in the notes. The column layout is the one CHIRP's documentation shows for
// its CSV files, not CHIRP's source.

import { parse } from "./csv.js";
import { LIMITS } from "./records.js";
import { parseTone } from "./fields.js";

export class ChirpError extends Error {
	constructor(code, params = {}) {
		super(code);
		this.code = code;
		this.params = params;
	}
}

const REQUIRED = ["location", "name", "frequency", "duplex", "offset", "tone", "mode"];

// Power: the channel's power byte (open-firmware-cps.md 7.3.3); 0 takes the radio's setting
const LOW = 3, MID = 6, HIGH = 9;

// True when the file's header row is CHIRP's: the Codeplug Editor's Open File uses it to pick this importer
export function isChirp(text) {
	const head = (parse(text)[0] || []).map((h) => h.trim().toLowerCase());
	return REQUIRED.every((k) => head.includes(k));
}

const mhz = (s) => {
	const v = Number(String(s ?? "").trim());
	return Number.isFinite(v) && v > 0 ? Math.round(v * 1e5) * 10 : null;
};

// CHIRP writes power as "5.0W" or a level name; null when there is none to use
function power(s) {
	s = String(s ?? "").trim().toLowerCase();
	if (!s) return 0;
	const w = /^([0-9]*\.?[0-9]+)\s*w/.exec(s);
	if (w) return Number(w[1]) <= 2 ? LOW : Number(w[1]) <= 5 ? MID : HIGH;
	if (/^(l|low|min)/.test(s)) return LOW;
	if (/^(m|mid|med)/.test(s)) return MID;
	if (/^(h|high|max)/.test(s)) return HIGH;
	return 0;
}

// The radio's TX and RX tones for a row: CHIRP's Tone column says which of its tone columns apply. null if a tone
// the radio can't hold
function tones(r, note) {
	const ctcss = (k) => parseTone(Number(r[k]).toFixed(1));
	// CHIRP marks a reversed DCS polarity R, the model I
	const code = (k, pol) => parseTone(`D${String(r[k] ?? "").trim().padStart(3, "0")}${pol === "R" ? "I" : pol}`);
	const polarity = (r.dtcspolarity || "NN").toUpperCase().padEnd(2, "N");
	const mode = (r.tone || "").trim();
	const cross = (r.crossmode || "").trim();
	let tx = "", rx = "";
	if (mode === "Tone") tx = ctcss("rtonefreq");
	else if (mode === "TSQL") tx = rx = ctcss("ctonefreq");
	else if (mode === "DTCS") {
		tx = code("dtcscode", polarity[0]);
		rx = code("dtcscode", polarity[1]);
	} else if (mode === "Cross") {
		// "Tone->DTCS": the first is sent, the second is needed to hear
		const [a, b] = cross.split("->");
		const one = (x, tonekey, codekey, pol) => (x === "Tone" ? ctcss(tonekey) : x === "DTCS" ? code(codekey, pol) : "");
		tx = one(a, "rtonefreq", "dtcscode", polarity[0]);
		rx = one(b, "ctonefreq", r.rxdtcscode ? "rxdtcscode" : "dtcscode", polarity[1]);
	} else if (mode) {
		note("bad_tone");	// DTCS-R and TSQL-R (reverse tone squelch) among them
	}
	if (tx === null || rx === null) {
		note("bad_tone");
		return { tx: "", rx: "" };
	}
	return { tx, rx };
}

// Replace m's channels and zones with the file's FM channels, in zones named zoneName. Returns {counts: {channels, zones}, notes}.
// A row that ends before the Mode column, or has a line break in a cell (a quote never closed takes in the rest of the
// file), is counted as unreadable rather than taken for a channel in another mode
export function importChirp(m, textIn, zoneName = "CHIRP") {
	const rows = parse(textIn);
	const head = (rows[0] || []).map((h) => h.trim().toLowerCase());
	if (!REQUIRED.every((k) => head.includes(k))) throw new ChirpError("not_chirp");
	const modeAt = head.indexOf("mode");
	const notes = new Map();
	const good = rows.slice(1).filter((cells) => {
		if (cells.length > modeAt && !cells.some((c) => /[\r\n]/.test(c))) return true;
		notes.set("unreadable", (notes.get("unreadable") || 0) + 1);
		return false;
	});
	return importRows(m, good.map((cells) => Object.fromEntries(head.map((h, i) => [h, (cells[i] ?? "").trim()]))), zoneName, notes);
}

// The same for rows already split into CHIRP's columns (lower case keys), as the image reader makes them. A row can
// carry the radio's tones directly as _tx and _rx
export function importRows(m, rows, zoneName = "CHIRP", notes = new Map()) {
	const note = (code, n = 1) => notes.set(code, (notes.get(code) || 0) + n);
	const made = [];
	for (const r of rows) {
		if (!r.frequency && !r.name) continue;
		const mode = (r.mode || "").toUpperCase();
		if (mode !== "FM" && mode !== "NFM") {
			note("mode");
			continue;
		}
		const rxHz = mhz(r.frequency);
		let txHz = rxHz;
		const off = mhz(r.offset);
		if (!rxHz) {
			note("bad_frequency");
			continue;
		}
		// A repeater channel without a usable transmit frequency is receive only: simplex would transmit on its output
		let rxOnly = r.duplex === "off";
		if (r.duplex === "+" || r.duplex === "-" || r.duplex === "split") {
			// split: the offset column holds the transmit frequency
			const tx = off === null ? null : r.duplex === "split" ? off : r.duplex === "+" ? rxHz + off : rxHz - off;
			if (tx === null || tx <= 0) {
				note("bad_frequency");
				rxOnly = true;
			} else txHz = tx;
		}
		const t = r._tx !== undefined ? { tx: r._tx, rx: r._rx } : tones(r, note);
		let name = r.name || `${(rxHz / 1e6).toFixed(4)}`;
		if (name.length > LIMITS.name) {
			name = name.slice(0, LIMITS.name);
			note("shortened");
		}
		made.push({
			number: null, name, rxHz, txHz, mode: "fm", power: power(r.power), tot: 0,
			rxTone: t.rx, txTone: t.tx, bw: mode === "NFM" ? 12.5 : 25, rxOnly, allSkip: r.skip === "S",
			zoneSkip: false, vox: false, squelch: 0, colorCode: 0, timeslot: 1, contact: null, rxList: null, aprs: null,
			dmrId: 0, noBeep: false, noEco: false, dmo: false, roam: false, aliasTs1: 0, aliasTs2: 0, useLocation: false,
			lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0, toneBurst: false,
		});
	}
	// Nothing to bring in: the codeplug stays as it is rather than losing its channels to an empty list
	if (!made.length) throw new ChirpError("empty", { notes: [...notes].map(([code, n]) => ({ code, n })) });
	if (made.length > LIMITS.channels) {
		note("too_many_channels", made.length - LIMITS.channels);
		made.length = LIMITS.channels;
	}
	// Channels take numbers in file order, so the radio lists them as CHIRP does. They replace the channels and zones;
	// contacts and the rest stay. Zones of up to 80 channels: "name", then "name 2", ...
	made.forEach((c, i) => (c.number = i + 1));
	const zones = [];
	for (let i = 0; i < made.length; i += LIMITS.zoneChannels) {
		if (zones.length >= LIMITS.zones) {
			note("too_many_zones");
			break;
		}
		const n = i / LIMITS.zoneChannels + 1;
		const suffix = n > 1 ? ` ${n}` : "";
		zones.push({ name: zoneName.slice(0, LIMITS.name - suffix.length) + suffix, channels: made.slice(i, i + LIMITS.zoneChannels), slot: null });
	}
	m.channels = made;
	m.zones = zones;
	for (const v of m.vfo) if (v) Object.assign(v, { contact: null, rxList: null, aprs: null });
	return { counts: { channels: made.length, zones: zones.length }, notes: [...notes].map(([code, n]) => ({ code, n })) };
}

// --- Export ---

const HEAD = ["Location", "Name", "Frequency", "Duplex", "Offset", "Tone", "rToneFreq", "cToneFreq", "DtcsCode", "DtcsPolarity",
	"RxDtcsCode", "CrossMode", "Mode", "TStep", "Skip", "Power", "Comment"];
const WATTS = { 1: "1.0W", 3: "2.0W", 6: "5.0W", 9: "10.0W", 10: "10.0W" };
const mhzText = (hz) => (hz / 1e6).toFixed(6);

// CHIRP's tone columns for a channel's two tones
function toneColumns(c) {
	const dcs = (t) => t.startsWith("D");
	const tx = c.txTone, rx = c.rxTone;
	const out = { Tone: "", rToneFreq: "88.5", cToneFreq: "88.5", DtcsCode: "023", DtcsPolarity: "NN", RxDtcsCode: "023", CrossMode: "Tone->Tone" };
	const code = (t) => t.slice(1, 4);
	if (!tx && !rx) return out;
	if (tx && !dcs(tx) && !rx) return { ...out, Tone: "Tone", rToneFreq: tx };
	if (tx && !dcs(tx) && tx === rx) return { ...out, Tone: "TSQL", rToneFreq: tx, cToneFreq: tx };
	if (tx && rx && dcs(tx) && code(tx) === code(rx)) {
		return { ...out, Tone: "DTCS", DtcsCode: code(tx), RxDtcsCode: code(tx), DtcsPolarity: (tx.endsWith("I") ? "R" : "N") + (rx.endsWith("I") ? "R" : "N") };
	}
	// anything else is a cross mode: what is sent, then what is needed to hear
	const kind = (t) => (!t ? "" : dcs(t) ? "DTCS" : "Tone");
	return {
		...out, Tone: "Cross", CrossMode: `${kind(tx)}->${kind(rx)}`,
		rToneFreq: tx && !dcs(tx) ? tx : out.rToneFreq, cToneFreq: rx && !dcs(rx) ? rx : out.cToneFreq,
		DtcsCode: tx && dcs(tx) ? code(tx) : rx && dcs(rx) ? code(rx) : out.DtcsCode,
		RxDtcsCode: rx && dcs(rx) ? code(rx) : out.RxDtcsCode,
		DtcsPolarity: (tx.endsWith("I") ? "R" : "N") + (rx.endsWith("I") ? "R" : "N"),
	};
}

// CHIRP CSV for the model's FM channels: {text, notes}. DMR channels have no place in a CHIRP file.
export function exportChirp(m) {
	const notes = new Map();
	const rows = [HEAD];
	for (const c of m.channels) {
		if (c.mode !== "fm") {
			notes.set("dmr", (notes.get("dmr") || 0) + 1);
			continue;
		}
		const diff = c.txHz - c.rxHz;
		const duplex = diff === 0 ? "" : Math.abs(diff) < 1e8 ? (diff > 0 ? "+" : "-") : "split";
		const t = toneColumns(c);
		rows.push([c.number, c.name, mhzText(c.rxHz), c.rxOnly ? "off" : duplex, duplex === "split" ? mhzText(c.txHz) : mhzText(Math.abs(diff)),
			t.Tone, t.rToneFreq, t.cToneFreq, t.DtcsCode, t.DtcsPolarity, t.RxDtcsCode, t.CrossMode, c.bw === 25 ? "FM" : "NFM", "5.00",
			c.allSkip ? "S" : "", WATTS[c.power] || "", ""]);
	}
	const q = (v) => (/[",\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v));
	return { text: rows.map((r) => r.map(q).join(",")).join("\r\n") + "\r\n", notes: [...notes].map(([code, n]) => ({ code, n })) };
}
