// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mode module of the Codeplug Editor (docs/spec/dmr-trunking-codeplug.md): kind 1 and system type 1
// on the channel-mode layer (channelmodes.js). Bytes to fields and back; the fields the pages show are declared here
// as data (modes.js says how) and drawn by the editor's generic views.

import { u8, u16, u32, s8, put16, put32, grow, hex, unhex } from "./modebytes.js";
import { ID_MAX } from "./kv.js";

export const T3_BLOCK = 39, T3_SITES = 8, T3_LCN = 4094, T3_CTRL = 4;

// The network model's NET and site bits: tiny, small, large, huge
export const NET_BITS = [9, 7, 4, 2];
export const netMax = (model) => 2 ** NET_BITS[model & 3] - 1;
export const siteMax = (model) => 2 ** (12 - NET_BITS[model & 3]) - 1;

// The options word (1): the bits the editor knows, by field. Others are kept as stored
export const OPTS = {
	allCalls: 0x0002, emergencyAny: 0x0004, followAll: 0x0008, learnSystem: 0x0010, privateCalls: 0x0020,
	registerTalkgroups: 0x0040, acceptAls: 0x0080, acceptKill: 0x0100, noRoaming: 0x0200, acceptStun: 0x0400,
	ipGateway: 0x1000,
};
const OPT_MASK = Object.values(OPTS).reduce((a, b) => a | b, 0);

export function newT3System() {
	return {
		model: 0, net: 0, category: "a", colorCode: 1, ...Object.fromEntries(Object.keys(OPTS).map((k) => [k, k === "privateCalls"])),
		fallbackHold: 0, planBaseHz: 0, planFirstLcn: 0, planSep: 0, planTxLow: false, planNoLearn: false, planSplitHz: 0,
		leaveDbm: 0, powerSave: 0, sites: [],
	};
}

function decodeBlock(v) {
	const opts = u16(v, 5);
	const s = {
		model: u8(v, 0) & 3, net: u16(v, 1), category: u8(v, 3) === 2 ? "b" : "a", colorCode: u8(v, 4) & 0x0F,
		fallbackHold: u8(v, 7), planBaseHz: u32(v, 8), planFirstLcn: u16(v, 12), planSep: u8(v, 14) & 7,
		planTxLow: (u8(v, 15) & 1) !== 0, planNoLearn: (u8(v, 15) & 2) !== 0, planSplitHz: u32(v, 16), leaveDbm: s8(v, 20),
		powerSave: u8(v, 21) <= 7 ? u8(v, 21) : 0, sites: [],
	};
	for (const [k, bit] of Object.entries(OPTS)) s[k] = (opts & bit) !== 0;
	for (let i = 0; i < Math.min(u8(v, 22), T3_SITES); i++) s.sites.push(u16(v, 23 + 2 * i));
	return s;
}

function encodeBlock(s, prev) {
	const v = grow(prev, T3_BLOCK);
	v[0] = (v[0] & ~3) | (s.model & 3);
	put16(v, 1, s.net);
	v[3] = s.category === "b" ? 2 : 1;
	v[4] = s.colorCode & 0x0F;
	put16(v, 5, (u16(v, 5) & ~OPT_MASK) | Object.entries(OPTS).reduce((a, [k, bit]) => a | (s[k] ? bit : 0), 0));
	v[7] = s.fallbackHold & 0xFF;
	put32(v, 8, s.planBaseHz || 0);
	put16(v, 12, s.planFirstLcn || 0);
	v[14] = s.planSep & 7;
	v[15] = (v[15] & ~3) | (s.planTxLow ? 1 : 0) | (s.planNoLearn ? 2 : 0);
	put32(v, 16, s.planSplitHz || 0);
	v[20] = s.leaveDbm & 0xFF;
	v[21] = s.powerSave & 7;
	const sites = s.sites.slice(0, T3_SITES);
	v[22] = sites.length;
	v.fill(0, 23, 39);
	sites.forEach((x, i) => put16(v, 23 + 2 * i, x));
	return v;
}

// The key (3): K, 16 octets, then the PSN (3, most significant first; 0 = none)
function decodeSecret(v) {
	if (!v || v.length < 16) return null;
	return { key: hex(v.subarray(0, 16)), psn: v.length >= 19 ? (v[16] << 16) | (v[17] << 8) | v[18] : 0 };
}
function encodeSecret(k, prev) {
	const key = unhex(k.key);
	// no PSN: 16 octets, nothing after the key
	const v = k.psn ? grow(prev && prev.length >= 19 ? prev : null, 19) : new Uint8Array(16);
	v.set(key, 0);
	if (k.psn) v.set([(k.psn >>> 16) & 0xFF, (k.psn >>> 8) & 0xFF, k.psn & 0xFF], 16);
	return v;
}
export const validKey = (k) => /^[0-9A-Fa-f]{32}$/.test(String(k ?? "").replace(/[\s:-]/g, ""));

const range = (lo, hi) => ({ type: "int", min: lo, max: hi });
// Each option's section in the system form, and its help (editor.t3.opt.<key>_help) where it has one
const OPT_SECTION = { learnSystem: "identity", allCalls: "calls", emergencyAny: "calls", followAll: "calls", privateCalls: "calls",
	registerTalkgroups: "radio", ipGateway: "radio", acceptAls: "commands", acceptKill: "commands", acceptStun: "commands",
	noRoaming: "roaming" };
const OPT_HELP = new Set(["learnSystem", "allCalls", "followAll", "registerTalkgroups", "ipGateway", "acceptAls", "acceptKill",
	"acceptStun", "noRoaming"]);
const optionFields = (section) => Object.keys(OPTS).filter((k) => OPT_SECTION[k] === section).map((k) =>
	({ key: k, label: `editor.t3.opt.${k}`, type: "bool", section, help: OPT_HELP.has(k) ? `editor.t3.opt.${k}_help` : undefined }));

// The recommended ham system (docs/spec/ham-trunking.md 2): one network for every ham site, Huge NET 1, category A,
// color code 1, registration on, no key. A site's number makes its control channel's LCN, 4 × site + 1, so no two
// sites' channel numbers clash (sites 0-1022: four carriers each fit under LCN 4094)
export const HAM_T3 = { model: 3, net: 1, category: "a", colorCode: 1, siteMax: 1022, lcn: (site, carrier = 1) => 4 * site + carrier };

export const t3System = {
	id: 1,
	idLen: 4,
	defaultTx: true,
	ham: {
		name: "Ham Tier III",
		tx: true,
		fields: () => ({ ...newT3System(), model: HAM_T3.model, net: HAM_T3.net, category: HAM_T3.category, colorCode: HAM_T3.colorCode, registerTalkgroups: true }),
		asks: [{ key: "site", label: "editor.t3.ham_site", type: "int", min: 0, max: HAM_T3.siteMax, help: "editor.t3.ham_site_help" }],
		entries: (a) => [{ cls: 0, group: 0xFF, flags: 0, control: true, chan: HAM_T3.lcn(a.site), rxHz: a.rxHz, txHz: a.txHz, cc: null, raw: null }],
	},
	newFields: newT3System,
	decode: decodeBlock,
	encode: encodeBlock,
	// the header's flag bit 0: the system authenticates
	needsKeyField: "authenticates",
	identity: (s) => `${["tiny", "small", "large", "huge"][s.model]} · NET ${s.net} · ${s.category.toUpperCase()}`,
	// The system form's sections, in order (the editor adds its own fields to identity, radio, fallback and sharing)
	sections: [
		{ id: "identity", intro: "editor.t3.intro.identity" },
		{ id: "radio" },
		{ id: "calls" },
		{ id: "fallback" },
		{ id: "roaming", advanced: true },
		{ id: "commands", intro: "editor.t3.intro.commands", advanced: true },
		{ id: "plan", intro: "editor.t3.intro.plan", advanced: true },
		{ id: "sharing" },
	],
	fields: () => [
		{ key: "model", label: "editor.t3.model", type: "select", options: () => [0, 1, 2, 3].map((n) => [n, `editor.t3.model${n}`]), section: "identity", help: "editor.t3.model_help" },
		{ key: "net", label: "editor.t3.net", type: "int", min: 0, max: (s) => netMax(s.model), section: "identity", help: "editor.t3.net_help" },
		{ key: "category", label: "editor.t3.category", type: "select", options: () => [["a", "A"], ["b", "B"]], section: "identity", help: "editor.t3.category_help" },
		{ key: "colorCode", label: "editor.t3.color_code", type: "select", options: () => [...Array(16).keys()].map((n) => [n, String(n)]), section: "identity", help: "editor.t3.color_code_help" },
		...optionFields("identity"),
		...optionFields("radio"),
		{ key: "powerSave", label: "editor.t3.power_save", type: "select", options: () => [...Array(8).keys()].map((n) => [n, n ? `1:${2 ** n}` : "editor.general.off"]), section: "radio", help: "editor.t3.power_save_help" },
		...optionFields("calls"),
		{ key: "sites", label: "editor.t3.sites", help: "editor.t3.sites_help", type: "list", min: 0, max: (s) => siteMax(s.model), count: T3_SITES, section: "roaming" },
		...optionFields("roaming"),
		{ key: "leaveDbm", label: "editor.t3.leave_dbm", help: "editor.t3.leave_dbm_help", ...range(-128, 127), section: "roaming" },
		{ key: "fallbackHold", label: "editor.t3.fallback_hold", help: "editor.t3.fallback_hold_help", ...range(0, 255), section: "fallback" },
		...optionFields("commands"),
		{ key: "planBaseHz", label: "editor.t3.plan_base", type: "freq", zero: true, section: "plan", help: "editor.t3.plan_base_help" },
		{ key: "planFirstLcn", label: "editor.t3.plan_first", ...range(0, T3_LCN), section: "plan" },
		{ key: "planSep", label: "editor.t3.plan_sep", type: "select", options: () => [5, 6.25, 10, 12.5, 15, 20, 25, 30].map((k, i) => [i, `${k} kHz`]), section: "plan" },
		{ key: "planSplitHz", label: "editor.t3.plan_split", type: "freq", zero: true, section: "plan" },
		{ key: "planTxLow", label: "editor.t3.plan_tx_low", type: "bool", section: "plan" },
		{ key: "planNoLearn", label: "editor.t3.plan_no_learn", type: "bool", section: "plan", help: "editor.t3.plan_no_learn_help" },
	],
	classes: [
		{
			cls: 0, name: "channels", group: false, control: true,
			newEntry: () => ({ control: true, chan: 1, rxHz: 0, txHz: 0, cc: null }),
			decode: (body) => ({ cc: u8(body, 10) === 0xFF || body.length < 11 ? null : u8(body, 10) & 0x0F }),
			encode: (e, body) => {
				const v = grow(body, 11);
				v[10] = e.cc == null ? 0xFF : e.cc & 0x0F;
				return v;
			},
			fields: [
				{ key: "control", label: "editor.modes.control", type: "bool" },
				{ key: "chan", label: "editor.t3.lcn", type: "int", min: 1, max: T3_LCN },
				{ key: "rxHz", label: "editor.col.rx", type: "freq" },
				{ key: "txHz", label: "editor.col.tx", type: "freq" },
				{ key: "cc", label: "editor.col.cc", type: "select", options: () => [[null, "editor.t3.cc_system"], ...[...Array(16).keys()].map((n) => [n, String(n)])] },
			],
		},
	],
	secret: {
		decode: decodeSecret,
		encode: encodeSecret,
		fields: [
			{ key: "key", label: "editor.t3.key", type: "hexbytes", bytes: 16, help: "editor.t3.key_help" },
			{ key: "psn", label: "editor.t3.psn", type: "int", min: 0, max: 0xFFFFFF, help: "editor.t3.psn_help" },
		],
		valid: (k) => validKey(k.key) && Number.isInteger(k.psn) && k.psn >= 0 && k.psn <= 0xFFFFFF,
		empty: () => ({ key: "", psn: 0 }),
	},
	// the records of the type's own family (keys 0x0800 and 0x0900 + n: the poll setting and the supplementary data),
	// kept as stored and removed with the system
	ownedKeys: (n) => [0x0800 + n, 0x0900 + n],
	validate(s, f, chans, problem) {
		if (!(Number.isInteger(f.net) && f.net >= 0 && f.net <= netMax(f.model))) problem.err("net", "t3_net_range", { max: netMax(f.model) });
		if (f.sites.length > T3_SITES) problem.err("sites", "too_many", { max: T3_SITES, count: f.sites.length });
		if (f.sites.some((x) => !Number.isInteger(x) || x < 0 || x > siteMax(f.model))) problem.err("sites", "range", { min: 0, max: siteMax(f.model) });
		const ctrl = chans.filter((c) => c.control).length;
		if (ctrl > T3_CTRL) problem.warn("entries", "t3_ctrl_extra", { max: T3_CTRL });
		if (s.ownId > ID_MAX) problem.err("ownId", "range", { min: 0, max: ID_MAX });
	},
};

export const t3Kind = {
	id: 1,
	systemType: 1,
	contacts: true,		// the talk target is the record's contact, the groups followed its RX group list
	recordFields: ["contact", "rxList", "acceptBadCrc"],
	ownFreqs: false,	// the record holds the system's first control channel
	defaultTx: true,	// a site grants every transmission
	fallback: true,
	flags: { noFallback: 0x10 },
	newParams: () => ({}),
	decode: () => ({}),
	encode: (p, prev) => prev ?? new Uint8Array(0),
	fields: () => [],
};
