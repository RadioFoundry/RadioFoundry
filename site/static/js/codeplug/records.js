// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The open-firmware codeplug's records (docs/spec/open-firmware-cps.md 7.2-7.3), decoded into plain objects and
// encoded back. An encoder writes only the fields it owns onto the record as stored, so vendor bytes and flags no
// field covers survive an edit. Addresses are image addresses (image.js). References between records (a channel's
// contact, a zone's channels) are 1-based numbers here; model.js turns them into objects.

import { EE, FL } from "./image.js";
import {
	getFreq, putFreq, getBcdId, putBcdId, getTone, putTone, getName, putName, getCoord, coordBytes, u16, put16,
} from "./fields.js";

export const LIMITS = {
	channels: 1024, zones: 68, zoneChannels: 80, contacts: 1024, rxLists: 76, rxListContacts: 32,
	dtmfContacts: 63, aprs: 8, name: 16, radioName: 8, bootText: 16, aprsName: 8, aprsComment: 23, dtmfCode: 16,
};

// --- Layout ---

export const CH_SIZE = 56;
const CH_PER_BANK = 128;
const BANK0 = EE + 0x3780;
const BANK1 = FL + 0x7B1B0;
const BANK_STRIDE = 16 + CH_PER_BANK * CH_SIZE;

export const bankBitmap = (bank) => (bank === 0 ? BANK0 : BANK1 + (bank - 1) * BANK_STRIDE);
export const channelAddr = (n) => bankBitmap(Math.floor((n - 1) / CH_PER_BANK)) + 16 + ((n - 1) % CH_PER_BANK) * CH_SIZE;

export const GENERAL = EE + 0x00E0;
export const GENERAL_SIZE = 0x28;
export const DTMF_CONTACTS = EE + 0x2F88;
export const APRS_BASE = EE + 0x1588;
export const LUCZ = EE + 0x6000;
export const BOOT_TEXT = EE + 0x7540;
export const VFO = [EE + 0x7590, EE + 0x75C8];
export const ZONE_BITMAP = EE + 0x8010;
export const ZONE_DATA = EE + 0x8030;
export const ZONE_LAYOUT = EE + 0x806F;
export const ZONE_RECORD = 0xB0;
export const CONTACTS = FL + 0x87620;
export const CONTACT_SIZE = 24;
export const LIST_COUNTS = FL + 0x8D620;
export const LISTS = FL + 0x8D6A0;
export const LIST_SIZE = 80;

// --- Channels (7.3.3) ---

// Power byte: 0 = the radio's master power, n = level n - 1. qdmr's names for the levels it writes
export const POWER_NAMES = { 0: "Default", 1: "Min", 3: "Low", 6: "Mid", 9: "High", 10: "Max" };

export function blankChannel() {
	const r = new Uint8Array(CH_SIZE);
	r.fill(0xFF, 0, 16);
	put16(r, 0x20, 0xFFFF);
	put16(r, 0x22, 0xFFFF);
	r[0x28] = 0x16;		// the vendor defaults the per-channel DMR ID bytes replaced
	return r;
}

// A record, or null if its frequencies don't decode (an erased or broken record)
export function decodeChannel(r) {
	const rxHz = getFreq(r, 0x10), txHz = getFreq(r, 0x14);
	if (rxHz === null || txHz === null) return null;
	const l1 = r[0x26], f1 = r[0x30], f4 = r[0x33];
	const hasLoc = (l1 & 0x08) !== 0;
	return {
		name: getName(r, 0, 16),
		rxHz, txHz,
		mode: r[0x18] === 1 ? "dmr" : "fm",
		power: r[0x19],
		tot: r[0x1B] * 15,
		rxTone: getTone(r, 0x20),
		txTone: getTone(r, 0x22),
		bw: f4 & 0x02 ? 25 : 12.5,
		rxOnly: (f4 & 0x04) !== 0,
		allSkip: (f4 & 0x10) !== 0,
		zoneSkip: (f4 & 0x20) !== 0,
		vox: (f4 & 0x40) !== 0,
		squelch: r[0x37] <= 21 ? r[0x37] : 10,
		colorCode: r[0x2C] & 0x0F,
		timeslot: r[0x31] & 0x40 ? 2 : 1,
		contact: u16(r, 0x2E),
		rxList: r[0x2B],
		aprs: r[0x2D],
		dmrId: l1 & 0x80 ? (r[0x27] << 16) | (r[0x28] << 8) | r[0x29] : 0,
		noBeep: (l1 & 0x40) !== 0,
		noEco: (l1 & 0x20) !== 0,
		dmo: (l1 & 0x04) !== 0,
		roam: (l1 & 0x01) !== 0,
		aliasTs1: f1 & 0x03,
		aliasTs2: (f1 >> 2) & 0x03,
		useLocation: hasLoc,
		lat: getCoord(r[0x1D], r[0x1C], r[0x1A]),
		lon: getCoord(r[0x24], r[0x1F], r[0x1E]),
		sfr: (l1 & 0x02) !== 0,
		sfrA: r[0x25],
		sfrB: r[0x2A],
	};
}

// Write a channel's fields onto its stored record r (a blank one if r doesn't decode); r is changed in place
export function encodeChannel(c, r) {
	if (getFreq(r, 0x10) === null) r.set(blankChannel());
	putName(r, 0, 16, c.name);
	putFreq(r, 0x10, c.rxHz);
	putFreq(r, 0x14, c.txHz);
	r[0x18] = c.mode === "dmr" ? 1 : 0;
	r[0x19] = clamp(c.power, 0, 255);
	r[0x1B] = clamp(Math.ceil((c.tot || 0) / 15), 0, 255);
	putTone(r, 0x20, c.rxTone);
	putTone(r, 0x22, c.txTone);
	r[0x33] = (r[0x33] & ~0x76) | (c.bw === 25 ? 0x02 : 0) | (c.rxOnly ? 0x04 : 0) | (c.allSkip ? 0x10 : 0) |
		(c.zoneSkip ? 0x20 : 0) | (c.vox ? 0x40 : 0);
	r[0x37] = clamp(c.squelch, 0, 21);
	r[0x2C] = (r[0x2C] & 0xF0) | (c.colorCode & 0x0F);
	r[0x31] = (r[0x31] & ~0x40) | (c.timeslot === 2 ? 0x40 : 0);
	put16(r, 0x2E, c.contact || 0);
	r[0x2B] = clamp(c.rxList || 0, 0, 255);
	r[0x2D] = clamp(c.aprs || 0, 0, 255);
	r[0x26] = (r[0x26] & ~0xEF) | (c.dmrId ? 0x80 : 0) | (c.noBeep ? 0x40 : 0) | (c.noEco ? 0x20 : 0) |
		(c.useLocation ? 0x08 : 0) | (c.dmo ? 0x04 : 0) | (c.sfr ? 0x02 : 0) | (c.roam ? 0x01 : 0);
	if (c.dmrId) {
		r[0x27] = (c.dmrId >> 16) & 0xFF;
		r[0x28] = (c.dmrId >> 8) & 0xFF;
		r[0x29] = c.dmrId & 0xFF;
	} else if (r[0x27] !== 0x00 || r[0x28] !== 0x16 || r[0x29] !== 0x00) {
		r[0x27] = 0x00;
		r[0x28] = 0x16;
		r[0x29] = 0x00;
	}
	r[0x30] = (r[0x30] & ~0x0F) | (c.aliasTs1 & 3) | ((c.aliasTs2 & 3) << 2);
	// Location bytes only when they change, so a record whose location bytes hold something else keeps them
	const old = { lat: getCoord(r[0x1D], r[0x1C], r[0x1A]), lon: getCoord(r[0x24], r[0x1F], r[0x1E]) };
	if (Math.abs(old.lat - (c.lat || 0)) > 0.00005 || Math.abs(old.lon - (c.lon || 0)) > 0.00005) {
		const [ah, am, al] = coordBytes(c.lat || 0);
		const [oh, om, ol] = coordBytes(c.lon || 0);
		r[0x1D] = ah; r[0x1C] = am; r[0x1A] = al;
		r[0x24] = oh; r[0x1F] = om; r[0x1E] = ol;
	}
	r[0x25] = c.sfrA & 0xFF;
	r[0x2A] = c.sfrB & 0xFF;
	return r;
}

function clamp(v, lo, hi) {
	v = Math.round(Number(v) || 0);
	return v < lo ? lo : v > hi ? hi : v;
}

// --- Zones (7.2, 7.3.4) ---

export function zoneLayout(b) {
	return b[ZONE_LAYOUT] <= 0x04 ? 80 : 16;
}

export function decodeZone(r, per) {
	const out = { name: getName(r, 0, 16), channels: [] };
	for (let i = 0; i < per; i++) {
		const n = u16(r, 16 + 2 * i);
		if (n === 0) break;
		out.channels.push(n);
	}
	return out;
}

// The 80-channel layout: name, channel numbers, zeros after the last
export function encodeZone(z, r) {
	r.fill(0, 0, ZONE_RECORD);
	putName(r, 0, 16, z.name);
	z.channels.slice(0, 80).forEach((n, i) => put16(r, 16 + 2 * i, n));
	return r;
}

// --- Contacts (7.3.5) ---

export const CALL_TYPES = ["group", "private", "all"];

export function decodeContact(r) {
	if (r[0] === 0xFF || r[0x14] > 2) return null;
	const type = CALL_TYPES[r[0x14]];
	const id = type === "all" ? 0xFFFFFF : getBcdId(r, 0x10);
	if (!id) return null;
	const ts = r[0x17] !== 0xFF && (r[0x17] & 1) === 0 ? (r[0x17] & 2 ? 2 : 1) : 0;
	return { name: getName(r, 0, 16), id, type, timeslot: ts };
}

export function encodeContact(c, r) {
	const was = decodeContact(r);
	putName(r, 0, 16, c.name);
	putBcdId(r, 0x10, c.type === "all" ? 0xFFFFFF : c.id);
	r[0x14] = CALL_TYPES.indexOf(c.type);
	if (!was) {
		r[0x15] = 0;
		r[0x16] = 0;
	}
	if (!was || was.timeslot !== c.timeslot) r[0x17] = c.timeslot ? (c.timeslot - 1) << 1 : 0x01;
	return r;
}

// --- RX group lists (7.3.6) ---

export function decodeList(r, count) {
	const out = { name: getName(r, 0, 16), contacts: [] };
	for (let i = 0; i < Math.min(count - 1, 32); i++) {
		const c = u16(r, 16 + 2 * i);
		if (c === 0 || c > 1024) break;
		out.contacts.push(c);
	}
	return out;
}

export function encodeList(l, r) {
	r.fill(0, 0, LIST_SIZE);
	putName(r, 0, 16, l.name);
	l.contacts.slice(0, 32).forEach((n, i) => put16(r, 16 + 2 * i, n));
	return r;
}

// --- DTMF contacts (7.3.7) ---

const DTMF = "0123456789ABCD*#";

export function decodeDtmf(r) {
	if (r[0] === 0xFF || r[0] === 0x00) return null;
	let code = "";
	for (let i = 16; i < 32 && r[i] < 16; i++) code += DTMF[r[i]];
	return { name: getName(r, 0, 16), code };
}

export function encodeDtmf(d, r) {
	putName(r, 0, 16, d.name);
	r.fill(0xFF, 16, 32);
	const code = String(d.code || "").toUpperCase();
	let n = 0;
	for (const ch of code) {
		const v = DTMF.indexOf(ch);
		if (v >= 0 && n < 16) r[16 + n++] = v;
	}
	return r;
}

export const validDtmf = (code) => /^[0-9A-D*#]{1,16}$/i.test(String(code || ""));

// --- APRS configurations (7.3.8) ---

export const APRS_SIZE = 0x40;
const APRS_MAGIC = 0x4153;

function getPath(r, off) {
	const call = getName(r, off, 6).replace(/ +$/, "");
	if (!call) return "";
	const ssid = r[off + 6];
	return ssid >= 1 && ssid <= 15 ? `${call}-${ssid}` : call;
}

function putPath(r, off, text) {
	const [call, ssid] = String(text || "").toUpperCase().split("-");
	r.fill(0x20, off, off + 6);
	for (let i = 0; i < 6 && i < call.length; i++) r[off + i] = call.charCodeAt(i) & 0x7F;
	const s = Number(ssid);
	r[off + 6] = s >= 1 && s <= 15 ? s : 0;
}

export function decodeAprs(r) {
	const magic = u16(r, 0x3E);
	if (r[0] === 0xFF || r[0] === 0x00 || (magic !== APRS_MAGIC && magic !== APRS_MAGIC - 1)) return null;
	const flags = r[0x3D];
	const amb = magic === APRS_MAGIC ? flags >> 5 : (flags >> 4) & 7;
	return {
		name: getName(r, 0, 8),
		ssid: r[0x08] <= 15 ? r[0x08] : 0,
		lat: getCoord(r[0x09], r[0x0A], r[0x0B]),
		lon: getCoord(r[0x0C], r[0x0D], r[0x0E]),
		path1: getPath(r, 0x0F),
		path2: getPath(r, 0x16),
		symTable: String.fromCharCode(r[0x1D]),
		symCode: String.fromCharCode(r[0x1E]),
		comment: getName(r, 0x1F, 23),
		txHz: getFreq(r, 0x37) || 0,
		baud300: (flags & 0x01) !== 0,
		useFixed: (flags & 0x02) !== 0,
		qsy: (flags & 0x04) !== 0,
		silent: (flags & 0x08) !== 0,
		ambiguity: Math.min(amb, 4),
	};
}

export function encodeAprs(a, r) {
	const was = decodeAprs(r);
	if (!was) r.fill(0);
	putName(r, 0, 8, a.name);
	r[0x08] = a.ssid & 0x0F;
	if (!was || was.lat !== a.lat || was.lon !== a.lon) {
		r.set(coordBytes(a.lat), 0x09);
		r.set(coordBytes(a.lon), 0x0C);
	}
	if (!was || was.path1 !== a.path1) putPath(r, 0x0F, a.path1);
	if (!was || was.path2 !== a.path2) putPath(r, 0x16, a.path2);
	r[0x1D] = (a.symTable || "/").charCodeAt(0) & 0xFF;
	r[0x1E] = (a.symCode || ">").charCodeAt(0) & 0xFF;
	if (!was || was.comment !== a.comment) putName(r, 0x1F, 24, a.comment);
	if (a.txHz) putFreq(r, 0x37, a.txHz);
	else r.fill(0, 0x37, 0x3B);
	// Always the current layout: an old (0x4152) record is upgraded, as the firmware does
	r[0x3D] = (a.baud300 ? 1 : 0) | (a.useFixed ? 2 : 0) | (a.qsy ? 4 : 0) | (a.silent ? 8 : 0) | ((a.ambiguity & 7) << 5);
	put16(r, 0x3E, APRS_MAGIC);
	return r;
}

// --- General settings, boot text (7.3.2, 7.2) ---

export function decodeGeneral(b) {
	const g = b.subarray(GENERAL, GENERAL + GENERAL_SIZE);
	return {
		radioName: getName(g, 0, 8),
		dmrId: getBcdId(g, 8),
		groupHang: g[0x18] === 0xFF ? 0 : g[0x18],
		privateHang: g[0x19] === 0xFF ? 0 : g[0x19],
		bootLine1: getName(b, BOOT_TEXT, 16),
		bootLine2: getName(b, BOOT_TEXT + 16, 16),
	};
}

export function encodeGeneral(s, b) {
	const was = decodeGeneral(b);
	const g = b.subarray(GENERAL, GENERAL + GENERAL_SIZE);
	if (was.radioName !== s.radioName) putName(g, 0, 8, s.radioName);
	if (was.dmrId !== s.dmrId) {
		if (s.dmrId) putBcdId(g, 8, s.dmrId);
		else g.fill(0xFF, 8, 12);
	}
	if (was.groupHang !== s.groupHang) g[0x18] = s.groupHang & 0xFF;
	if (was.privateHang !== s.privateHang) g[0x19] = s.privateHang & 0xFF;
	if (was.bootLine1 !== s.bootLine1) putName(b, BOOT_TEXT, 16, s.bootLine1);
	if (was.bootLine2 !== s.bootLine2) putName(b, BOOT_TEXT + 16, 16, s.bootLine2);
}
