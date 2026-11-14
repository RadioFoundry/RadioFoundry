// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III trunked systems in the codeplug settings area (docs/spec/dmr-trunking.md 12): the system records, their
// channels, their keys and the tags that make a codeplug channel a trunked one. Records here are plain objects with
// numbers; model.js turns the references into objects. The C reader is core/radio/radio_trunk_cp.c.

import { putName, getName } from "./fields.js";
import { ID_MAX } from "./kv.js";

export const T3_KEY = { system: 0x0100, chan: 0x0200, key: 0x0300, tag: 0x0400, poll: 0x0800, supl: 0x0900 };
export const T3_LIMITS = {
	systems: 8,		// RADIO_T3_SYSTEMS
	chanKeys: 256,		// keys 0x0200-0x02FF
	ctrl: 4,		// T3_CTRL_MAX: the control channels a system hunts
	chans: 8,		// RADIO_T3_CHANS: the channels a system keeps
	sites: 8,		// T3_SITES_MAX
	name: 16,
	lcn: 4094,
};
export const SYS_LEN = 64, CHAN_LEN = 13, KEY_LEN = 16, PSN_LEN = 3, TAG_LEN = 8;

// MODEL's NET and SITE bits (section 4): tiny, small, large, huge
export const NET_BITS = [9, 7, 4, 2];
export const netMax = (model) => 2 ** NET_BITS[model & 3] - 1;
export const siteMax = (model) => 2 ** (12 - NET_BITS[model & 3]) - 1;

// Separation codes 0-7 of the fixed plan (section 5), in Hz
export const SEP_HZ = [5000, 6250, 10000, 12500, 15000, 20000, 25000, 30000];

// The options word (12.1): the codeplug's bits, by the model's field. Bits not named here are kept as stored
export const OPTS = {
	listenOnly: 0x0001, allCalls: 0x0002, emergencyAny: 0x0004, privateCalls: 0x0020, registerTalkgroups: 0x0040,
	acceptAls: 0x0080, acceptKill: 0x0100, noRoaming: 0x0200, acceptStun: 0x0400, authenticates: 0x0800,
	ipGateway: 0x1000,
};
const OPT_MASK = Object.values(OPTS).reduce((a, b) => a | b, 0);
const CAT = { a: 1, b: 2 };

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const put16 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = (v >>> 8) & 0xFF;
};
const put32 = (b, o, v) => {
	for (let i = 0; i < 4; i++) b[o + i] = Math.floor(v / 2 ** (8 * i)) & 0xFF;
};

// A channel's name check (12.4): FNV-1a 32 over the name as the channel record stores it (16 octets, up to the first
// 0x00 or 0xFF, then NULs), the halves XORed
export function nameCheck(name) {
	const r = new Uint8Array(16);
	putName(r, 0, 16, name);
	let h = 0x811C9DC5, end = false;
	for (let i = 0; i < 16; i++) {
		const c = end || r[i] === 0x00 || r[i] === 0xFF ? 0 : r[i];
		end = end || c === 0;
		h = Math.imul(h ^ c, 0x01000193) >>> 0;
	}
	return (h ^ (h >>> 16)) & 0xFFFF;
}

// --- Systems (12.1) ---

// A system as the model holds it, but for the fallback channel: its number and check here
export function newSystem(fields = {}) {
	return {
		number: null, name: "", model: 0, net: 0, category: "a", colorCode: 1,
		listenOnly: false, allCalls: false, emergencyAny: false, privateCalls: true, registerTalkgroups: false,
		acceptAls: false, acceptKill: false, noRoaming: false, acceptStun: false, authenticates: false, ipGateway: false,
		ownId: 0, sharing: 0, fallbackHold: 0, fallback: null,
		planBaseHz: 0, planFirstLcn: 0, planSep: 0, planTxLow: false, planNoLearn: false, planSplitHz: 0,
		leaveDbm: 0, powerSave: 0, sites: [], key: "", psn: 0, ...POLL_DEFAULT, ...SUPL_DEFAULT, ...fields,
	};
}

// A record of at least SYS_LEN octets, or null (a shorter one is absent to the radio)
export function decodeSystem(v) {
	if (!v || v.length < SYS_LEN) return null;
	const opts = u16(v, 21);
	const s = {
		name: getName(v, 0, 16),
		model: v[16] & 3,
		net: u16(v, 17),
		category: v[19] === 2 ? "b" : "a",
		colorCode: v[20] & 0x0F,
		ownId: u32(v, 23),
		sharing: v[27] <= 2 ? v[27] : 0,
		fallbackHold: v[28],
		fallback: u16(v, 29),
		fallbackCheck: u16(v, 62),
		planBaseHz: u32(v, 31),
		planFirstLcn: u16(v, 35),
		planSep: v[37] & 7,
		planTxLow: (v[38] & 1) !== 0,
		planNoLearn: (v[38] & 2) !== 0,
		planSplitHz: u32(v, 39),
		leaveDbm: v[43] >= 0x80 ? v[43] - 0x100 : v[43],
		powerSave: v[44] <= 7 ? v[44] : 0,
		sites: [],
	};
	for (const [k, bit] of Object.entries(OPTS)) s[k] = (opts & bit) !== 0;
	for (let i = 0; i < Math.min(v[45], T3_LIMITS.sites); i++) s.sites.push(u16(v, 46 + 2 * i));
	return s;
}

// The fields decodeSystem gives, compared by encodeTrunk to tell an edited system
export const SYSTEM_KEYS = ["name", "model", "net", "category", "colorCode", ...Object.keys(OPTS), "ownId", "sharing",
	"fallbackHold", "fallback", "fallbackCheck", "planBaseHz", "planFirstLcn", "planSep", "planTxLow", "planNoLearn",
	"planSplitHz", "leaveDbm", "powerSave", "sites"];

// s with fallback and fallbackCheck as numbers, written onto prev (a stored record, kept where no field covers it:
// the options bits not named, the bytes past SYS_LEN) or a new record
export function encodeSystem(s, prev = null) {
	const v = prev && prev.length >= SYS_LEN ? Uint8Array.from(prev) : new Uint8Array(SYS_LEN);
	const was = prev && prev.length >= SYS_LEN ? decodeSystem(prev) : null;
	// a name is written only when it changed: bytes after its end stay as stored
	if (!was || was.name !== s.name) {
		v.fill(0, 0, 16);
		putName(v, 0, 16, s.name);
		for (let i = 0; i < 16; i++) if (v[i] === 0xFF) v[i] = 0;
	}
	v[16] = (v[16] & ~3) | (s.model & 3);
	put16(v, 17, s.net);
	v[19] = CAT[s.category] || 1;
	v[20] = s.colorCode & 0x0F;
	let opts = u16(v, 21) & ~OPT_MASK;
	for (const [k, bit] of Object.entries(OPTS)) if (s[k]) opts |= bit;
	put16(v, 21, opts);
	put32(v, 23, s.ownId || 0);
	v[27] = s.sharing;
	v[28] = s.fallbackHold & 0xFF;
	put16(v, 29, s.fallback || 0);
	put32(v, 31, s.planBaseHz || 0);
	put16(v, 35, s.planFirstLcn || 0);
	v[37] = s.planSep & 7;
	v[38] = (v[38] & ~3) | (s.planTxLow ? 1 : 0) | (s.planNoLearn ? 2 : 0);
	put32(v, 39, s.planSplitHz || 0);
	v[43] = s.leaveDbm & 0xFF;
	v[44] = s.powerSave & 7;
	const sites = s.sites.slice(0, T3_LIMITS.sites);
	v[45] = sites.length;
	v.fill(0, 46, 62);
	sites.forEach((x, i) => put16(v, 46 + 2 * i, x));
	put16(v, 62, s.fallback ? s.fallbackCheck : 0);
	return v;
}

// --- System channels (12.2) ---

// {system: n (0-based), control, lcn, rxHz, txHz, cc: 0-15, or null for the system's}; null when too short
export function decodeChan(v) {
	if (!v || v.length < CHAN_LEN) return null;
	return { system: v[0], control: (v[1] & 1) !== 0, lcn: u16(v, 2), rxHz: u32(v, 4), txHz: u32(v, 8), cc: v[12] === 0xFF ? null : v[12] };
}

export function encodeChan(c) {
	const v = new Uint8Array(CHAN_LEN);
	v[0] = c.system;
	v[1] = c.control ? 1 : 0;
	put16(v, 2, c.lcn);
	put32(v, 4, c.rxHz);
	put32(v, 8, c.txHz);
	v[12] = c.cc == null ? 0xFF : c.cc & 0x0F;
	return v;
}

// --- Keys (12.3) ---

// {key: 32 hex digits, upper case, psn: 0 = none}, or null for no key (absent or shorter than 16 octets)
export function decodeKey(v) {
	if (!v || v.length < KEY_LEN) return null;
	const key = [...v.subarray(0, KEY_LEN)].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase();
	const psn = v.length >= KEY_LEN + PSN_LEN ? (v[16] << 16) | (v[17] << 8) | v[18] : 0;
	return { key, psn };
}

export const validKey = (k) => /^[0-9A-Fa-f]{32}$/.test(k);

// The value for a key and PSN: 16 octets, 19 with a PSN
export function encodeKey(key, psn = 0) {
	const v = new Uint8Array(psn ? KEY_LEN + PSN_LEN : KEY_LEN);
	for (let i = 0; i < KEY_LEN; i++) v[i] = parseInt(key.slice(2 * i, 2 * i + 2), 16);
	if (psn) v.set([(psn >>> 16) & 0xFF, (psn >>> 8) & 0xFF, psn & 0xFF], KEY_LEN);
	return v;
}

// --- Short data poll (12.7) ---

// UDT_Format (table 7.60) of the text formats the editor writes a reply in, and the octets a reply holds at most
export const POLL_FORMATS = { iso7: 3, iso8: 4, utf16: 7 };
export const POLL_MAX = 46;
// No record: ISO 8 bit, 4 blocks, no reply. pollReply null: a reply in a format the editor doesn't write (pollRaw)
export const POLL_DEFAULT = { pollFormat: POLL_FORMATS.iso8, pollBlocks: 4, pollReply: "", pollRaw: null };

// The poll fields of a record, or the defaults when it is absent or shorter than 2 octets
export function decodePoll(v) {
	if (!v || v.length < 2) return { ...POLL_DEFAULT };
	const format = v[0] & 0x0F, blocks = v[1] >= 1 && v[1] <= 4 ? v[1] : 4;
	const d = v.subarray(2, 2 + POLL_MAX);
	let reply = null;
	if (format === POLL_FORMATS.iso8) reply = String.fromCharCode(...[...d].slice(0, d.indexOf(0) < 0 ? d.length : d.indexOf(0)));
	else if (format === POLL_FORMATS.utf16) {
		reply = "";
		for (let i = 0; i + 1 < d.length; i += 2) {
			const u = (d[i] << 8) | d[i + 1];
			if (!u) break;
			reply += String.fromCharCode(u);
		}
	} else if (format === POLL_FORMATS.iso7) {
		reply = "";
		for (let pos = 0; pos + 7 <= d.length * 8; pos += 7) {
			let c = 0;
			for (let b = pos; b < pos + 7; b++) c = (c << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1);
			if (!c) break;
			reply += String.fromCharCode(c);
		}
	}
	return { pollFormat: format, pollBlocks: blocks, pollReply: reply, pollRaw: reply === null ? Uint8Array.from(d) : null };
}

// A reply's octets in a text format, or null when it doesn't fit (POLL_MAX) or has a character the format lacks
export function encodePollReply(format, text) {
	const cs = [...String(text ?? "")].map((c) => c.codePointAt(0));
	let d;
	if (format === POLL_FORMATS.iso8) {
		if (cs.some((c) => c > 0xFF)) return null;
		d = Uint8Array.from(cs);
	} else if (format === POLL_FORMATS.utf16) {
		if (cs.some((c) => c > 0xFFFF)) return null;
		d = new Uint8Array(cs.length * 2);
		cs.forEach((c, i) => d.set([c >> 8, c & 0xFF], 2 * i));
	} else if (format === POLL_FORMATS.iso7) {
		if (cs.some((c) => c > 0x7F || c === 0)) return null;
		d = new Uint8Array(Math.ceil((cs.length * 7) / 8));
		cs.forEach((c, i) => {
			for (let b = 0; b < 7; b++) if ((c >> (6 - b)) & 1) d[(i * 7 + b) >> 3] |= 0x80 >> ((i * 7 + b) & 7);
		});
	} else return null;
	return d.length <= POLL_MAX ? d : null;
}

// The record for a system's poll fields, or null when they are the defaults (no record needed) or don't encode
export function encodePoll(s) {
	const d = s.pollReply === null ? s.pollRaw ?? new Uint8Array(0) : encodePollReply(s.pollFormat, s.pollReply);
	if (!d) return null;
	if (!d.length && s.pollFormat === POLL_DEFAULT.pollFormat && s.pollBlocks === POLL_DEFAULT.pollBlocks) return null;
	const v = new Uint8Array(2 + d.length);
	v[0] = s.pollFormat & 0x0F;
	v[1] = s.pollBlocks;
	v.set(d, 2);
	return v;
}

// --- Supplementary data (12.8) ---

// No record: none offered. suplData null: data in a format the editor doesn't write (suplRaw)
export const SUPL_DEFAULT = { suplFormat: POLL_FORMATS.iso8, suplData: "", suplRaw: null };

// The supplementary data fields of a record (UDT_Format, then the octets), or the defaults when it has no octets
export function decodeSupl(v) {
	if (!v || v.length < 2) return { ...SUPL_DEFAULT };
	const p = decodePoll(Uint8Array.of(v[0], 4, ...v.subarray(1)));
	return { suplFormat: p.pollFormat, suplData: p.pollReply, suplRaw: p.pollRaw };
}

// The record for a system's supplementary data, or null when it offers none or the text doesn't encode
export function encodeSupl(s) {
	const d = s.suplData === null ? s.suplRaw ?? new Uint8Array(0) : encodePollReply(s.suplFormat, s.suplData ?? "");
	if (!d || !d.length) return null;
	const v = new Uint8Array(1 + d.length);
	v[0] = s.suplFormat & 0x0F;
	v.set(d, 1);
	return v;
}

// --- Tags (12.4) ---

// {system: n (0-based), noFallback, check, fallback: channel number or 0, fallbackCheck}, or null when too short
export function decodeTag(v) {
	if (!v || v.length < TAG_LEN) return null;
	return { system: v[0], noFallback: (v[1] & 1) !== 0, check: u16(v, 2), fallback: u16(v, 4), fallbackCheck: u16(v, 6) };
}

// A tag onto prev (its flag bits past bit 0 kept) or a new one
export function encodeTag(tag, prev = null) {
	const v = prev && prev.length >= TAG_LEN ? Uint8Array.from(prev) : new Uint8Array(TAG_LEN);
	v[0] = tag.system;
	v[1] = (v[1] & ~1) | (tag.noFallback ? 1 : 0);
	put16(v, 2, tag.check);
	put16(v, 4, tag.fallback || 0);
	put16(v, 6, tag.fallback ? tag.fallbackCheck : 0);
	return v;
}

export { ID_MAX };
