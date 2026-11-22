// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The D-STAR DV mode module of the Codeplug Editor (docs/spec/dstar-codeplug.md): kind 5 on the channel-mode layer
// (channelmodes.js) and its radio-wide record. The stored fields are on-air text (ASCII, space padded); the model holds
// them as strings without the trailing spaces, cleaned the way the firmware reads them (1), so what the editor shows is
// what the radio sends. No system type and no contacts: a D-STAR call is addressed by the UR field.

import { u8, grow } from "./modebytes.js";

export const CALL_LEN = 8, SUFFIX_LEN = 4, MSG_LEN = 20;
export const CHAN_LEN = 25, STATION_LEN = 32;
export const VIA_REPEATER = 0x01;
export const UR_CQ = "CQCQCQ";

// The characters a callsign field keeps (A-Z 0-9 space /), and a message's (printable ASCII)
const CALL_OK = /^[A-Z0-9 /]$/;
const callChar = (c) => {
	const ch = String.fromCharCode(c >= 0x61 && c <= 0x7A ? c - 0x20 : c);
	return CALL_OK.test(ch) ? ch : " ";
};
const msgChar = (c) => (c >= 0x20 && c <= 0x7E ? String.fromCharCode(c) : " ");

// n bytes of b from o as text through clean (bytes past the record's end read as spaces), trailing spaces dropped
function readText(b, o, n, clean) {
	let s = "";
	for (let i = 0; i < n; i++) s += clean(u8(b, o + i));
	return s.trimEnd();
}

// text written at o, cut or padded with spaces to n
function putText(v, o, n, text, clean) {
	const s = String(text ?? "");
	for (let i = 0; i < n; i++) v[o + i] = i < s.length ? clean(s.charCodeAt(i)).charCodeAt(0) : 0x20;
}

// --- Channel (2.1) ---

export function decodeChan(b) {
	const ur = readText(b, 0, CALL_LEN, callChar);
	const f = u8(b, 24);
	return {
		ur: ur.trim() === "" ? UR_CQ : ur,
		rpt1: readText(b, 8, CALL_LEN, callChar),
		rpt2: readText(b, 16, CALL_LEN, callChar),
		viaRepeater: f !== 0xFF && (f & VIA_REPEATER) !== 0,
	};
}

export function encodeChan(p, prev = null) {
	const v = grow(prev, CHAN_LEN);
	putText(v, 0, CALL_LEN, p.ur, callChar);
	putText(v, 8, CALL_LEN, p.rpt1, callChar);
	putText(v, 16, CALL_LEN, p.rpt2, callChar);
	// an erased flags byte read as 0: writing it starts the reserved bits at 0 too
	const keep = v[24] === 0xFF ? 0 : v[24] & ~VIA_REPEATER;
	v[24] = keep | (p.viaRepeater ? VIA_REPEATER : 0);
	return v;
}

// --- Radio-wide record (3) ---

export function decodeStation(b) {
	return {
		my: readText(b, 0, CALL_LEN, callChar),
		suffix: readText(b, 8, SUFFIX_LEN, callChar),
		msg: readText(b, 12, MSG_LEN, msgChar),
	};
}

export function encodeStation(s, prev = null) {
	const blank = !s.my?.trim() && !s.suffix?.trim() && !s.msg?.trim();
	// all blank is the default: no record, unless one holds bytes a later firmware appended
	if (blank && !(prev && prev.length > STATION_LEN)) return null;
	const v = grow(prev, STATION_LEN);
	putText(v, 0, CALL_LEN, s.my, callChar);
	putText(v, 8, SUFFIX_LEN, s.suffix, callChar);
	putText(v, 12, MSG_LEN, s.msg, msgChar);
	return v;
}

// A callsign field's problems: too long, or characters the radio would send as spaces. Lower case is fine: it is
// stored upper-cased
function checkCall(v, key, max, problem) {
	const s = String(v ?? "");
	if (s.length > max) problem.err(key, "dstar_call_long", { max });
	if (!/^[A-Za-z0-9 /]*$/.test(s)) problem.err(key, "dstar_call_chars");
}

const callField = (key, label, help) => ({ key, label, type: "text", ...(help ? { help } : {}) });

export const dstarKind = {
	id: 5,
	feature: "DSTAR",
	systemType: null,
	contacts: false,
	ownFreqs: true,
	defaultTx: true,	// an amateur mode: a new channel may transmit; the base record is still the stand-in
	recordFields: [],
	flags: {},
	newParams: () => ({ ur: UR_CQ, rpt1: "", rpt2: "", viaRepeater: false }),
	decode: decodeChan,
	encode: encodeChan,
	fields: () => [
		callField("ur", "editor.dstar.ur", "editor.dstar.ur_help"),
		callField("rpt1", "editor.dstar.rpt1", "editor.dstar.rpt1_help"),
		callField("rpt2", "editor.dstar.rpt2", "editor.dstar.rpt2_help"),
		{ key: "viaRepeater", label: "editor.dstar.via", type: "bool" },
	],
	validate(c, p, problem) {
		for (const k of ["ur", "rpt1", "rpt2"]) checkCall(p[k], k, CALL_LEN, problem);
		if (p.viaRepeater && !String(p.rpt1 ?? "").trim()) problem.warn("rpt1", "dstar_rpt_blank");
	},
	settings: {
		key: "dstar",
		decode: decodeStation,
		encode: encodeStation,
		fields: [
			callField("my", "editor.dstar.my", "editor.dstar.my_help"),
			callField("suffix", "editor.dstar.suffix"),
			{ key: "msg", label: "editor.dstar.msg", type: "text", help: "editor.dstar.msg_help" },
		],
		validate(s, problem) {
			checkCall(s.my, "my", CALL_LEN, problem);
			checkCall(s.suffix, "suffix", SUFFIX_LEN, problem);
			const m = String(s.msg ?? "");
			if (m.length > MSG_LEN) problem.err("msg", "dstar_msg_long", { max: MSG_LEN });
			if (!/^[\x20-\x7E]*$/.test(m)) problem.err("msg", "dstar_msg_chars");
		},
	},
};
