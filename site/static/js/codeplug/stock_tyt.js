// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stock TYT-family codeplugs (docs/spec/codeplug-stock-tyt.md): the MD-UV380 family (DM-1701, RT84, MD-UV380/390,
// RT3S, MD-2017) and the MD-380 family, as they sit in the radio's SPI flash. Recognizes one, reads its records, and
// fills the editing model (model.js) from it the way a qdmr import does (qdmr.js), with the same note codes.

import { getFreq, getTone } from "./fields.js";
import { LIMITS, POWER_NAMES } from "./records.js";

const GENERAL = { boot1: 0x2040, boot2: 0x2054, bootLen: 10, dmrId: 0x2084, name: 0x20B0, nameLen: 16 };

// Table addresses and counts per family (3.2)
export const STOCK = {
	"tyt-uv380": {
		channels: 0x110000, nChannels: 3000, contacts: 0x140000, nContacts: 10000, lists: 0xEC20, nLists: 250,
		zones: 0x149E0, nZones: 250, zoneExt: 0x31000, size: 0x1A0000,
	},
	"tyt-md380": {
		channels: 0x1EE00, nChannels: 1000, contacts: 0x5F80, nContacts: 1000, lists: 0xEC20, nLists: 250,
		zones: 0x149E0, nZones: 250, zoneExt: 0, size: 0x40000,
	},
};

const CH = 64, CONTACT = 36, LIST = 96, ZONE = 64, ZONE_EXT = 224;

// RadioFoundry's settings ring and sector journal on these radios (settings-store.md 1): once RadioFoundry has claimed the area, stock
// records there are gone, and RadioFoundry's own bytes must not read as records
const RFOUNDRY_AREA = [0x11000, 0x1B000];
const MAGICS = ["OSET", "OJNL", "OJN2"];

export function rfoundryClaimed(b) {
	if (b.length < RFOUNDRY_AREA[1]) return false;
	const at = (a) => String.fromCharCode(b[a], b[a + 1], b[a + 2], b[a + 3]);
	for (let a = RFOUNDRY_AREA[0]; a < 0x19000; a += 256) if (at(a) === "OSET") return true;
	return MAGICS.includes(at(0x19000)) || MAGICS.includes(at(0x1A000));
}
// The DMR ID in the stock general settings, or 0 when it isn't a valid one (erased, or overwritten by RadioFoundry's codeplug)
export function stockDmrId(b) {
	if (b.length < GENERAL.dmrId + 3) return 0;
	const id = b[GENERAL.dmrId] | (b[GENERAL.dmrId + 1] << 8) | (b[GENERAL.dmrId + 2] << 16);
	return id >= 1 && id <= 16776415 ? id : 0;
}
export const RDT_MD380 = 262709, RDT_UV380 = 852533, RDT_HEADER = 549;
const HIGH = Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === "High"));
const LOW = Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === "Low"));

// The codeplug bytes at their flash addresses: a TYT MD-380 CPS file loses its header
export function stockBytes(bytes) {
	return bytes.length === RDT_MD380 ? bytes.subarray(RDT_HEADER, RDT_HEADER + 0x40000) : bytes;
}

const u16 = (b, o) => b[o] | (b[o + 1] << 8);

// UTF-16LE text of up to n characters: {text, ok}; ok when every character is 0x20 or above and not 0xFFFF, with
// only 0x0000 after the first 0x0000
export function utf16(b, off, n) {
	let text = "", ok = true, end = false;
	for (let i = 0; i < n; i++) {
		const c = u16(b, off + 2 * i);
		if (end) {
			if (c !== 0) ok = false;
		} else if (c === 0) end = true;
		else {
			if (c < 0x20 || c === 0xFFFF) ok = false;
			text += String.fromCharCode(c);
		}
	}
	return { text, ok };
}

// The text of a valid name; "" for an erased or broken one (RadioFoundry's settings area overlaps the lists and zones, 3.2)
function okText(b, off, n) {
	const t = utf16(b, off, n);
	return t.ok ? t.text : "";
}

// The fields of a channel record, or null when it isn't in use
function readChannel(r) {
	const mode = r[0] & 3;
	const nm = utf16(r, 0x20, 16);
	if (!nm.text || (mode !== 1 && mode !== 2)) return null;
	return {
		name: nm.text, nameOk: nm.ok, rxHz: getFreq(r, 0x10), txHz: getFreq(r, 0x14), mode: mode === 2 ? "dmr" : "fm",
		bw: r[0] & 0x08 ? 25 : 12.5, rxOnly: (r[1] & 0x02) !== 0, timeslot: ((r[1] >> 2) & 3) === 2 ? 2 : 1,
		colorCode: r[1] >> 4, vox: (r[4] & 0x10) !== 0, high: (r[4] & 0x20) !== 0, contact: u16(r, 6), tot: r[8] * 15,
		scanList: r[0x0A], rxList: r[0x0C], rxTone: getTone(r, 0x18), txTone: getTone(r, 0x1A),
	};
}

// Good and bad in-use channels of a family's table (spec section 2, step 2)
export function stockEvidence(bytes, format) {
	const L = STOCK[format];
	let good = 0, bad = 0;
	for (let i = 0; i < L.nChannels; i++) {
		const a = L.channels + i * CH;
		if (a + CH > bytes.length) break;
		const c = readChannel(bytes.subarray(a, a + CH));
		if (!c) continue;
		if (c.nameOk && c.rxHz && c.txHz) good++;
		else bad++;
	}
	return { good, bad };
}

// "tyt-uv380" | "tyt-md380" | null
export function detectStock(bytes) {
	const b = stockBytes(bytes);
	for (const format of Object.keys(STOCK)) {
		const { good, bad } = stockEvidence(b, format);
		if (good && bad * 4 <= good) return format;
	}
	return null;
}

// Every record in use, with references as 1-based stock indexes
export function readStock(bytes, format) {
	const b = stockBytes(bytes);
	const L = STOCK[format];
	const claimed = rfoundryClaimed(b);
	const rec = (base, size, i) => {
		const a = base + i * size;
		if (a + size > b.length) return null;
		if (claimed && a < RFOUNDRY_AREA[1] && a + size > RFOUNDRY_AREA[0]) return null;
		return b.subarray(a, a + size);
	};
	const indexes = (r, off, n) => {
		const out = [];
		for (let i = 0; i < n; i++) {
			const x = u16(r, off + 2 * i);
			if (!x) break;
			out.push(x);
		}
		return out;
	};
	const out = {
		general: {
			radioName: okText(b, GENERAL.name, GENERAL.nameLen),
			dmrId: b[GENERAL.dmrId] | (b[GENERAL.dmrId + 1] << 8) | (b[GENERAL.dmrId + 2] << 16),
			bootLine1: okText(b, GENERAL.boot1, GENERAL.bootLen),
			bootLine2: okText(b, GENERAL.boot2, GENERAL.bootLen),
		},
		channels: [], contacts: [], rxLists: [], zones: [], claimed,
	};
	if (out.general.dmrId === 0xFFFFFF) out.general.dmrId = 0;
	for (let i = 0; i < L.nChannels; i++) {
		const r = rec(L.channels, CH, i);
		const c = r && readChannel(r);
		if (c) out.channels.push({ index: i + 1, ...c });
	}
	for (let i = 0; i < L.nContacts; i++) {
		const r = rec(L.contacts, CONTACT, i);
		if (!r) break;
		const type = [null, "group", "private", "all"][r[3] & 3];
		const name = okText(r, 4, 16);
		if (type && name) out.contacts.push({ index: i + 1, name, type, id: r[0] | (r[1] << 8) | (r[2] << 16) });
	}
	for (let i = 0; i < L.nLists; i++) {
		const r = rec(L.lists, LIST, i);
		const name = r && okText(r, 0, 16);
		if (name) out.rxLists.push({ index: i + 1, name, contacts: indexes(r, 0x20, 32) });
	}
	for (let i = 0; i < L.nZones; i++) {
		const r = rec(L.zones, ZONE, i);
		const name = r && okText(r, 0, 16);
		if (!name) continue;
		let a = indexes(r, 0x20, 16), bList = [];
		const x = L.zoneExt ? rec(L.zoneExt, ZONE_EXT, i) : null;
		if (x) {
			if (a.length === 16) a = a.concat(indexes(x, 0, 48));
			bList = indexes(x, 0x60, 64);
		}
		out.zones.push({ index: i + 1, name, a, b: bList });
	}
	return out;
}

// Keep at most max rows, those in `wanted` first; the rest in table order. Returns the kept rows in table order
function keepWanted(rows, max, wanted) {
	if (rows.length <= max) return rows;
	const keep = new Set(rows.filter((r) => wanted.has(r.index)).slice(0, max));
	for (const r of rows) {
		if (keep.size >= max) break;
		keep.add(r);
	}
	return rows.filter((r) => keep.has(r));
}

// The model filled from a stock codeplug, as importQdmr fills it: {counts, notes}. Every table the stock codeplug has
// is replaced; DTMF contacts and APRS configurations are emptied, the VFOs lose their references
export function importStock(m, bytes, format) {
	const s = readStock(bytes, format);
	const notes = new Map();
	const note = (code, n = 1) => notes.set(code, (notes.get(code) || 0) + n);
	const shorten = (t, max) => {
		if (t.length <= max) return t;
		note("shortened");
		return t.slice(0, max);
	};

	let stockChannels = s.channels.filter((c) => {
		if (c.rxHz && c.txHz) return true;
		note("bad_frequency");
		return false;
	});
	if (stockChannels.length > LIMITS.channels) {
		note("too_many_channels", stockChannels.length - LIMITS.channels);
		stockChannels = stockChannels.slice(0, LIMITS.channels);
	}
	if (stockChannels.some((c) => c.scanList)) note("scan_lists");
	// the stock 0 is "off"; RadioFoundry's 0 is the radio's TX timeout
	const totOff = stockChannels.filter((c) => !c.tot).length;
	if (totOff) note("tot_off", totOff);
	if (s.claimed) note("settings_area");

	// RX group lists, then contacts: the ones kept channels and lists refer to come first when a table is too long
	const listRefs = new Set(stockChannels.filter((c) => c.mode === "dmr" && c.rxList).map((c) => c.rxList));
	const stockLists = keepWanted(s.rxLists, LIMITS.rxLists, listRefs);
	if (s.rxLists.length > stockLists.length) note("too_many_rxLists", s.rxLists.length - stockLists.length);
	const contactRefs = new Set(stockChannels.filter((c) => c.mode === "dmr" && c.contact).map((c) => c.contact));
	for (const l of stockLists) for (const x of l.contacts) contactRefs.add(x);
	const stockContacts = keepWanted(s.contacts, LIMITS.contacts, contactRefs);
	if (s.contacts.length > stockContacts.length) note("too_many_contacts", s.contacts.length - stockContacts.length);

	const contactOf = new Map();
	const contacts = stockContacts.map((c, i) => {
		const x = { number: i + 1, name: shorten(c.name, LIMITS.name), id: c.type === "all" ? 0xFFFFFF : c.id, type: c.type, timeslot: 0 };
		contactOf.set(c.index, x);
		return x;
	});
	const listOf = new Map();
	const rxLists = stockLists.map((l, i) => {
		const members = l.contacts.map((n) => contactOf.get(n)).filter(Boolean);
		if (members.length > LIMITS.rxListContacts) note("list_cut");
		const x = { number: i + 1, name: shorten(l.name, LIMITS.name), contacts: members.slice(0, LIMITS.rxListContacts) };
		listOf.set(l.index, x);
		return x;
	});

	const channelOf = new Map();
	const channels = stockChannels.map((c, i) => {
		const dmr = c.mode === "dmr";
		const x = {
			number: i + 1, name: shorten(c.name, LIMITS.name), rxHz: c.rxHz, txHz: c.txHz, mode: c.mode,
			power: c.high ? HIGH : LOW, tot: c.tot, rxTone: dmr ? "" : c.rxTone, txTone: dmr ? "" : c.txTone,
			bw: dmr ? 12.5 : c.bw, rxOnly: c.rxOnly, allSkip: false, zoneSkip: false, vox: c.vox, squelch: 0,
			colorCode: dmr ? c.colorCode : 0, timeslot: dmr ? c.timeslot : 1,
			contact: dmr ? contactOf.get(c.contact) || null : null, rxList: dmr ? listOf.get(c.rxList) || null : null,
			aprs: null, dmrId: 0, noBeep: false, noEco: false, dmo: false, roam: false, aliasTs1: 0, aliasTs2: 0,
			useLocation: false, lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0,
		};
		channelOf.set(c.index, x);
		return x;
	});

	// Zones: a B list makes two zones, "<name> A" and "<name> B", as a qdmr import does
	let zones = [];
	for (const z of s.zones) {
		const pick = (list) => list.map((n) => channelOf.get(n)).filter(Boolean);
		const A = pick(z.a), B = pick(z.b);
		if (B.length) {
			zones.push({ name: shorten(`${z.name} A`, LIMITS.name), channels: A, slot: null });
			zones.push({ name: shorten(`${z.name} B`, LIMITS.name), channels: B, slot: null });
		} else zones.push({ name: shorten(z.name, LIMITS.name), channels: A, slot: null });
	}
	for (const z of zones) {
		if (z.channels.length > LIMITS.zoneChannels) {
			note("zone_cut");
			z.channels = z.channels.slice(0, LIMITS.zoneChannels);
		}
	}
	if (zones.length > LIMITS.zones) {
		note("too_many_zones", zones.length - LIMITS.zones);
		zones = zones.slice(0, LIMITS.zones);
	}

	Object.assign(m, { channels, contacts, rxLists, zones, dtmfContacts: [], aprs: [] });
	for (const v of m.vfo) if (v) Object.assign(v, { contact: null, rxList: null, aprs: null });
	const g = m.general;
	g.dmrId = s.general.dmrId;
	g.radioName = shorten(s.general.radioName, LIMITS.radioName);
	g.bootLine1 = shorten(s.general.bootLine1, LIMITS.bootText);
	g.bootLine2 = shorten(s.general.bootLine2, LIMITS.bootText);

	const counts = { channels: channels.length, contacts: contacts.length, rxLists: rxLists.length, zones: zones.length };
	return { counts, notes: [...notes].map(([code, n]) => ({ code, n })) };
}
