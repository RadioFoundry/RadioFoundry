// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Contact lists in the editing model (docs/spec/contact-lists.md): named groups of contacts, the lists each system
// selects, and the network a plain channel points at. The keys live in the settings area's continuation (kv.js), so the
// editor writes exactly the bytes the firmware reads: the same keys, list bytes and table check.
//
// In the model (model.js decode()):
//   m.contactLists   [{number 0-31, name}]
//   m.contactListsVoid  the codeplug's contact tables changed behind the lists' back (3.3): every contact reads as in no
//                    list, and the keys are written back as found until resolveVoid() says the user assigned them again
//   contact.list     a list number 0-31, or null for none
//   system.lists     [primary, secondary]: a list number, LIST_IDDB (the ID database) or null for none
//   channel.network  a Conventional system (type 6, the object), or null

import { crc32 } from "../flasher/bytes.js";
import { decodeChain, putContinuation, putArea, getValue, dedupe, inContinuation, decodeArea, KV_SIZE, KV_HDR } from "./kv.js";
import { CONTACTS, CONTACT_SIZE, AIR_CONTACTS, AIR_CONTACT_SIZE, LIMITS } from "./records.js";
import { isDmr, isP25 } from "./contactmode.js";
import { kindRow, hasFeature } from "./modes.js";
import { CONV_TYPE } from "./mode_conv.js";
import { newSystem } from "./channelmodes.js";

export const LIST_MAX = 32;
export const LIST_NAME_MAX = 16;
export const LIST_NONE = 0xFF;
export const LIST_IDDB = 0xFE;	// the ID database, a selection value only
export const SYSTEM_MAX = 255;
export const SYSTEMS_RUN = 8;	// Tier III systems take the numbers below this (as indexes); a network prefers the rest

// The keys (3): the family bases and the four tables
export const LIST_KEY = { name: 0x1E00, system: 0x1F00, dmr: 0x2000, air: 0x2001, network: 0x2002, check: 0x2003 };

const IDENT = 5;	// bytes of a contact slot the table check covers
const DMR_IDENT_AT = 0x10;

const utf8 = (s) => new TextEncoder().encode(s);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const isNetwork = (s) => !!s && typeof s === "object" && s.type === CONV_TYPE;

// --- The table check (3.3) ---

// The CRC-32 of the contact tables as an image stores them: the 5 identity bytes of each of the 1024 DMR slots (the ID
// as stored, then the call type), then the first 5 bytes of each of the 341 air slots. Names are left out
export function contactTablesCheck(bytes) {
	const dmr = LIMITS.contacts, air = LIMITS.p25Contacts;
	const s = new Uint8Array((dmr + air) * IDENT);
	for (let n = 0; n < dmr; n++) s.set(bytes.subarray(CONTACTS + n * CONTACT_SIZE + DMR_IDENT_AT, CONTACTS + n * CONTACT_SIZE + DMR_IDENT_AT + IDENT), n * IDENT);
	for (let n = 0; n < air; n++) s.set(bytes.subarray(AIR_CONTACTS + n * AIR_CONTACT_SIZE, AIR_CONTACTS + n * AIR_CONTACT_SIZE + IDENT), (dmr + n) * IDENT);
	return crc32(s);
}

const checkValue = (crc) => Uint8Array.of(crc & 0xFF, (crc >>> 8) & 0xFF, (crc >>> 16) & 0xFF, crc >>> 24);

// --- Values (3.1) ---

// A list's name as a key holds it: 1 to 16 bytes with none below 0x20; null for a value that makes no list
export function decodeListName(v) {
	if (!v || v.length < 1 || v.length > LIST_NAME_MAX || v.some((x) => x < 0x20)) return null;
	return new TextDecoder().decode(v);
}

// Whether a name can be stored
export const listNameValid = (name) => {
	const b = utf8(name ?? "");
	return b.length >= 1 && b.length <= LIST_NAME_MAX && b.every((x) => x >= 0x20);
};

// A selection byte: a list number, LIST_IDDB, or null (0xFF and every other value read as none)
const selOf = (x) => (x < LIST_MAX || x === LIST_IDDB ? x : null);
// A membership byte: a list number or null (the ID database is never a member list)
const memberOf = (x) => (x < LIST_MAX ? x : null);

// A system's selection from its key's value: [primary, secondary]; one byte is the primary only
export function decodeSelection(v) {
	if (!v || v.length === 0) return [null, null];
	return [selOf(v[0]), v.length >= 2 ? selOf(v[1]) : null];
}

const sameSel = (a, b) => a[0] === b[0] && a[1] === b[1];

// A membership table's bytes (count slots, 0xFF where the value ends)
const readTable = (v, count) => Uint8Array.from({ length: count }, (_, i) => (v && i < v.length ? v[i] : 0xFF));

// A table with the trailing 0xFF bytes left off
function trimmed(t) {
	let n = t.length;
	while (n > 0 && t[n - 1] === 0xFF) n--;
	return t.slice(0, n);
}

// --- Decoding ---

// The contact lists in an image's bytes into the model, after its contacts, channels and systems are in it
export function decodeContactLists(m, b) {
	const chain = decodeChain(b);
	const entries = chain.state === "valid" ? chain.entries : [];
	m.contactLists = [];
	for (let n = 0; n < LIST_MAX; n++) {
		const name = decodeListName(getValue(entries, LIST_KEY.name + n));
		if (name != null) m.contactLists.push({ number: n, name });
	}
	const dmr = getValue(entries, LIST_KEY.dmr), air = getValue(entries, LIST_KEY.air), check = getValue(entries, LIST_KEY.check);
	// void: a membership key is stored and the check key is not the tables' (3.3)
	m.contactListsVoid = (dmr != null || air != null) && !(check && check.length === 4 && u32(check, 0) === contactTablesCheck(b));
	const dmrT = readTable(dmr, LIMITS.contacts), airT = readTable(air, LIMITS.p25Contacts);
	for (const c of m.contacts) {
		const t = isP25(c) ? airT : isDmr(c) ? dmrT : null;
		c.list = !m.contactListsVoid && t && c.number >= 1 && c.number <= t.length ? memberOf(t[c.number - 1]) : null;
	}
	for (const s of m.systems ?? []) {
		s.lists = s.number >= 1 && s.number <= SYSTEM_MAX ? decodeSelection(getValue(entries, LIST_KEY.system + s.number - 1)) : [null, null];
	}
	const net = getValue(entries, LIST_KEY.network);
	for (const c of m.channels) {
		const x = net && c.number >= 1 && c.number <= net.length ? net[c.number - 1] : 0xFF;
		const s = x < SYSTEM_MAX ? (m.systems ?? []).find((y) => y.number === x + 1) : null;
		c.network = isNetwork(s) ? s : null;
	}
}

// The model's contact lists as the continuation's entries of the contact lists' range. have: the entries the image
// holds (kept byte for byte where the model says the same). check: the table check to store with a live membership
// (the bytes of the image the contacts are written onto)
export function listEntries(m, have, check) {
	const out = [];
	const put = (key, value) => out.push({ key, value });
	const val = (key) => getValue(have, key);
	for (const l of [...(m.contactLists ?? [])].sort((x, y) => x.number - y.number)) {
		if (Number.isInteger(l.number) && l.number >= 0 && l.number < LIST_MAX && listNameValid(l.name)) put(LIST_KEY.name + l.number, utf8(l.name));
	}
	for (const s of [...(m.systems ?? [])].sort((x, y) => x.number - y.number)) {
		if (!(s.number >= 1 && s.number <= SYSTEM_MAX)) continue;
		const sel = (s.lists ?? [null, null]).map((x) => (x != null && selOf(x) != null ? x : null));
		if (sel[0] == null && sel[1] == null) continue;
		const key = LIST_KEY.system + s.number - 1, was = val(key);
		if (was && sameSel(decodeSelection(was), sel)) put(key, was);
		else put(key, sel[1] == null ? Uint8Array.of(sel[0]) : Uint8Array.of(sel[0] ?? LIST_NONE, sel[1]));
	}
	const hd = val(LIST_KEY.dmr), ha = val(LIST_KEY.air), hc = val(LIST_KEY.check);
	if (m.contactListsVoid) {
		// the keys as found: only the editor that sees the contacts and the lists together repairs them
		for (const [key, v] of [[LIST_KEY.dmr, hd], [LIST_KEY.air, ha], [LIST_KEY.check, hc]]) if (v) put(key, v);
	} else {
		let any = false;
		for (const [key, pool, count, was] of [[LIST_KEY.dmr, m.contacts.filter(isDmr), LIMITS.contacts, hd], [LIST_KEY.air, m.contacts.filter(isP25), LIMITS.p25Contacts, ha]]) {
			const t = new Uint8Array(count).fill(LIST_NONE);
			for (const c of pool) if (c.number >= 1 && c.number <= count && c.list != null && c.list >= 0 && c.list < LIST_MAX) t[c.number - 1] = c.list;
			const w = trimmed(t);
			if (!w.length) continue;
			any = true;
			const old = was ? Array.from(readTable(was, count), (x) => memberOf(x) ?? LIST_NONE) : null;
			put(key, old && old.every((x, i) => x === t[i]) ? was : w);
		}
		if (any) put(LIST_KEY.check, check ?? new Uint8Array(4));
		else if (!hd && !ha && hc) put(LIST_KEY.check, hc);
	}
	const net = new Uint8Array(LIMITS.channels).fill(LIST_NONE);
	for (const c of m.channels) {
		if (c.number >= 1 && c.number <= LIMITS.channels && isNetwork(c.network) && c.network.number >= 1 && c.network.number <= SYSTEM_MAX) net[c.number - 1] = c.network.number - 1;
	}
	const w = trimmed(net);
	if (w.length) put(LIST_KEY.network, w);
	return out;
}

// The model's contact lists written onto an image's bytes (the contacts already written, which the check covers). Nothing
// is written when the model has none and the image has no continuation. A first sector of a newer version is left alone
// (validate() warns)
export function encodeContactLists(m, b) {
	if (m.contactLists === undefined) return;
	const first = decodeArea(b);
	if (first.state === "newer") return;
	const chain = decodeChain(b);
	const have = chain.entries.filter((e) => inContinuation(e.key));
	const out = listEntries(m, have, checkValue(contactTablesCheck(b)));
	// a contact list key in the first sector would be read before the continuation's: out of the way
	if (first.entries.some((e) => inContinuation(e.key))) putArea(b, dedupe(first.entries).filter((e) => !inContinuation(e.key)));
	putContinuation(b, out);
}

// The octets the continuation would take with the model written, and the most it holds (3.2)
export function contactListsUse(m, b) {
	const chain = decodeChain(b);
	const have = chain.entries.filter((e) => inContinuation(e.key));
	const keys = listEntries(m, have, new Uint8Array(4));
	const sector = chain.sectors[1];
	const rest = sector?.state === "valid" ? dedupe(sector.entries).filter((e) => !inContinuation(e.key)) : [];
	const hdr = sector?.state === "valid" ? b[sector.addr + 5] : KV_HDR;
	return { used: [...rest, ...keys].reduce((n, e) => n + 4 + e.value.length, 0), max: KV_SIZE - hdr };
}

// --- Editing ---

// Contacts and lists settle: the user has assigned the contacts again, so the next write stores a new check (3.3)
export function resolveVoid(m) {
	m.contactListsVoid = false;
}

// The lowest list number in 0-31 no list has, or null
export function freeListNumber(m) {
	const taken = new Set((m.contactLists ?? []).map((l) => l.number));
	for (let n = 0; n < LIST_MAX; n++) if (!taken.has(n)) return n;
	return null;
}

// A new contact list with a name: the object, null when all 32 numbers are taken
export function newContactList(m, name) {
	const number = freeListNumber(m);
	if (number == null) return null;
	const l = { number, name };
	m.contactLists = [...(m.contactLists ?? []), l].sort((x, y) => x.number - y.number);
	return l;
}

// The systems that select a list (by number), as [{system, slot}]: a list can't go while one does
export function listUsers(m, list) {
	const out = [];
	for (const s of m.systems ?? []) (s.lists ?? []).forEach((x, slot) => x === list.number && out.push({ system: s, slot }));
	return out;
}

// Remove lists: their contacts are in none, and nothing else changes. Returns false, removing nothing, when a system
// selects one of them
export function removeLists(m, lists) {
	const gone = new Set(lists.map((l) => l.number));
	if ((m.systems ?? []).some((s) => (s.lists ?? []).some((x) => gone.has(x)))) return false;
	m.contactLists = (m.contactLists ?? []).filter((l) => !lists.includes(l));
	for (const c of m.contacts) if (gone.has(c.list)) c.list = null;
	return true;
}

// Whether the editor offers contact lists: no radio to ask (features null), a radio running the feature, or a codeplug
// that already has lists or networks (which stay editable)
export const listsOffered = (m, features) => features == null || hasFeature(features, "CONTACT_LISTS") ||
	(m.contactLists ?? []).length > 0 || (m.systems ?? []).some(isNetwork);

// Whether a channel points at a network: a plain DMR channel, or a P25 channel with no system of its own (the modes
// the radio names calls in). Other channels keep the field but nothing reads it for them
export function takesNetwork(c) {
	if (c.system) return false;
	return c.kind ? kindRow(c.kind)?.token === "P25" : c.mode === "dmr";
}

// The free system number a new network takes: the lowest from SYSTEMS_RUN + 1 up (a Tier III system's numbers come
// first), else the lowest free one. null when all 255 are taken
export function freeNetworkNumber(m) {
	const taken = new Set((m.systems ?? []).map((s) => s.number));
	for (let n = SYSTEMS_RUN + 1; n <= SYSTEM_MAX; n++) if (!taken.has(n)) return n;
	for (let n = 1; n <= SYSTEMS_RUN; n++) if (!taken.has(n)) return n;
	return null;
}

// A new network (a Conventional system): selecting the lists, with no number assigned unless one is free
export function newNetwork(m, name, lists = [null, null]) {
	const s = newSystem(m, CONV_TYPE, name);
	s.number = freeNetworkNumber(m);
	s.lists = [...lists];
	return s;
}

// --- Migration (6) ---

// Whether a codeplug from before contact lists should be offered the Default network: it has contacts or channels that
// would take one and neither a list nor a network
export function needsContactListMigration(m) {
	if (m.contactLists === undefined || m.contactListsVoid) return false;
	if (m.contactLists.length || (m.systems ?? []).some(isNetwork)) return false;
	return m.contacts.some((c) => isDmr(c) || isP25(c)) || m.channels.some(takesNetwork);
}

// The Default network and list: a list "Default" holding every contact, a Conventional system "Default" selecting it
// and then the ID database, and every channel that takes a network pointing at it. Returns {list, system, contacts,
// channels}, or null (changing nothing) when there is no number left for the list or the system
export function makeDefaultNetwork(m, name = "Default") {
	m.contactLists ??= [];
	if (freeListNumber(m) == null || freeNetworkNumber(m) == null) return null;
	const list = newContactList(m, name);
	const system = newNetwork(m, name, [list.number, LIST_IDDB]);
	m.systems.push(system);
	let contacts = 0, channels = 0;
	for (const c of m.contacts) {
		if (!isDmr(c) && !isP25(c)) continue;
		c.list = list.number;
		contacts++;
	}
	for (const c of m.channels) {
		if (!takesNetwork(c)) continue;
		c.network = system;
		channels++;
	}
	m.contactListsVoid = false;
	return { list, system, contacts, channels };
}

// --- Validation ---

// Problems of the lists, in validate()'s terms
export function validateContactLists(m, err, warn) {
	const lists = m.contactLists;
	if (lists === undefined) return;
	if (lists.length > LIST_MAX) err("contactLists", null, null, "too_many", { max: LIST_MAX, count: lists.length });
	const seen = new Map(), numbers = new Set();
	for (const l of lists) {
		if (!Number.isInteger(l.number) || l.number < 0 || l.number >= LIST_MAX || numbers.has(l.number)) err("contactLists", l, "number", "range", { min: 0, max: LIST_MAX - 1 });
		numbers.add(l.number);
		const bytes = utf8(l.name ?? "");
		if (!l.name) err("contactLists", l, "name", "no_name");
		else if (bytes.length > LIST_NAME_MAX) err("contactLists", l, "name", "too_long", { max: LIST_NAME_MAX });
		else if (bytes.some((x) => x < 0x20)) err("contactLists", l, "name", "list_name_chars");
		seen.set(l.name, (seen.get(l.name) || 0) + 1);
	}
	for (const l of lists) if (l.name && seen.get(l.name) > 1) warn("contactLists", l, "name", "list_name_duplicate");
	for (const c of m.contacts) {
		if (c.list == null) continue;
		if (!Number.isInteger(c.list) || c.list < 0 || c.list >= LIST_MAX) err("contacts", c, "list", "range", { min: 0, max: LIST_MAX - 1 });
		else if (!numbers.has(c.list)) warn("contacts", c, "list", "list_missing");
	}
	for (const s of m.systems ?? []) {
		for (const x of s.lists ?? []) {
			if (x == null) continue;
			if (selOf(x) == null) err("systems", s, "lists", "range", { min: 0, max: LIST_MAX - 1 });
			else if (x !== LIST_IDDB && !numbers.has(x)) err("systems", s, "lists", "list_missing");
		}
	}
	for (const c of m.channels) {
		if (c.network == null) continue;
		if (!isNetwork(c.network)) err("channels", c, "network", "network_type");
		else if (!(m.systems ?? []).includes(c.network)) err("channels", c, "network", "network_missing");
	}
	if (m.contactListsVoid) warn("contacts", null, "list", "lists_void");
	if (!m.base) return;
	const state = decodeArea(m.base.bytes).state;
	const any = lists.length || (m.systems ?? []).some((s) => (s.lists ?? []).some((x) => x != null)) || m.channels.some((c) => c.network);
	if (state === "newer") {
		if (any) warn("contactLists", null, null, "contact_lists_area_newer");
		return;
	}
	if (!any) return;
	const { used, max } = contactListsUse(m, m.base.bytes);
	if (used > max) err("contactLists", null, null, "contact_lists_area_full", { used, max });
}
