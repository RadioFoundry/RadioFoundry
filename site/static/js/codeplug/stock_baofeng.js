// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock Baofeng DM-32UV codeplug (docs/spec/dm32-stock-codeplug.md): 4 KB pages in the data flash, each tagged
// with the logical block it holds. Builds the logical image from a data flash read, recognizes a codeplug in it,
// reads its records, and fills the editing model (model.js) from it as stock_tyt.js does, with the same note codes.

import { getFreq } from "./fields.js";
import { LIMITS, POWER_NAMES } from "./records.js";

export const DM32_STOCK = "baofeng-dm32";

// Pages (spec 1): the codeplug's 4 KB pages from 0x1000 up to (not including) PAGES_END; the last byte of a page is
// the logical block it holds, 0x00 (a page given up) or 0xFF (erased) for none
export const PAGES_START = 0x1000, PAGES_END = 0xC9000, PAGE = 0x1000;

// Logical blocks and record sizes (spec 2)
const BLK = { settings: 0x04, lists: 0x0F, channels: 0x12, txContacts: 0x42, contacts: 0x44, zones: 0x5C, ids: 0x67 };
const CH = 0x30, CONTACT = 0x18, LIST = 0x6D, ZONE = 0x91, RADIO_ID = 0x10;
const N_LISTS = 32, LIST_MEMBERS = 32, ZONE_CHANNELS = 64, N_IDS = 255;
const BLOCK_DATA = PAGE - 1;	// the last byte is the block number
// Channels past the first block's are in a layout not mapped yet (spec 5): only the first block's are read
const CH_PER_BLOCK = Math.floor((BLOCK_DATA - 0x10) / CH);

const power = (name) => Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === name));
const POWER = [power("Low"), power("Mid"), power("High")];

// {block: page bytes} of every page in use, or null when two pages claim one block (not this layout) or none is in use
export function pageMap(bytes) {
	const out = new Map();
	for (let a = PAGES_START; a + PAGE <= Math.min(bytes.length, PAGES_END); a += PAGE) {
		const id = bytes[a + PAGE - 1];
		if (id === 0x00 || id === 0xFF) continue;
		if (out.has(id)) return null;
		out.set(id, bytes.subarray(a, a + PAGE));
	}
	return out.size ? out : null;
}

// A NUL-ended ASCII name of up to n bytes: {text, ok}; ok when every character before the end is printable. What
// follows the end is not checked: the stock software leaves old text there
function asciiName(b, off, n) {
	let text = "";
	for (let i = 0; i < n; i++) {
		const c = b[off + i];
		if (c === 0x00 || c === 0xFF) break;
		if (c < 0x20 || c > 0x7E) return { text, ok: false };
		text += String.fromCharCode(c);
	}
	return { text, ok: true };
}
const okName = (b, off, n) => {
	const t = asciiName(b, off, n);
	return t.ok ? t.text : "";
};

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u24 = (b, o) => b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);

// A tone word (spec 2.2): "" none, "103.5" CTCSS (BCD tenths), "D023N" DCS (flag 0x8000, the code's three digits as a
// binary number). A DCS code with a digit above 7 isn't one: null
export function dm32Tone(b, off) {
	const v = u16(b, off);
	if (v === 0xFFFF || v === 0) return "";
	if (v & 0x8000) {
		const n = v & 0x0FFF;
		const code = String(n).padStart(3, "0");
		return n <= 777 && /^[0-7]{3}$/.test(code) ? `D${code}N` : null;
	}
	let tenths = 0;
	for (let i = 3; i >= 0; i--) {
		const d = (v >> (4 * i)) & 0xF;
		if (d > 9) return null;
		tenths = tenths * 10 + d;
	}
	return tenths >= 300 && tenths <= 3000 ? (tenths / 10).toFixed(1) : null;
}

// The fields of a channel record (spec 2.2), or null when it holds no name
function readChannel(r) {
	const nm = asciiName(r, 0, 16);
	if (!nm.text) return null;
	const dmr = (r[0x18] & 0x10) !== 0;
	return {
		name: nm.text, nameOk: nm.ok, rxHz: getFreq(r, 0x10), txHz: getFreq(r, 0x14), mode: dmr ? "dmr" : "fm",
		power: POWER[(r[0x18] >> 1) & 3] ?? POWER[2], bw: r[0x19] & 0x80 ? 25 : 12.5, scanList: (r[0x19] >> 2) & 0x1F,
		timeslot: r[0x1D] & 0x10 ? 2 : 1, colorCode: r[0x1D] & 0x0F, rxList: r[0x1F], rxTone: dm32Tone(r, 0x21),
		txTone: dm32Tone(r, 0x23),
	};
}

// The logical image's blocks, or null: a page map holding a channel block (detect, spec 3)
function blocksOf(bytes) {
	const m = pageMap(bytes);
	return m && m.has(BLK.channels) ? m : null;
}

// Good and bad channels (spec 3): named records with both frequencies are good
export function stockEvidence(bytes) {
	const m = blocksOf(bytes);
	if (!m) return { good: 0, bad: 0 };
	const ch = m.get(BLK.channels), n = u16(ch, 0);
	let good = 0, bad = 0;
	for (let i = 0; i < Math.min(n, CH_PER_BLOCK); i++) {
		const c = readChannel(ch.subarray(0x10 + i * CH, 0x10 + (i + 1) * CH));
		if (c && c.nameOk && c.rxHz && c.txHz) good++;
		else bad++;
	}
	return { good, bad };
}

// DM32_STOCK or null
export function detectStock(bytes) {
	const { good, bad } = stockEvidence(bytes);
	return good && bad * 4 <= good ? DM32_STOCK : null;
}

// Every record in use; channel, contact and zone references as 1-based stock indexes, list members as DMR IDs
export function readStock(bytes) {
	const m = blocksOf(bytes);
	if (!m) throw new Error("no stock DM-32UV codeplug");
	const blank = new Uint8Array(PAGE).fill(0xFF);
	const blk = (id) => m.get(id) || blank;
	const out = { general: { dmrId: 0, radioName: "", bootLine1: "", bootLine2: "" }, channelCount: 0, channels: [], contacts: [], rxLists: [], zones: [] };

	const s = blk(BLK.settings);
	out.general.bootLine1 = okName(s, 0x01, 14);
	out.general.bootLine2 = okName(s, 0x0F, 14);
	const ids = blk(BLK.ids);
	for (let i = 0; i < Math.min(ids[0], N_IDS); i++) {
		const a = 0x10 + i * RADIO_ID;
		if (a + RADIO_ID > BLOCK_DATA) break;
		const id = u24(ids, a);
		if (id >= 1 && id <= 16776415) {
			out.general.dmrId = id;
			out.general.radioName = okName(ids, a + 3, 13);
			break;
		}
	}

	const ch = blk(BLK.channels), tx = blk(BLK.txContacts);
	out.channelCount = u16(ch, 0);
	const nCh = Math.min(out.channelCount, CH_PER_BLOCK);
	for (let i = 0; i < nCh; i++) {
		const c = readChannel(ch.subarray(0x10 + i * CH, 0x10 + (i + 1) * CH));
		if (c) out.channels.push({ index: i + 1, ...c, contact: c.mode === "dmr" && 2 * i + 1 < BLOCK_DATA ? tx[2 * i + 1] : 0 });
	}

	const ct = blk(BLK.contacts);
	for (let i = 0; (i + 1) * CONTACT <= BLOCK_DATA; i++) {
		const r = ct.subarray(i * CONTACT, (i + 1) * CONTACT);
		const type = { 3: "private", 4: "group" }[r[0x16]];
		const name = okName(r, 2, 16);
		const id = u24(r, 0x13);
		if (type && name && id) out.contacts.push({ index: i + 1, name, type, id });
	}

	const gl = blk(BLK.lists);
	for (let i = 0; i < N_LISTS; i++) {
		const a = 0x10 + i * LIST;
		if (a + LIST > BLOCK_DATA) break;
		const name = okName(gl, a + 1, 11);
		if (!name) continue;
		const members = [];
		for (let k = 0; k < LIST_MEMBERS; k++) {
			const id = u24(gl, a + 0x0C + 3 * k);
			if (!id || id === 0xFFFFFF) break;
			members.push(id);
		}
		out.rxLists.push({ index: i + 1, name, ids: members });
	}

	const zb = blk(BLK.zones);
	for (let i = 0; i < Math.min(zb[0], 255); i++) {
		const a = 0x10 + i * ZONE;
		if (a + ZONE > BLOCK_DATA) break;
		const name = okName(zb, a, 16);
		if (!name) continue;
		const n = Math.min(zb[a + 0x10], ZONE_CHANNELS), list = [];
		for (let k = 0; k < n; k++) {
			const x = u16(zb, a + 0x11 + 2 * k);
			if (x && x !== 0xFFFF) list.push(x);
		}
		out.zones.push({ index: i + 1, name, a: list });
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

// The model filled from a stock codeplug, as stock_tyt.js importStock fills it: {counts, notes}. Every table the
// stock codeplug has is replaced; DTMF contacts and APRS configurations are emptied, the VFOs lose their references
export function importStock(m, bytes) {
	const s = readStock(bytes);
	const notes = new Map();
	const note = (code, n = 1) => notes.set(code, (notes.get(code) || 0) + n);
	const shorten = (t, max) => {
		if (t.length <= max) return t;
		note("shortened");
		return t.slice(0, max);
	};

	if (s.channelCount > CH_PER_BLOCK) note("too_many_channels", s.channelCount - CH_PER_BLOCK);
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

	// A list names its members by ID: the contact with that ID, the first such one; an ID without a contact gets a
	// group contact named after it, as the stock radio shows it
	const listRefs = new Set(stockChannels.filter((c) => c.mode === "dmr" && c.rxList).map((c) => c.rxList));
	const named = s.rxLists.filter((l) => l.ids.length || listRefs.has(l.index));
	const stockLists = keepWanted(named, LIMITS.rxLists, listRefs);
	if (named.length > stockLists.length) note("too_many_rxLists", named.length - stockLists.length);
	const byId = new Map();
	for (const c of s.contacts) if (!byId.has(c.id)) byId.set(c.id, c);
	// numbers past the table's, which has gaps
	let extra = s.contacts.reduce((n, c) => Math.max(n, c.index), 0);
	for (const l of stockLists) {
		for (const id of l.ids) {
			if (byId.has(id)) continue;
			const c = { index: ++extra, name: String(id), type: "group", id };
			s.contacts.push(c);
			byId.set(id, c);
		}
	}
	const contactRefs = new Set(stockChannels.filter((c) => c.mode === "dmr" && c.contact).map((c) => c.contact));
	for (const l of stockLists) for (const id of l.ids) contactRefs.add(byId.get(id).index);
	const stockContacts = keepWanted(s.contacts, LIMITS.contacts, contactRefs);
	if (s.contacts.length > stockContacts.length) note("too_many_contacts", s.contacts.length - stockContacts.length);

	const contactOf = new Map();
	const contacts = stockContacts.map((c, i) => {
		const x = { number: i + 1, name: shorten(c.name, LIMITS.name), id: c.id, type: c.type, timeslot: 0 };
		contactOf.set(c.index, x);
		return x;
	});
	const listOf = new Map();
	const rxLists = stockLists.map((l, i) => {
		const members = l.ids.map((id) => contactOf.get(byId.get(id).index)).filter(Boolean);
		if (members.length > LIMITS.rxListContacts) note("list_cut");
		const x = { number: i + 1, name: shorten(l.name, LIMITS.name), contacts: members.slice(0, LIMITS.rxListContacts) };
		listOf.set(l.index, x);
		return x;
	});

	const channelOf = new Map();
	const channels = stockChannels.map((c, i) => {
		const dmr = c.mode === "dmr";
		const tone = (t) => {
			if (dmr) return "";
			if (t === null) note("bad_tone");
			return t || "";
		};
		const x = {
			number: i + 1, name: shorten(c.name, LIMITS.name), rxHz: c.rxHz, txHz: c.txHz, mode: c.mode,
			power: c.power, tot: 0, rxTone: tone(c.rxTone), txTone: tone(c.txTone),
			bw: dmr ? 12.5 : c.bw, rxOnly: false, allSkip: false, zoneSkip: false, vox: false, squelch: 0,
			colorCode: dmr ? c.colorCode : 0, timeslot: dmr ? c.timeslot : 1,
			contact: dmr ? contactOf.get(c.contact) || null : null, rxList: dmr ? listOf.get(c.rxList) || null : null,
			aprs: null, dmrId: 0, noBeep: false, noEco: false, dmo: false, roam: false, aliasTs1: 0, aliasTs2: 0,
			useLocation: false, lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0,
		};
		channelOf.set(c.index, x);
		return x;
	});

	let zones = s.zones.map((z) => ({ name: shorten(z.name, LIMITS.name), channels: z.a.map((n) => channelOf.get(n)).filter(Boolean), slot: null }));
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
