// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stock AnyTone codeplugs (docs/spec/codeplug-stock-anytone.md): the tables the vendor firmware keeps in the radio's
// SPI NAND, read through its PC mode before an install (flasher/anytone.js AnytoneProgram) and filled into the
// editing model (model.js) the way a qdmr import fills it (qdmr.js), with the same note codes. RadioFoundry keeps its
// own codeplug elsewhere in the NAND, so these tables can't be read once it runs.
//
// Each table sits in a pair of 128 KB blocks, one current (section 2); the PC mode's read picks the current block
// itself, so the reader asks for the pair's first. Bitmaps say which zones, channels, lists and contacts exist; only
// the channels the zones name are read, since reading all 4000 would take minutes.

import { bcd } from "./fields.js";
import { LIMITS, POWER_NAMES } from "./records.js";

export const BLOCK = 0x20000;
const VALID = [0x55, 0x55, 0xAA, 0xAA], OBSOLETE = [0xAA, 0xAA, 0x55, 0x55];

// Table addresses and sizes (section 3)
export const AT = {
	channels: 0x00800000, nChannels: 4000, chPerPair: 128, chHalf: 64,
	zoneNames: 0x02540000, zoneMembers: 0x01000000, nZones: 250, zoneName: 32, zoneRec: 0x200, zoneSlots: 250,
	ids: 0x02580000, nIds: 250, idRec: 32,
	lists: 0x02980000, nLists: 250, listRec: 0x200, listSlots: 64, listName: 0x100,
	contacts: 0x02680000, nContacts: 10000, perSlot: 1000, contactRec: 100, contactUsed: 0x27,
	name: 16,
};
// The bitmaps (section 3.3): bit set = the zone, channel or list exists; for contacts bit set = free
export const MAP = { zones: 0x024C1300, channels: 0x024C1500, lists: 0x025C0B10, contacts: 0x02640000 };
const PAIR = 2 * BLOCK;
const CONTACT_END = 0x4E20;
const NONE = 0xFF;		// no RX group list, no scan list
// The contact's type byte (offset 0): the call type (section 3.6)
export const CONTACT_TYPES = { 0: "private", 1: "group", 2: "all" };
// CTCSS tones by index (0x33: the channel's own tone at 0x10), in 0.1 Hz
export const CTCSS = [625, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148,
	1188, 1230, 1273, 1318, 1365, 1413, 1462, 1500, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862,
	1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541];
const CUSTOM_TONE = 0x33;

const HIGH = Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === "High"));
const MID = Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === "Mid"));
const LOW = Number(Object.keys(POWER_NAMES).find((k) => POWER_NAMES[k] === "Low"));

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const bcdBe = (b, o) => bcd(((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0, 8);
const same = (b, o, w) => w.every((x, i) => b[o + i] === x);
export const bit = (b, i) => ((b[i >> 3] >> (i & 7)) & 1) === 1;

// ASCII text up to the first NUL (or n bytes); "" for an erased field or one holding other bytes (the radio's names
// are GBK: one in Chinese has no name here)
export function text(b, off, n) {
	let s = "";
	for (let i = 0; i < n; i++) {
		const c = b[off + i];
		if (c === 0) break;
		if (c < 0x20 || c > 0x7E) return "";
		s += String.fromCharCode(c);
	}
	return s.trim();
}

// The current block of a pair from the 16 bytes at the end of its first block: the first when it carries the valid
// marker and not the obsolete one, else the second (section 2). The radio's read applies it; the simulated radio too
export function currentBlock(base, tail0) {
	return same(tail0, 12, VALID) && !same(tail0, 8, OBSOLETE) ? base : base + BLOCK;
}

// A tone ("" none, "88.5" CTCSS, "D023N" DCS) from the type (0 off, 1 CTCSS, 2 DCS), the CTCSS index, the DCS word and
// the custom CTCSS; null for one that can't be read
export function tone(type, index, dcs, custom) {
	if (type === 0) return "";
	if (type === 1) {
		const t = index === CUSTOM_TONE ? custom : CTCSS[index];
		return t && t !== 0xFFFF ? (t / 10).toFixed(1) : null;
	}
	if (type === 2) {
		const v = dcs & 0x1FF;
		return `D${((v >> 6) & 7)}${((v >> 3) & 7)}${v & 7}${dcs & 0x200 ? "I" : "N"}`;
	}
	return null;
}

// The fields of a channel's first half (64 bytes), or null when it holds no channel
export function readChannel(r) {
	const rx = bcdBe(r, 0);
	if (rx === null || !rx) return null;
	const off = bcdBe(r, 4) ?? 0;
	const dir = (r[8] >> 6) & 3;
	const talkaround = (r[9] & 0x80) !== 0;
	const rxHz = rx * 10;
	const txHz = talkaround || dir === 0 ? rxHz : dir === 1 ? rxHz + off * 10 : dir === 2 ? rxHz - off * 10 : 0;
	return {
		name: text(r, 0x23, 17), rxHz, txHz, type: r[8] & 3, power: (r[8] >> 2) & 3, wide: ((r[8] >> 4) & 3) !== 0,
		rxTone: tone(r[9] & 3, r[0xB], u16(r, 0xE), u16(r, 0x10)), txTone: tone((r[9] >> 2) & 3, r[0xA], u16(r, 0xC), u16(r, 0x10)),
		rxOnly: (r[9] & 0x20) !== 0, contact: u32(r, 0x14), radioId: r[0x18], rxList: r[0x1C], scanList: r[0x1B],
		colorCode: r[0x20] & 0x0F, timeslot: r[0x21] & 1 ? 2 : 1,
	};
}

export function readContact(r) {
	const type = CONTACT_TYPES[r[0]];
	const id = bcdBe(r, 0x23);
	if (!type || id === null) return null;
	return { name: text(r, 1, 17) || String(id), type, id };
}

// Every table the channels in zones reach, read through read(addr, n) (bytes from the radio's memory; the stock PC
// mode reads them). onBytes(done, total): total grows as the references are found
export async function readAnytone(read, onBytes = () => {}) {
	let done = 0, total = 0;
	const plan = (n) => { total += n; onBytes(done, total); };
	const get = async (addr, n) => {
		const b = await read(addr, n);
		done += n;
		onBytes(done, total);
		return b;
	};

	plan(Math.ceil(AT.nZones / 8) + AT.nChannels / 8);
	const zoneSet = await get(MAP.zones, Math.ceil(AT.nZones / 8));
	const chSet = await get(MAP.channels, AT.nChannels / 8);
	// an erased radio has every bit set: a zone needs a name too
	const zones = [];
	const named = [...Array(AT.nZones).keys()].filter((z) => bit(zoneSet, z));
	plan(named.length * AT.zoneName);
	for (const z of named) {
		const name = text(await get(AT.zoneNames + z * AT.zoneName, AT.zoneName), 0, 17);
		if (name) zones.push({ index: z, name });
	}
	plan(zones.length * AT.zoneSlots * 2);
	for (const z of zones) {
		const r = await get(AT.zoneMembers + z.index * AT.zoneRec, AT.zoneSlots * 2);
		z.channels = [];
		for (let i = 0; i < AT.zoneSlots; i++) {
			const n = u16(r, 2 * i);
			if (n < AT.nChannels && bit(chSet, n)) z.channels.push(n);
		}
	}

	const wanted = [...new Set(zones.flatMap((z) => z.channels))].sort((a, b) => a - b);
	plan(wanted.length * AT.chHalf);
	const channels = [];
	for (const n of wanted) {
		const base = AT.channels + Math.floor(n / AT.chPerPair) * PAIR;
		const c = readChannel(await get(base + (n % AT.chPerPair) * AT.chHalf, AT.chHalf));
		if (c) channels.push({ index: n, ...c });
	}

	const digital = channels.filter((c) => c.type !== 0);
	let listIdx = [...new Set(digital.map((c) => c.rxList).filter((i) => i < AT.nLists))];
	const lists = [];
	if (listIdx.length) {
		plan(Math.ceil(AT.nLists / 8));
		const listSet = await get(MAP.lists, Math.ceil(AT.nLists / 8));
		// the radio drops a channel's reference to a list that doesn't exist
		listIdx = listIdx.filter((i) => bit(listSet, i));
		plan(listIdx.length * (AT.listSlots * 4 + 17));
		for (const i of listIdx) {
			const r = await get(AT.lists + i * AT.listRec, AT.listSlots * 4);
			const nm = await get(AT.lists + i * AT.listRec + AT.listName, 17);
			const contacts = [];
			for (let k = 0; k < AT.listSlots; k++) {
				const x = u32(r, 4 * k);
				if (x >= CONTACT_END) break;
				contacts.push(x);
			}
			lists.push({ index: i, name: text(nm, 0, 17) || `Group List ${i + 1}`, contacts });
		}
	}

	let contactIdx = [...new Set([...digital.map((c) => c.contact), ...lists.flatMap((l) => l.contacts)])]
		.filter((i) => i < AT.nContacts).sort((a, b) => a - b);
	const contacts = [];
	if (contactIdx.length) {
		plan(AT.nContacts / 8);
		const free = await get(MAP.contacts, AT.nContacts / 8);
		contactIdx = contactIdx.filter((i) => !bit(free, i));
		plan(contactIdx.length * AT.contactUsed);
		for (const i of contactIdx) {
			const base = AT.contacts + Math.floor(i / AT.perSlot) * PAIR;
			const c = readContact(await get(base + (i % AT.perSlot) * AT.contactRec, AT.contactUsed));
			if (c) contacts.push({ index: i, ...c });
		}
	}

	// The radio ID most channels use is the radio's
	const uses = new Map();
	for (const c of channels) uses.set(c.radioId, (uses.get(c.radioId) || 0) + 1);
	const idIdx = [...uses].sort((a, b) => b[1] - a[1])[0]?.[0];
	let radio = { dmrId: 0, name: "" };
	if (idIdx !== undefined && idIdx < AT.nIds) {
		plan(AT.idRec);
		const r = await get(AT.ids + idIdx * AT.idRec, AT.idRec);
		const id = bcdBe(r, 0);
		radio = { dmrId: id && id <= 16776415 ? id : 0, name: text(r, 5, 17) };
	}
	return { zones, channels, lists, contacts, radio };
}

// The model filled from what readAnytone read, as importQdmr fills it: {counts, notes}. Every table the stock codeplug
// has is replaced; DTMF contacts and APRS configurations are emptied, the VFOs lose their references
export function importAnytone(m, s) {
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
	const mixed = stockChannels.filter((c) => c.type >= 2).length;
	if (mixed) note("mixed_mode", mixed);
	if (stockChannels.some((c) => c.scanList !== NONE)) note("scan_lists");
	const badTones = stockChannels.filter((c) => c.rxTone === null || c.txTone === null).length;
	if (badTones) note("bad_tone", badTones);
	note("zone_only");

	// a mixed channel transmits in its first mode: A+D (2) in FM, D+A (3) in DMR
	const dmrOf = (c) => c.type === 1 || c.type === 3;
	const listRefs = new Set(stockChannels.filter(dmrOf).map((c) => c.rxList));
	const keep = (rows, max, refs) => {
		if (rows.length <= max) return rows;
		const first = rows.filter((r) => refs.has(r.index)).slice(0, max);
		const set = new Set(first);
		for (const r of rows) {
			if (set.size >= max) break;
			set.add(r);
		}
		return rows.filter((r) => set.has(r));
	};
	const stockLists = keep(s.lists, LIMITS.rxLists, listRefs);
	if (s.lists.length > stockLists.length) note("too_many_rxLists", s.lists.length - stockLists.length);
	const contactRefs = new Set(stockChannels.filter(dmrOf).map((c) => c.contact));
	for (const l of stockLists) for (const x of l.contacts) contactRefs.add(x);
	const stockContacts = keep(s.contacts, LIMITS.contacts, contactRefs);
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
		const dmr = dmrOf(c);
		const x = {
			number: i + 1, name: shorten(c.name || `CH ${c.index + 1}`, LIMITS.name), rxHz: c.rxHz, txHz: c.txHz,
			mode: dmr ? "dmr" : "fm", power: [LOW, MID, HIGH, HIGH][c.power], tot: 0,
			rxTone: dmr ? "" : c.rxTone || "", txTone: dmr ? "" : c.txTone || "", bw: dmr ? 12.5 : c.wide ? 25 : 12.5,
			rxOnly: c.rxOnly, allSkip: false, zoneSkip: false, vox: false, squelch: 0,
			colorCode: dmr ? c.colorCode : 0, timeslot: dmr ? c.timeslot : 1,
			contact: dmr ? contactOf.get(c.contact) || null : null, rxList: dmr ? listOf.get(c.rxList) || null : null,
			aprs: null, dmrId: 0, noBeep: false, noEco: false, dmo: false, roam: false, aliasTs1: 0, aliasTs2: 0,
			useLocation: false, lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0,
		};
		channelOf.set(c.index, x);
		return x;
	});

	let zones = s.zones.map((z) => ({ name: shorten(z.name, LIMITS.name), channels: z.channels.map((n) => channelOf.get(n)).filter(Boolean), slot: null }))
		.filter((z) => z.channels.length);
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
	if (s.radio.dmrId) g.dmrId = s.radio.dmrId;
	if (s.radio.name) g.radioName = shorten(s.radio.name, LIMITS.radioName);

	const counts = { channels: channels.length, contacts: contacts.length, rxLists: rxLists.length, zones: zones.length };
	return { counts, notes: [...notes].map(([code, n]) => ({ code, n })) };
}
