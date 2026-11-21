// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dPMR mode module of the Codeplug Editor (docs/spec/dpmr.md 6): kind 7 on the channel-mode layer
// (channelmodes.js) and its radio-wide record. An ID is the standard's 24 bits; most radios (the licence-free dPMR446
// ones) use a common ID, 1-255, which is the top 8 bits with sixteen ones after them, so the editor shows a common ID
// as its number and keeps a full address as a second, advanced field. No system type and no contacts.

import { u8, u24, put24, grow } from "./modebytes.js";

export const CHAN_LEN = 5, STATION_LEN = 3;
export const RX_MINE = 0x01;		// flags: play only calls to the called ID, to all and to us
// Flag bits 1-6 are reserved and kept as found; bit 7 is written 0, so the byte is never the erased 0xFF (6.1)
const KEPT = 0x7E;
export const COMMON_ALL = 255;
export const CC_AUTO = 0, CC_MAX = 32;
export const ID_MAX = 0xFFFFFF;

// A 24-bit ID as the editor holds it: the common ID when it is one (and the address 0), else 0 and the address
const isCommon = (id) => (id & 0xFFFF) === 0xFFFF;
export const splitId = (id) => (isCommon(id) ? { common: id >>> 16, address: 0 } : { common: 0, address: id });
export const joinId = (common, address) => (Number(common) > 0 ? ((Number(common) << 16) | 0xFFFF) >>> 0 : Number(address) >>> 0);

// --- Channel (6.1) ---

export function decodeChan(b) {
	let called = u24(b, 0);
	if (called === 0) called = joinId(COMMON_ALL, 0);	// common ID 0 is reserved: a zeroed block calls all
	const cc = u8(b, 3);
	const f = u8(b, 4);
	const id = splitId(called);
	return {
		common: id.common,
		address: id.address,
		cc: cc <= CC_MAX ? cc : CC_AUTO,
		rxMine: f !== 0xFF && (f & RX_MINE) !== 0,
	};
}

export function encodeChan(p, prev = null) {
	const v = grow(prev, CHAN_LEN);
	put24(v, 0, joinId(p.common, p.address) & ID_MAX);
	const cc = Number(p.cc ?? CC_AUTO);
	v[3] = cc >= 0 && cc <= CC_MAX ? cc : CC_AUTO;
	const keep = v[4] === 0xFF ? 0 : v[4] & KEPT;
	v[4] = keep | (p.rxMine ? RX_MINE : 0);
	return v;
}

// --- Radio-wide record (6.2) ---

export function decodeStation(b) {
	const own = b && b.length >= STATION_LEN ? u24(b, 0) : 0;
	return own === 0 || own === ID_MAX ? { common: 0, address: 0 } : splitId(own);
}

export function encodeStation(s, prev = null) {
	const own = joinId(s.common, s.address);
	// none is the default: no record, unless one holds bytes a later firmware appended
	if (own === 0 && !(prev && prev.length > STATION_LEN)) return null;
	const v = grow(prev, STATION_LEN);
	put24(v, 0, own & ID_MAX);
	return v;
}

function checkId(p, problem) {
	const c = Number(p.common ?? 0), a = Number(p.address ?? 0);
	if (!Number.isInteger(c) || c < 0 || c > 255) problem.err("common", "dpmr_common_range");
	if (!Number.isInteger(a) || a < 0 || a > ID_MAX) problem.err("address", "dpmr_address_range", { max: ID_MAX });
}

const ccOptions = () => [[CC_AUTO, "editor.dpmr.cc_auto"],
	...[...Array(16).keys()].map((n) => [n + 1, `A${n + 1}`]), ...[...Array(16).keys()].map((n) => [n + 17, `B${n + 1}`])];

export const dpmrKind = {
	id: 7,
	feature: "DPMR",
	systemType: null,
	contacts: false,
	ownFreqs: true,
	// As every other kind's: whether the radio may transmit on a frequency is its band plan's to decide, not the
	// editor's
	defaultTx: true,
	recordFields: [],
	flags: {},
	newParams: () => ({ common: COMMON_ALL, address: 0, cc: CC_AUTO, rxMine: false }),
	decode: decodeChan,
	encode: encodeChan,
	fields: () => [
		{ key: "common", label: "editor.dpmr.common", type: "int", min: 0, max: 255, help: "editor.dpmr.common_help" },
		{ key: "address", label: "editor.dpmr.address", type: "int", min: 0, max: ID_MAX, help: "editor.dpmr.address_help", advanced: true },
		{ key: "cc", label: "editor.dpmr.cc", type: "select", help: "editor.dpmr.cc_help", options: ccOptions },
		{ key: "rxMine", label: "editor.dpmr.rx_mine", type: "bool", advanced: true },
	],
	validate(c, p, problem) {
		checkId(p, problem);
	},
	settings: {
		key: "dpmr",
		decode: decodeStation,
		encode: encodeStation,
		fields: [
			{ key: "common", label: "editor.dpmr.own_common", type: "int", min: 0, max: 255, help: "editor.dpmr.own_common_help" },
			{ key: "address", label: "editor.dpmr.own_address", type: "int", min: 0, max: ID_MAX, help: "editor.dpmr.address_help" },
		],
		validate(s, problem) {
			checkId(s, problem);
		},
	},
};
