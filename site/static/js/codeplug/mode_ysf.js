// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Yaesu System Fusion mode module of the Codeplug Editor (docs/spec/ysf-codeplug.md): kind 6 on the channel-mode
// layer (channelmodes.js) and its radio-wide record. The stored callsign fields are on-air text (ASCII, space padded);
// the model holds them as strings without the trailing spaces, cleaned the way the firmware reads them (1), so what the
// editor shows is what the radio sends. No system type and no contacts: a Fusion call is addressed by callsign.

import { u8, grow } from "./modebytes.js";

export const CALL_LEN = 10, RADIO_ID_LEN = 5;
export const CHAN_LEN = 13, STATION_LEN = 15;
export const NARROW = 0x01;
export const VW = 0x02;		// PTT sends Voice FR (VW); clear: DN (V/D mode 2)
const FLAGS = NARROW | VW;
export const DEST_ALL = "ALL";
export const DGID_MAX = 99;

// The characters a callsign field keeps: A-Z 0-9 space / -
const CALL_OK = /^[A-Z0-9 /-]$/;
const callChar = (c) => {
	const ch = String.fromCharCode(c >= 0x61 && c <= 0x7A ? c - 0x20 : c);
	return CALL_OK.test(ch) ? ch : " ";
};

// n bytes of b from o as text (bytes past the record's end read as spaces), trailing spaces dropped
function readText(b, o, n) {
	let s = "";
	for (let i = 0; i < n; i++) s += callChar(u8(b, o + i));
	return s.trimEnd();
}

function putText(v, o, n, text) {
	const s = String(text ?? "");
	for (let i = 0; i < n; i++) v[o + i] = i < s.length ? callChar(s.charCodeAt(i)).charCodeAt(0) : 0x20;
}

// A DG-ID byte: erased (0xFF) or past 99 reads as 0
const dgid = (v) => (v <= DGID_MAX ? v : 0);

// --- Channel (2.1) ---

export function decodeChan(b) {
	const dest = readText(b, 0, CALL_LEN);
	const f = u8(b, 12);
	return {
		dest: dest.trim() === "" ? DEST_ALL : dest,
		txDgid: dgid(u8(b, 10)),
		rxDgid: dgid(u8(b, 11)),
		narrow: f !== 0xFF && (f & NARROW) !== 0,
		voice: f !== 0xFF && (f & VW) !== 0 ? 1 : 0,
	};
}

export function encodeChan(p, prev = null) {
	const v = grow(prev, CHAN_LEN);
	putText(v, 0, CALL_LEN, p.dest);
	v[10] = dgid(p.txDgid ?? 0);
	v[11] = dgid(p.rxDgid ?? 0);
	const keep = v[12] === 0xFF ? 0 : v[12] & ~FLAGS;
	v[12] = keep | (p.narrow ? NARROW : 0) | (Number(p.voice) === 1 ? VW : 0);
	return v;
}

// --- Radio-wide record (3) ---

export function decodeStation(b) {
	return { call: readText(b, 0, CALL_LEN), radioId: readText(b, 10, RADIO_ID_LEN) };
}

export function encodeStation(s, prev = null) {
	const blank = !s.call?.trim() && !s.radioId?.trim();
	// all blank is the default: no record, unless one holds bytes a later firmware appended
	if (blank && !(prev && prev.length > STATION_LEN)) return null;
	const v = grow(prev, STATION_LEN);
	putText(v, 0, CALL_LEN, s.call);
	putText(v, 10, RADIO_ID_LEN, s.radioId);
	return v;
}

// A callsign field's problems: too long, or characters the radio would send as spaces. Lower case is fine: it is
// stored upper-cased
function checkCall(v, key, max, problem) {
	const s = String(v ?? "");
	if (s.length > max) problem.err(key, "ysf_call_long", { max });
	if (!/^[A-Za-z0-9 /-]*$/.test(s)) problem.err(key, "ysf_call_chars");
}

export const ysfKind = {
	id: 6,
	feature: "YSF",
	systemType: null,
	contacts: false,
	ownFreqs: true,
	defaultTx: true,	// an amateur mode: a new channel may transmit; the base record is still the stand-in
	recordFields: [],
	flags: {},
	newParams: () => ({ dest: DEST_ALL, txDgid: 0, rxDgid: 0, narrow: false, voice: 0 }),
	decode: decodeChan,
	encode: encodeChan,
	fields: () => [
		{ key: "dest", label: "editor.ysf.dest", type: "text", help: "editor.ysf.dest_help" },
		{ key: "txDgid", label: "editor.ysf.tx_dgid", type: "int", min: 0, max: DGID_MAX, help: "editor.ysf.dgid_help" },
		{ key: "rxDgid", label: "editor.ysf.rx_dgid", type: "int", min: 0, max: DGID_MAX, help: "editor.ysf.rx_dgid_help" },
		{ key: "voice", label: "editor.ysf.voice", type: "select", help: "editor.ysf.voice_help",
			options: () => [[0, "editor.ysf.voice_dn"], [1, "editor.ysf.voice_vw"]] },
		{ key: "narrow", label: "editor.ysf.narrow", type: "bool", advanced: true },
	],
	validate(c, p, problem) {
		checkCall(p.dest, "dest", CALL_LEN, problem);
		for (const k of ["txDgid", "rxDgid"]) {
			const v = Number(p[k] ?? 0);
			if (!Number.isInteger(v) || v < 0 || v > DGID_MAX) problem.err(k, "ysf_dgid_range", { max: DGID_MAX });
		}
	},
	settings: {
		key: "ysf",
		decode: decodeStation,
		encode: encodeStation,
		fields: [
			{ key: "call", label: "editor.ysf.call", type: "text", help: "editor.ysf.call_help" },
			{ key: "radioId", label: "editor.ysf.radio_id", type: "text", help: "editor.ysf.radio_id_help" },
		],
		validate(s, problem) {
			checkCall(s.call, "call", CALL_LEN, problem);
			checkCall(s.radioId, "radioId", RADIO_ID_LEN, problem);
		},
	},
};
