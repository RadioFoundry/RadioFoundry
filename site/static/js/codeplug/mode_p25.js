// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 mode modules of the Codeplug Editor (docs/spec/p25-codeplug.md): kind 2 (conventional), kind 3 (trunked)
// and system type 3 on the channel-mode layer (channelmodes.js). Bytes to fields and back, and the fields the pages
// show, as data. Talk targets are the contact list's: a group contact is a talk group, a private one a unit.

import { u8, u16, u24, u32, s8, put16, put24, put32, grow } from "./modebytes.js";

export const NAC_DEFAULT = 0x293, NAC_ANY = 0xF7E, NAC_REPEAT_ANY = 0xF7F, NAC_SET = 0x8000;
export const UNIT_MAX = 0xFFFFFE;
// Above this the editor warns: unit IDs this long are unusual (the standard's range is not checked here)
export const UNIT_WARN = 9999999;
export const WACN_ANY = 0xFFFFFF, SYSID_ANY = 0xFFFF, P25_UNKNOWN = 0xFF;

// A NAC field: its value when set, else dflt; a value the field can't hold reads as dflt too
const nacField = (f, dflt) => ((f & NAC_SET) && !(f & 0x7000) ? f & 0x0FFF : dflt);
const nacOut = (v, dflt) => (v === dflt ? 0 : NAC_SET | (v & 0x0FFF));

// A talk group is 16 bits: a group contact above that calls talk group 1 (2)
function tgRange(c, problem) {
	const tg = c.contact && typeof c.contact === "object" ? c.contact : null;
	if (tg && tg.type === "group" && tg.id > 0xFFFF) problem.warn("contact", "p25_tg_range");
}

// --- Conventional channel (2) ---

export function decodeConv(b) {
	const sq = u8(b, 0) & 3;
	let rx = nacField(u16(b, 1), NAC_DEFAULT);
	if (rx === NAC_REPEAT_ANY) rx = NAC_DEFAULT;
	const txDflt = rx === NAC_ANY ? NAC_DEFAULT : rx;
	let tx = nacField(u16(b, 3), txDflt);
	if (tx === NAC_ANY || tx === NAC_REPEAT_ANY) tx = txDflt;
	return { squelch: sq === 1 ? 1 : 0, rxNac: rx, txNac: tx };
}

export function encodeConv(p, prev = null) {
	const v = grow(prev, 5);
	v[0] = (v[0] & ~3) | (p.squelch === 1 ? 1 : 0);
	put16(v, 1, nacOut(p.rxNac, NAC_DEFAULT));
	put16(v, 3, nacOut(p.txNac, p.rxNac === NAC_ANY ? NAC_DEFAULT : p.rxNac));
	return v;
}

// The radio-wide record (1): {unitId: 0 = the radio's DMR ID}
export function decodeUnit(b) {
	const id = b && b.length >= 4 ? u32(b, 0) : 0;
	return { unitId: id >= 1 && id <= UNIT_MAX ? id : 0 };
}

const nacText = (v) => v.toString(16).toUpperCase().padStart(3, "0");
const nacFieldDef = (key, label) => ({ key, label, type: "hex", min: 0, max: 0xFFF, digits: 3, format: nacText });

export const p25Kind = {
	id: 2,
	feature: "P25",
	systemType: null,
	contacts: true,
	ownFreqs: true,
	defaultTx: true,
	recordFields: ["contact", "rxList", "dmrId", "dmo"],
	flags: {},
	newParams: () => ({ squelch: 0, rxNac: NAC_DEFAULT, txNac: NAC_DEFAULT }),
	decode: decodeConv,
	encode: encodeConv,
	fields: () => [
		{ key: "squelch", label: "editor.p25.squelch", type: "select", options: () => [[0, "editor.p25.squelch0"], [1, "editor.p25.squelch1"]] },
		// blank: the default NAC; the TX NAC's, the RX NAC
		{ ...nacFieldDef("rxNac", "editor.p25.rx_nac"), blank: NAC_DEFAULT, help: "editor.p25.nac_any" },
		{ ...nacFieldDef("txNac", "editor.p25.tx_nac"), blank: (p) => (p.rxNac === NAC_ANY ? NAC_DEFAULT : p.rxNac) },
	],
	validate(c, p, problem) {
		tgRange(c, problem);
		if (p.rxNac === NAC_REPEAT_ANY) problem.err("rxNac", "p25_nac_repeat");
		if (p.txNac === NAC_ANY || p.txNac === NAC_REPEAT_ANY) problem.err("txNac", "p25_nac_any_tx");
	},
	settings: {
		key: "p25",
		decode: decodeUnit,
		encode: (s, prev) => {
			if (!s.unitId) return null;
			const v = grow(prev, 4);
			put32(v, 0, s.unitId);
			return v;
		},
		fields: [{ key: "unitId", label: "editor.p25.unit_id", type: "int", min: 0, max: UNIT_MAX, blank: 0, help: "editor.p25.unit_id_help" }],
		validate(s, problem) {
			if (s.unitId > UNIT_WARN) problem.warn("unitId", "p25_unit_long", { max: UNIT_WARN });
		},
	},
};

// --- Trunked system (3) ---

export function decodeTsys(b) {
	const w = b && b.length >= 3 ? u24(b, 0) : 0;
	const s = u16(b, 3);
	return {
		wacn: w === WACN_ANY ? null : w & 0xFFFFF,
		sysid: s === SYSID_ANY ? null : s & 0x0FFF,
		ccNac: (() => {
			const n = nacField(u16(b, 5), NAC_ANY);
			return n === NAC_ANY ? null : n;
		})(),
		skipEnc: (u8(b, 7) & 1) !== 0,
		planOnly: (u8(b, 7) & 2) !== 0,
		noReg: (u8(b, 7) & 4) !== 0,
		holdS: u8(b, 8),
		roamDbm: s8(b, 9),
	};
}

export function encodeTsys(s, prev = null) {
	const v = grow(prev, 10);
	put24(v, 0, s.wacn == null ? WACN_ANY : s.wacn & 0xFFFFF);
	put16(v, 3, s.sysid == null ? SYSID_ANY : s.sysid & 0x0FFF);
	put16(v, 5, s.ccNac == null ? 0 : NAC_SET | (s.ccNac & 0x0FFF));
	v[7] = (v[7] & ~7) | (s.skipEnc ? 1 : 0) | (s.planOnly ? 2 : 0) | (s.noReg ? 4 : 0);
	v[8] = s.holdS & 0xFF;
	v[9] = s.roamDbm & 0xFF;
	return v;
}

const optHex = (key, label, max, digits) => ({ key, label, type: "hex", min: 0, max, digits, any: true, format: (v) => v.toString(16).toUpperCase().padStart(digits, "0") });
const byteOrUnknown = (key, label) => ({ key, label, type: "int", min: 0, max: 254, any: true });

export const p25System = {
	id: 3,
	feature: "P25_TRUNK",
	idLen: 5,
	defaultTx: false,	// a new system is listen only until the user turns transmit on
	newFields: () => ({ wacn: null, sysid: null, ccNac: null, skipEnc: false, planOnly: false, noReg: false, holdS: 0, roamDbm: 0 }),
	decode: decodeTsys,
	encode: encodeTsys,
	identity: (s) => `WACN ${s.wacn == null ? "*" : s.wacn.toString(16).toUpperCase()} · SYS ${s.sysid == null ? "*" : s.sysid.toString(16).toUpperCase()}`,
	sections: [
		{ id: "identity", intro: "editor.p25.intro.identity" },
		{ id: "radio" },
		{ id: "calls" },
		{ id: "fallback" },
		{ id: "roaming", advanced: true },
		{ id: "sharing" },
	],
	// the fallback channel is stored, not used yet (p25-trunking.md)
	fallbackHelp: "editor.p25.fallback_help",
	// a third transmit choice in the system form: transmit without registering (the field set while the radio transmits)
	noRegField: "noReg",
	fields: () => [
		{ ...optHex("wacn", "editor.p25.wacn", 0xFFFFF, 5), help: "editor.p25.wacn_help", section: "identity" },
		{ ...optHex("sysid", "editor.p25.sysid", 0xFFF, 3), help: "editor.p25.any_help", section: "identity" },
		{ ...optHex("ccNac", "editor.p25.cc_nac", 0xFFF, 3), help: "editor.p25.any_help", section: "identity" },
		{ key: "skipEnc", label: "editor.p25.skip_enc", type: "bool", section: "calls", help: "editor.p25.skip_enc_help" },
		{ key: "holdS", label: "editor.p25.hold", type: "int", min: 0, max: 255, section: "calls", help: "editor.p25.hold_help" },
		{ key: "planOnly", label: "editor.p25.plan_only", type: "bool", section: "roaming", help: "editor.p25.plan_only_help" },
		{ key: "roamDbm", label: "editor.p25.roam_dbm", type: "int", min: -128, max: 127, section: "roaming", help: "editor.p25.roam_dbm_help" },
	],
	classes: [
		{
			cls: 1, name: "sites", group: true, groupLabel: "editor.p25.site_no", groupMax: 254,
			newEntry: (list) => ({ group: Math.min(254, list.reduce((n, e) => Math.max(n, e.group + 1), 0)), rfss: null, site: null }),
			decode: (body) => ({ rfss: u8(body, 0) === P25_UNKNOWN || !body.length ? null : body[0], site: u8(body, 1) === P25_UNKNOWN || body.length < 2 ? null : body[1] }),
			encode: (e, body) => {
				const v = grow(body, 2);
				v[0] = e.rfss ?? P25_UNKNOWN;
				v[1] = e.site ?? P25_UNKNOWN;
				return v;
			},
			fields: [{ ...byteOrUnknown("rfss", "editor.p25.rfss"), help: "editor.p25.unknown_help" }, { ...byteOrUnknown("site", "editor.p25.site"), help: "editor.p25.unknown_help" }],
		},
		{
			cls: 0, name: "control", group: true, groupLabel: "editor.p25.site_no", groupMax: 254, control: true,
			newEntry: (list) => ({ group: list[list.length - 1]?.group ?? 0, control: true, iden: 0, chanNo: 0, rxHz: 0, txHz: 0 }),
			decode: (body) => ({ iden: u16(body, 0) >> 12, chanNo: u16(body, 0) & 0x0FFF }),
			encode: (e, body) => {
				const v = grow(body, 10);
				put16(v, 0, ((e.iden & 15) << 12) | (e.chanNo & 0x0FFF));
				return v;
			},
			fields: [
				{ key: "rxHz", label: "editor.col.rx", type: "freq" },
				{ key: "txHz", label: "editor.col.tx", type: "freq", zero: true },
				{ key: "iden", label: "editor.p25.iden", type: "int", min: 0, max: 15 },
				{ key: "chanNo", label: "editor.p25.chan_no", type: "int", min: 0, max: 4095 },
			],
		},
		{
			cls: 2, name: "plan", group: true, groupLabel: "editor.p25.iden", groupMax: 15,
			newEntry: (list) => ({ group: Math.min(15, list.reduce((n, e) => Math.max(n, e.group + 1), 0)), baseHz: 0, spacingHz: 12500, txOffsetHz: 0, bwHz: 12500, tdma: false, slots: 2 }),
			decode: (b) => ({
				baseHz: u32(b, 0), spacingHz: u32(b, 4), txOffsetHz: u32(b, 8) | 0, bwHz: u32(b, 12), tdma: u8(b, 16) === 1,
				slots: u8(b, 16) === 1 ? u8(b, 17) || 2 : 2,
			}),
			encode: (e, body) => {
				const v = grow(body, 18);
				put32(v, 0, e.baseHz);
				put32(v, 4, e.spacingHz);
				put32(v, 8, e.txOffsetHz >>> 0);
				put32(v, 12, e.bwHz);
				v[16] = e.tdma ? 1 : 0;
				v[17] = e.tdma ? e.slots & 0xFF : 0;
				return v;
			},
			fields: [
				{ key: "baseHz", label: "editor.p25.base", type: "freq" },
				{ key: "spacingHz", label: "editor.p25.spacing", type: "int", min: 1, max: 1000000 },
				{ key: "txOffsetHz", label: "editor.p25.tx_offset", type: "int", min: -2147483648, max: 2147483647 },
				{ key: "bwHz", label: "editor.p25.bw", type: "int", min: 0, max: 1000000 },
				{ key: "tdma", label: "editor.p25.tdma", type: "bool" },
				{ key: "slots", label: "editor.p25.slots", type: "int", min: 1, max: 8 },
			],
		},
	],
	secret: null,
	ownedKeys: () => [],
	validate(s, f, entries, problem) {
		const sites = entries.filter((e) => e.cls === 1);
		const groups = new Set(sites.map((e) => e.group));
		if (groups.size !== sites.length) problem.err("entries", "p25_site_twice");
		const plan = entries.filter((e) => e.cls === 2);
		if (new Set(plan.map((e) => e.group)).size !== plan.length) problem.err("entries", "p25_iden_twice");
		for (const e of entries.filter((x) => x.cls === 0 && x.control)) {
			if (sites.length && !groups.has(e.group)) problem.warn("entries", "p25_ctl_no_site", { site: e.group });
		}
	},
};

export const p25TrunkKind = {
	id: 3,
	feature: "P25_TRUNK",
	systemType: 3,
	contacts: true,
	ownFreqs: false,
	defaultTx: false,
	fallback: true,
	recordFields: ["contact", "rxList"],
	flags: { noFallback: 0x10 },
	newParams: () => ({ followAll: false }),
	decode: (b) => ({ followAll: (u8(b, 0) & 1) !== 0 }),
	encode: (p, prev) => {
		const v = grow(prev, 1);
		v[0] = (v[0] & ~1) | (p.followAll ? 1 : 0);
		return v;
	},
	fields: () => [{ key: "followAll", label: "editor.p25.follow_all", type: "bool" }],
	validate: (c, p, problem) => tgRange(c, problem),
};
