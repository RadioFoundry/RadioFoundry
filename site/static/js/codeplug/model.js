// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The editing model: a whole codeplug as plain objects. Records keep their number (channel 1..1024, contact
// 1..1024, list 1..76, DTMF contact 1..63, APRS configuration 1..8) so an edit rewrites only what changed. References
// are the referenced objects themselves, so deleting or renumbering a record can't leave a reference pointing at the
// wrong one; a reference to a record that isn't stored stays a plain number, as read. encode() writes the model onto
// the image it came from and touches only records whose fields changed, so an unedited codeplug round-trips byte for
// byte.

import { CodeplugImage } from "./image.js";
import {
	LIMITS, CH_SIZE, bankBitmap, channelAddr, decodeChannel, encodeChannel, zoneLayout, decodeZone, encodeZone,
	ZONE_BITMAP, ZONE_DATA, ZONE_RECORD, CONTACTS, CONTACT_SIZE, decodeContact, encodeContact, LIST_COUNTS, LISTS,
	LIST_SIZE, decodeList, encodeList, DTMF_CONTACTS, decodeDtmf, encodeDtmf, validDtmf, APRS_BASE, APRS_SIZE,
	decodeAprs, encodeAprs, decodeGeneral, encodeGeneral, VFO, LUCZ,
} from "./records.js";
import { parseTone } from "./fields.js";
import { readHeader } from "./iddb.js";

const REFS = ["contact", "rxList", "aprs"];

export function decode(image) {
	const b = image.bytes;
	const m = {
		general: decodeGeneral(b),
		channels: [], vfo: [], zones: [], contacts: [], rxLists: [], dtmfContacts: [], aprs: [],
		iddb: readHeader(b),
		base: image,
	};
	for (let n = 1; n <= LIMITS.contacts; n++) {
		const c = decodeContact(b.subarray(CONTACTS + (n - 1) * CONTACT_SIZE, CONTACTS + n * CONTACT_SIZE));
		if (c) m.contacts.push({ number: n, ...c });
	}
	for (let n = 1; n <= LIMITS.rxLists; n++) {
		const count = b[LIST_COUNTS + n - 1];
		if (count === 0 || count === 0xFF) continue;
		const l = decodeList(b.subarray(LISTS + (n - 1) * LIST_SIZE, LISTS + n * LIST_SIZE), count);
		m.rxLists.push({ number: n, name: l.name, contacts: l.contacts.map((x) => find(m.contacts, x)) });
	}
	for (let n = 1; n <= LIMITS.aprs; n++) {
		const a = decodeAprs(b.subarray(APRS_BASE + (n - 1) * APRS_SIZE, APRS_BASE + n * APRS_SIZE));
		if (a) m.aprs.push({ number: n, ...a });
	}
	for (let n = 1; n <= LIMITS.dtmfContacts; n++) {
		const d = decodeDtmf(b.subarray(DTMF_CONTACTS + (n - 1) * 32, DTMF_CONTACTS + n * 32));
		if (d) m.dtmfContacts.push({ number: n, ...d });
	}
	for (let n = 1; n <= LIMITS.channels; n++) {
		if (!channelUsed(b, n)) continue;
		const c = decodeChannel(b.subarray(channelAddr(n), channelAddr(n) + CH_SIZE));
		if (c) m.channels.push(linkRefs(m, { number: n, ...c }));
	}
	for (const a of VFO) {
		const c = decodeChannel(b.subarray(a, a + CH_SIZE));
		m.vfo.push(c ? linkRefs(m, c) : null);
	}
	const per = zoneLayout(b);
	const rec = 16 + 2 * per;
	for (let slot = 0; slot < LIMITS.zones; slot++) {
		if (!(b[ZONE_BITMAP + (slot >> 3)] & (1 << (slot & 7)))) continue;
		const r = b.subarray(ZONE_DATA + slot * rec, ZONE_DATA + (slot + 1) * rec);
		if (r[0] === 0x00 || r[0] === 0xFF) continue;
		const z = decodeZone(r, per);
		m.zones.push({ name: z.name, channels: z.channels.map((x) => find(m.channels, x)), slot });
	}
	return m;
}

export function empty() {
	return decode(CodeplugImage.blank());
}

function channelUsed(b, n) {
	const bank = Math.floor((n - 1) / 128), k = (n - 1) % 128;
	return (b[bankBitmap(bank) + (k >> 3)] & (1 << (k & 7))) !== 0;
}

function find(list, n) {
	return list.find((x) => x.number === n) || n;
}

function linkRefs(m, c) {
	c.contact = c.contact ? find(m.contacts, c.contact) : null;
	c.rxList = c.rxList ? find(m.rxLists, c.rxList) : null;
	c.aprs = c.aprs ? find(m.aprs, c.aprs) : null;
	return c;
}

// The number a reference stands for: the object's, a dangling number as read, or 0
export function refNumber(x) {
	return x == null ? 0 : typeof x === "number" ? x : x.number || 0;
}

// Every record of a table without a number, or with one another record already has, gets the lowest free number
export function assignNumbers(list, max) {
	const taken = new Set();
	const todo = [];
	for (const x of list) {
		if (Number.isInteger(x.number) && x.number >= 1 && x.number <= max && !taken.has(x.number)) taken.add(x.number);
		else todo.push(x);
	}
	let n = 1;
	for (const x of todo) {
		while (taken.has(n)) n++;
		x.number = n <= max ? n : null;
		taken.add(n);
	}
}

export function numberAll(m) {
	assignNumbers(m.channels, LIMITS.channels);
	assignNumbers(m.contacts, LIMITS.contacts);
	assignNumbers(m.rxLists, LIMITS.rxLists);
	assignNumbers(m.dtmfContacts, LIMITS.dtmfContacts);
	assignNumbers(m.aprs, LIMITS.aprs);
}

function same(a, b, keys) {
	if (!a) return false;
	return keys.every((k) => a[k] === b[k] || (typeof a[k] === "number" && Math.abs(a[k] - b[k]) < 0.00005));
}

const CHANNEL_KEYS = [
	"name", "rxHz", "txHz", "mode", "power", "tot", "rxTone", "txTone", "bw", "rxOnly", "allSkip", "zoneSkip", "vox",
	"squelch", "colorCode", "timeslot", "contact", "rxList", "aprs", "dmrId", "noBeep", "noEco", "dmo", "roam",
	"aliasTs1", "aliasTs2", "useLocation", "lat", "lon", "sfr", "sfrA", "sfrB",
];

function flatChannel(c) {
	const out = { ...c };
	for (const k of REFS) out[k] = refNumber(c[k]);
	return out;
}

function putChannel(r, c) {
	const flat = flatChannel(c);
	if (!same(decodeChannel(r), flat, CHANNEL_KEYS)) encodeChannel(flat, r);
}

// The model written onto a copy of the image it was decoded from: the new image
export function encode(m) {
	numberAll(m);
	const img = m.base.clone();
	const b = img.bytes;

	// Channels: records first, then the in-use bitmaps
	const bits = new Uint8Array(LIMITS.channels / 8);
	for (const c of m.channels) {
		if (!c.number) continue;
		bits[(c.number - 1) >> 3] |= 1 << ((c.number - 1) & 7);
		putChannel(b.subarray(channelAddr(c.number), channelAddr(c.number) + CH_SIZE), c);
	}
	for (let bank = 0; bank < 8; bank++) b.set(bits.subarray(bank * 16, bank * 16 + 16), bankBitmap(bank));
	m.vfo.forEach((c, i) => c && putChannel(b.subarray(VFO[i], VFO[i] + CH_SIZE), c));

	// Zones: packed into slots 0..n-1 in model order, the 80-channel layout. A 16-channel table is rewritten whole
	const per = zoneLayout(b);
	if (per !== 80 && m.zones.length) b.fill(0, ZONE_DATA, ZONE_DATA + LIMITS.zones * ZONE_RECORD);
	const zones = m.zones.slice(0, LIMITS.zones);
	zones.forEach((z, slot) => {
		const r = b.subarray(ZONE_DATA + slot * ZONE_RECORD, ZONE_DATA + (slot + 1) * ZONE_RECORD);
		const want = { name: z.name, channels: z.channels.map(refNumber).filter((n) => n) };
		const have = per === 80 ? decodeZone(r, 80) : null;
		if (!have || have.name !== want.name || have.channels.join() !== want.channels.join()) encodeZone(want, r);
		z.slot = slot;
	});
	for (let slot = 0; slot < LIMITS.zones; slot++) {
		const a = ZONE_BITMAP + (slot >> 3), bit = 1 << (slot & 7);
		b[a] = slot < zones.length ? b[a] | bit : b[a] & ~bit;
	}

	// Contacts: a number without a contact is erased, as the radio deletes one
	const contacts = new Map(m.contacts.filter((c) => c.number).map((c) => [c.number, c]));
	for (let n = 1; n <= LIMITS.contacts; n++) {
		const r = b.subarray(CONTACTS + (n - 1) * CONTACT_SIZE, CONTACTS + n * CONTACT_SIZE);
		const c = contacts.get(n), have = decodeContact(r);
		if (c && !same(have, c, ["name", "id", "type", "timeslot"])) encodeContact(c, r);
		else if (!c && have) r.fill(0xFF);
	}

	// RX group lists: the record, then the count that makes it visible
	const lists = new Map(m.rxLists.filter((l) => l.number).map((l) => [l.number, l]));
	for (let n = 1; n <= LIMITS.rxLists; n++) {
		const r = b.subarray(LISTS + (n - 1) * LIST_SIZE, LISTS + n * LIST_SIZE);
		const l = lists.get(n), count = b[LIST_COUNTS + n - 1];
		if (!l) {
			if (count !== 0 && count !== 0xFF) b[LIST_COUNTS + n - 1] = 0;
			continue;
		}
		const want = { name: l.name, contacts: l.contacts.map(refNumber).filter((x) => x).slice(0, 32) };
		const have = count !== 0 && count !== 0xFF ? decodeList(r, count) : null;
		if (!have || have.name !== want.name || have.contacts.join() !== want.contacts.join()) {
			encodeList(want, r);
			b[LIST_COUNTS + n - 1] = want.contacts.length + 1;
		}
	}

	const dtmf = new Map(m.dtmfContacts.filter((d) => d.number).map((d) => [d.number, d]));
	for (let n = 1; n <= LIMITS.dtmfContacts; n++) {
		const r = b.subarray(DTMF_CONTACTS + (n - 1) * 32, DTMF_CONTACTS + n * 32);
		const d = dtmf.get(n), have = decodeDtmf(r);
		if (d && !same(have, d, ["name", "code"])) encodeDtmf(d, r);
		else if (!d && have) r[0] = 0xFF;
	}

	const aprs = new Map(m.aprs.filter((a) => a.number).map((a) => [a.number, a]));
	for (let n = 1; n <= LIMITS.aprs; n++) {
		const r = b.subarray(APRS_BASE + (n - 1) * APRS_SIZE, APRS_BASE + n * APRS_SIZE);
		const a = aprs.get(n), have = decodeAprs(r);
		const keys = ["name", "ssid", "lat", "lon", "path1", "path2", "symTable", "symCode", "comment", "txHz", "baud300",
			"useFixed", "qsy", "silent", "ambiguity"];
		if (a && !same(have, a, keys)) encodeAprs(a, r);
		else if (!a && have) r[0] = 0xFF;
	}

	encodeGeneral(m.general, b);
	fixLucz(b, m);
	return img;
}

// The last used channel of each zone (7.3.10) must stay inside the zone; All Channels' must be a stored channel
function fixLucz(b, m) {
	if (String.fromCharCode(...b.subarray(LUCZ, LUCZ + 4)) !== "LUCZ") return;
	m.zones.forEach((z, slot) => {
		if (b[LUCZ + 4 + slot] >= z.channels.length) b[LUCZ + 4 + slot] = 0;
	});
	const all = b[LUCZ + 72] | (b[LUCZ + 73] << 8);
	if (!m.channels.some((c) => c.number === all + 1)) {
		b[LUCZ + 72] = 0;
		b[LUCZ + 73] = 0;
	}
}

// --- Editing helpers ---

export function newChannel(fields = {}) {
	return {
		number: null, name: "", rxHz: 446006250, txHz: 446006250, mode: "fm", power: 0, tot: 0, rxTone: "",
		txTone: "", bw: 12.5, rxOnly: false, allSkip: false, zoneSkip: false, vox: false, squelch: 0, colorCode: 1,
		timeslot: 1, contact: null, rxList: null, aprs: null, dmrId: 0, noBeep: false, noEco: false, dmo: false,
		roam: false, aliasTs1: 0, aliasTs2: 0, useLocation: false, lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0,
		...fields,
	};
}

export function newContact(fields = {}) {
	return { number: null, name: "", id: 0, type: "group", timeslot: 0, ...fields };
}

// Remove records and every reference to them
export function remove(m, table, items) {
	const gone = new Set(items);
	m[table] = m[table].filter((x) => !gone.has(x));
	if (table === "channels") {
		for (const z of m.zones) z.channels = z.channels.filter((c) => !gone.has(c));
	} else if (table === "contacts") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.contact)) c.contact = null;
		for (const l of m.rxLists) l.contacts = l.contacts.filter((c) => !gone.has(c));
	} else if (table === "rxLists") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.rxList)) c.rxList = null;
	} else if (table === "aprs") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.aprs)) c.aprs = null;
	}
}

// --- Validation ---

// Problems as {level: "error" | "warning", table, item, field, code, params}; the page words them
export function validate(m, { bands = null } = {}) {
	const out = [];
	const err = (table, item, field, code, params = {}) => out.push({ level: "error", table, item, field, code, params });
	const warn = (table, item, field, code, params = {}) => out.push({ level: "warning", table, item, field, code, params });
	const g = m.general;
	if (!g.dmrId) warn("general", null, "dmrId", "no_dmr_id");
	else if (g.dmrId > 0xFFFFFF) err("general", null, "dmrId", "id_range");
	if (g.radioName.length > LIMITS.radioName) err("general", null, "radioName", "too_long", { max: LIMITS.radioName });

	const counts = [["channels", LIMITS.channels], ["contacts", LIMITS.contacts], ["rxLists", LIMITS.rxLists],
		["zones", LIMITS.zones], ["dtmfContacts", LIMITS.dtmfContacts], ["aprs", LIMITS.aprs]];
	for (const [t, max] of counts) if (m[t].length > max) err(t, null, null, "too_many", { max, count: m[t].length });

	for (const c of m.channels) {
		if (!c.name) err("channels", c, "name", "no_name");
		if (c.name.length > LIMITS.name) err("channels", c, "name", "too_long", { max: LIMITS.name });
		for (const f of ["rxHz", "txHz"]) {
			if (!(c[f] > 0 && c[f] < 1e9)) err("channels", c, f, "bad_freq");
			else if (bands && !bands.some(([lo, hi]) => c[f] >= lo && c[f] <= hi)) warn("channels", c, f, "out_of_band");
		}
		for (const f of ["rxTone", "txTone"]) if (parseTone(c[f]) === null) err("channels", c, f, "bad_tone");
		if (c.colorCode < 0 || c.colorCode > 15) err("channels", c, "colorCode", "range", { min: 0, max: 15 });
		if (c.mode === "dmr" && !c.contact && !c.rxOnly) warn("channels", c, "contact", "no_contact");
		for (const k of REFS) if (typeof c[k] === "number") warn("channels", c, k, "dangling", { number: c[k] });
		if (c.dmrId > 0xFFFFFF) err("channels", c, "dmrId", "id_range");
	}
	const names = new Map();
	for (const c of m.channels) names.set(c.name, (names.get(c.name) || 0) + 1);
	for (const c of m.channels) if (c.name && names.get(c.name) > 1) warn("channels", c, "name", "duplicate");

	for (const z of m.zones) {
		if (!z.name) err("zones", z, "name", "no_name");
		if (z.name.length > LIMITS.name) err("zones", z, "name", "too_long", { max: LIMITS.name });
		if (z.channels.length > LIMITS.zoneChannels) err("zones", z, "channels", "too_many", { max: LIMITS.zoneChannels, count: z.channels.length });
		if (!z.channels.length) warn("zones", z, "channels", "empty");
	}
	for (const c of m.contacts) {
		if (!c.name) err("contacts", c, "name", "no_name");
		if (c.name.length > LIMITS.name) err("contacts", c, "name", "too_long", { max: LIMITS.name });
		if (c.type !== "all" && !(c.id >= 1 && c.id <= 0xFFFFFF)) err("contacts", c, "id", "id_range");
	}
	for (const l of m.rxLists) {
		if (!l.name) err("rxLists", l, "name", "no_name");
		if (l.contacts.length > LIMITS.rxListContacts) err("rxLists", l, "contacts", "too_many", { max: LIMITS.rxListContacts, count: l.contacts.length });
	}
	for (const d of m.dtmfContacts) {
		if (!d.name) err("dtmfContacts", d, "name", "no_name");
		if (!validDtmf(d.code)) err("dtmfContacts", d, "code", "bad_dtmf");
	}
	for (const a of m.aprs) {
		if (!a.name) err("aprs", a, "name", "no_name");
		if (a.name.length > LIMITS.aprsName) err("aprs", a, "name", "too_long", { max: LIMITS.aprsName });
		if (a.comment.length > LIMITS.aprsComment) err("aprs", a, "comment", "too_long", { max: LIMITS.aprsComment });
	}
	return out;
}
