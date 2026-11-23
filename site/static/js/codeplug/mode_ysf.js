// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The YSF (C4FM) mode module of the Codeplug Editor (docs/spec/ysf-codeplug.md): kind 103 (0x67) on the
// channel-mode layer (channelmodes.js). Bytes to fields and back, and the fields the pages show, as data. YSF names
// its caller and us by callsign: no contacts. The radio ID is never in the codeplug: every radio makes its own from
// its chip (docs/spec/ysf.md 6), so this module has no field for it.

import { u8, grow } from "./modebytes.js";

export const DGID_MAX = 99, CALL_LEN = 10;
const CALL_RE = /^[0-9A-Z/-]{0,10}$/;

// --- Channel (2) ---

export function decodeChan(b) {
	const tx = u8(b, 0), rx = u8(b, 1);
	return { txDgid: tx <= DGID_MAX ? tx : 0, rxDgid: rx <= DGID_MAX ? rx : 0 };
}

export function encodeChan(p, prev = null) {
	const v = grow(prev, 2);
	v[0] = p.txDgid >= 0 && p.txDgid <= DGID_MAX ? p.txDgid : 0;
	v[1] = p.rxDgid >= 0 && p.rxDgid <= DGID_MAX ? p.rxDgid : 0;
	return v;
}

// --- The radio-wide record (1): the callsign, "" = the radio name's ---

export function decodeRadio(b) {
	let s = "";
	for (let i = 0; i < CALL_LEN && b && i < b.length && b[i] !== 0; i++) s += String.fromCharCode(b[i]);
	s = s.replace(/ +$/, "");
	return { callsign: CALL_RE.test(s) ? s : "" };
}

export function encodeRadio(s, prev = null) {
	const call = String(s.callsign ?? "").trim().toUpperCase();
	if (!call) return null;
	const v = grow(prev, CALL_LEN);
	v.fill(0, 0, CALL_LEN);
	for (let i = 0; i < call.length && i < CALL_LEN; i++) v[i] = call.charCodeAt(i);
	return v;
}

const dgidField = (key, label, extra) => ({ key, label, type: "int", min: 0, max: DGID_MAX, ...extra });

export const ysfKind = {
	id: 103,
	feature: "YSF",
	systemType: null,
	contacts: false,
	ownFreqs: true,
	defaultTx: true,
	recordFields: [],
	flags: {},
	newParams: () => ({ txDgid: 0, rxDgid: 0 }),
	decode: decodeChan,
	encode: encodeChan,
	fields: () => [
		dgidField("txDgid", "editor.ysf.tx_dgid"),
		dgidField("rxDgid", "editor.ysf.rx_dgid", { help: "editor.ysf.dgid_any" }),
	],
	validate() {},
	settings: {
		key: "ysf",
		decode: decodeRadio,
		encode: encodeRadio,
		fields: [{ key: "callsign", label: "editor.ysf.callsign", type: "text", help: "editor.ysf.callsign_help" }],
		validate(s, problem) {
			const call = String(s?.callsign ?? "").trim().toUpperCase();
			if (call && !CALL_RE.test(call)) problem.err("callsign", "ysf_callsign");
		},
	},
};
