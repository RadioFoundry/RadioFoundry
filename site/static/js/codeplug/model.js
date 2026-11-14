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
import { decodeArea, keyValid, KEYS, KV_ADDR, KV_SIZE, KV_HDR, getValue, setValue, deleteValue, dedupe, putArea } from "./kv.js";
import {
	T3_KEY, T3_LIMITS, nameCheck, decodeSystem, encodeSystem, SYSTEM_KEYS, decodeChan, encodeChan, decodeKey, encodeKey,
	decodePoll, encodePoll, encodePollReply, POLL_DEFAULT, POLL_MAX, decodeSupl, encodeSupl, validKey, decodeTag, encodeTag, netMax, siteMax, ID_MAX,
} from "./trunk.js";

const REFS = ["contact", "rxList", "aprs"];

export function decode(image) {
	const b = image.bytes;
	const m = {
		general: decodeGeneral(b),
		channels: [], vfo: [], zones: [], contacts: [], rxLists: [], dtmfContacts: [], aprs: [],
		trunkSystems: [], trunkChannels: [],
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
	decodeTrunk(m, b);
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
	assignNumbers(m.trunkSystems ??= [], T3_LIMITS.systems);
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
	"aliasTs1", "aliasTs2", "useLocation", "lat", "lon", "sfr", "sfrA", "sfrB", "toneBurst",
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
	encodeTrunk(m, b);
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

// --- Tier III trunked systems (docs/spec/dmr-trunking.md 12), in the codeplug settings area ---
//
// m.trunkSystems: {number: 1-8 (key 0x0100 + number - 1), the fields of trunk.js newSystem, fallback: a channel or a
// number as read}. m.trunkChannels: the system channel records in key order (hunt order), {system: a system or a
// number as read, control, lcn, rxHz, txHz, cc: 0-15 or null for the system's}. A trunked channel is a codeplug
// channel with system set (a system, or a number as read), noFallback, and fallback (a channel, a number as read, or
// null for the system's). A tag the radio wouldn't take (its name check fails) reads as none; a fallback whose check
// fails stays the number stored.

const areaEntries = (b) => {
	const a = decodeArea(b);
	return { state: a.state, entries: a.state === "valid" ? dedupe(a.entries) : [] };
};
const inFamily = (key, base, count) => key >= base && key < base + count;

// The channel a fallback names: the channel when its name check matches, else the number as stored; null for none
function fallbackRef(m, n, check) {
	if (!n) return null;
	const c = m.channels.find((x) => x.number === n);
	return c && nameCheck(c.name) === check ? c : n;
}

function decodeTrunk(m, b) {
	const { entries } = areaEntries(b);
	for (let n = 0; n < T3_LIMITS.systems; n++) {
		const s = decodeSystem(getValue(entries, T3_KEY.system + n));
		if (!s) continue;
		const k = decodeKey(getValue(entries, T3_KEY.key + n));
		const { fallbackCheck, ...rest } = s;
		m.trunkSystems.push({ number: n + 1, ...rest, fallback: fallbackRef(m, s.fallback, fallbackCheck), key: k?.key ?? "", psn: k?.psn ?? 0,
			...decodePoll(getValue(entries, T3_KEY.poll + n)), ...decodeSupl(getValue(entries, T3_KEY.supl + n)) });
	}
	const chans = entries.filter((e) => inFamily(e.key, T3_KEY.chan, T3_LIMITS.chanKeys)).sort((a, b) => a.key - b.key);
	for (const e of chans) {
		const c = decodeChan(e.value);
		if (c) m.trunkChannels.push({ ...c, system: find(m.trunkSystems, c.system + 1) });
	}
	for (const c of m.channels) {
		const tag = liveTag(getValue(entries, T3_KEY.tag + c.number - 1), c.name);
		if (!tag) continue;
		c.system = find(m.trunkSystems, tag.system + 1);
		c.noFallback = tag.noFallback;
		c.fallback = fallbackRef(m, tag.fallback, tag.fallbackCheck);
	}
}

// A tag the radio takes for a channel of this name, decoded; null otherwise
function liveTag(v, name) {
	const tag = decodeTag(v);
	return tag && name != null && tag.system < T3_LIMITS.systems && tag.check === nameCheck(name) ? tag : null;
}

// A fallback reference as stored: its number and name check. A number as read keeps the check stored with it (prev:
// the record before)
function fallbackOut(ref, prev) {
	if (!ref) return { fallback: 0, fallbackCheck: 0 };
	if (typeof ref === "object") return { fallback: ref.number || 0, fallbackCheck: ref.number ? nameCheck(ref.name) : 0 };
	return { fallback: ref, fallbackCheck: prev && prev.fallback === ref ? prev.fallbackCheck : 0 };
}

const sameFields = (a, b, keys) => keys.every((k) => (Array.isArray(a[k]) ? a[k].join() === (b[k] ?? []).join() : a[k] === b[k]));
const TAG_KEYS = ["system", "noFallback", "check", "fallback", "fallbackCheck"];
const CHAN_KEYS = ["system", "control", "lcn", "rxHz", "txHz", "cc"];

// The settings area's entries with the model's systems, channels, keys and tags in place of those stored. A record
// left as it was keeps its bytes, and the system channels keep their keys unless one changed. base: the image the
// model came from. A tag it held that the radio didn't take is kept as found, unless it would now name a channel
export function trunkEntries(m, entries, base = m.base?.bytes) {
	const systems = (m.trunkSystems ?? []).filter((s) => s.number >= 1 && s.number <= T3_LIMITS.systems);
	const sysIndex = (ref) => ((typeof ref === "number" ? ref : ref.number) - 1) & 0xFF;
	let out = entries;
	const set = (key, v) => (out = setValue(out, key, v));
	const del = (key) => (out = deleteValue(out, key));

	for (let n = 0; n < T3_LIMITS.systems; n++) {
		const s = systems.find((x) => x.number === n + 1);
		const prev = getValue(out, T3_KEY.system + n), was = decodeSystem(prev);
		if (!s) {
			// a system the model no longer has was deleted, its key with it
			if (was) {
				del(T3_KEY.system + n);
				del(T3_KEY.key + n);
				del(T3_KEY.poll + n);
				del(T3_KEY.supl + n);
			}
			continue;
		}
		// The poll record: rewritten only when its bytes change; one that doesn't encode (validate says why) stays
		const pollWas = getValue(out, T3_KEY.poll + n), poll = encodePoll(s);
		const pollDefault = s.pollReply === "" && s.pollFormat === POLL_DEFAULT.pollFormat && s.pollBlocks === POLL_DEFAULT.pollBlocks;
		if (poll && !(pollWas && pollWas.length === poll.length && pollWas.every((x, i) => x === poll[i]))) set(T3_KEY.poll + n, poll);
		else if (!poll && pollDefault && pollWas) del(T3_KEY.poll + n);
		// The supplementary data record, the same way; no data: no record
		const suplWas = getValue(out, T3_KEY.supl + n), supl = encodeSupl(s);
		if (supl && !(suplWas && suplWas.length === supl.length && suplWas.every((x, i) => x === supl[i]))) set(T3_KEY.supl + n, supl);
		else if (!supl && (s.suplData ?? "") === "" && suplWas) del(T3_KEY.supl + n);
		const flat = { ...s, ...fallbackOut(s.fallback, was) };
		if (!was || !sameFields(was, flat, SYSTEM_KEYS)) set(T3_KEY.system + n, encodeSystem(flat, prev));
		const k = decodeKey(getValue(out, T3_KEY.key + n));
		if (!s.key) {
			if (k) del(T3_KEY.key + n);
		} else if (validKey(s.key) && Number.isInteger(s.psn) && s.psn >= 0 && s.psn <= 0xFFFFFF &&
			(!k || k.key !== s.key.toUpperCase() || k.psn !== s.psn)) {
			set(T3_KEY.key + n, encodeKey(s.key, s.psn));
		}
	}

	// System channels: the family rewritten from 0x0200 up when any record changed, else left as it is
	const want = (m.trunkChannels ?? []).slice(0, T3_LIMITS.chanKeys).map((c) => ({ ...c, system: sysIndex(c.system), cc: c.cc ?? null }));
	const have = out.filter((e) => inFamily(e.key, T3_KEY.chan, T3_LIMITS.chanKeys)).sort((a, b) => a.key - b.key);
	const same = have.length === want.length && have.every((e, i) => {
		const d = decodeChan(e.value);
		return d && sameFields(d, want[i], CHAN_KEYS);
	});
	if (!same) {
		out = out.filter((e) => !inFamily(e.key, T3_KEY.chan, T3_LIMITS.chanKeys));
		want.forEach((c, i) => set(T3_KEY.chan + i, encodeChan(c)));
	}

	// Tags: one for each trunked channel, at its number. Any other goes, but for one the radio didn't take in the image
	// the model came from and still wouldn't
	const tagged = new Map();
	for (const c of m.channels) if (c.system && c.number) tagged.set(T3_KEY.tag + c.number - 1, c);
	const named = new Map(m.channels.filter((c) => c.number).map((c) => [c.number, c.name]));
	let baseNames = null, baseTags = null;
	for (const e of out.filter((x) => inFamily(x.key, T3_KEY.tag, LIMITS.channels))) {
		if (tagged.has(e.key)) continue;
		const n = e.key - T3_KEY.tag + 1;
		baseTags ??= base ? areaEntries(base).entries : [];
		baseNames ??= base ? baseChannelNames(base) : new Map();
		const stored = getValue(baseTags, e.key);
		const wasStale = !!stored && !liveTag(stored, baseNames.get(n));
		const staysStale = !liveTag(e.value, named.get(n));
		if (!(wasStale && staysStale)) del(e.key);
	}
	for (const [key, c] of tagged) {
		const prev = getValue(out, key), was = decodeTag(prev);
		const tag = { system: sysIndex(c.system), noFallback: !!c.noFallback, check: nameCheck(c.name), ...fallbackOut(c.fallback, was) };
		if (!was || !sameFields(was, tag, TAG_KEYS)) set(key, encodeTag(tag, prev));
	}
	return out;
}

// The names of the channels an image stores, by number
function baseChannelNames(b) {
	const out = new Map();
	for (let n = 1; n <= LIMITS.channels; n++) {
		if (!channelUsed(b, n)) continue;
		const c = decodeChannel(b.subarray(channelAddr(n), channelAddr(n) + CH_SIZE));
		if (c) out.set(n, c.name);
	}
	return out;
}

// The model's trunking written into the image's settings area, only when it changes the area. An area of a newer
// version is never written (validate() warns)
function encodeTrunk(m, b) {
	const { state, entries } = areaEntries(b);
	if (state === "newer") return;
	const out = trunkEntries(m, entries);
	if (out !== entries) putArea(b, out);
}

// The bytes the settings area's entries would take with the model written, and the most it holds
function areaUse(m) {
	const base = m.base?.bytes;
	const { state, entries } = base ? areaEntries(base) : { state: "empty", entries: [] };
	let out = trunkEntries(m, entries);
	for (const row of KEYS) {
		if (m.general?.[row.name] != null && !getValue(out, row.key)) out = [...out, { key: row.key, value: new Uint8Array(row.size) }];
	}
	const hdr = state === "valid" ? base[KV_ADDR + 5] : KV_HDR;
	return { used: out.reduce((n, e) => n + 4 + e.value.length, 0), max: KV_SIZE - hdr };
}

// The record a trunked channel keeps for tools that don't know tags (12.4): DMR, receive only, RX and TX on its
// system's first control channel
export function trunkRecord(m, c) {
	if (!c.system) return;
	c.mode = "dmr";
	c.rxOnly = true;
	const ctl = typeof c.system === "object" && (m.trunkChannels ?? []).find((x) => x.system === c.system && x.control);
	if (ctl && ctl.rxHz && ctl.txHz) {
		c.rxHz = ctl.rxHz;
		c.txHz = ctl.txHz;
	}
}

// The trunked channels of system s (of every system without s) brought to its record
export function syncTrunk(m, s = null) {
	for (const c of m.channels) if (c.system && (!s || c.system === s)) trunkRecord(m, c);
}

// A channel made trunked on system s, or conventional (s null)
export function setTrunk(m, c, s) {
	if (!s) {
		c.system = null;
		c.noFallback = false;
		c.fallback = null;
		return;
	}
	c.system = s;
	c.noFallback ??= false;
	c.fallback ??= null;
	trunkRecord(m, c);
}

// A system's edited fields and channel list (records without their system) put into the model: a new system is added
// with the lowest free number. Its channels take the place of the ones it had, so the other systems' keep their order;
// its trunked channels follow its first control channel
export function putSystem(m, s, fields, chans) {
	m.trunkSystems ??= [];
	m.trunkChannels ??= [];
	if (!m.trunkSystems.includes(s)) {
		m.trunkSystems.push(s);
		assignNumbers(m.trunkSystems, T3_LIMITS.systems);
	}
	Object.assign(s, fields);
	const at = m.trunkChannels.findIndex((c) => c.system === s);
	const rest = m.trunkChannels.filter((c) => c.system !== s);
	const mine = chans.map((c) => ({ ...c, system: s }));
	rest.splice(at < 0 ? rest.length : at, 0, ...mine);
	m.trunkChannels = rest;
	syncTrunk(m, s);
}

// Whether system s works on the radio (12.1): a control channel and, when it authenticates, a key
export function systemUsable(m, s) {
	return (m.trunkChannels ?? []).some((c) => c.system === s && c.control) && (!s.authenticates || validKey(s.key));
}

// --- Editing helpers ---

export function newChannel(fields = {}) {
	return {
		number: null, name: "", rxHz: 446006250, txHz: 446006250, mode: "fm", power: 0, tot: 0, rxTone: "",
		txTone: "", bw: 12.5, rxOnly: false, allSkip: false, zoneSkip: false, vox: false, squelch: 0, colorCode: 1,
		timeslot: 1, contact: null, rxList: null, aprs: null, dmrId: 0, noBeep: false, noEco: false, dmo: false,
		roam: false, aliasTs1: 0, aliasTs2: 0, useLocation: false, lat: 0, lon: 0, sfr: false, sfrA: 0, sfrB: 0,
		toneBurst: false, ...fields,
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
		// a fallback on a deleted channel is none; the deleted channel's tag goes with it (trunkEntries)
		for (const x of [...m.channels, ...(m.trunkSystems ?? [])]) if (x.fallback != null && gone.has(x.fallback)) x.fallback = null;
	} else if (table === "trunkSystems") {
		// its channels go with it; channels tagged with it become conventional, receive only: their record's frequencies
		// are the system's, where a conventional transmission doesn't belong
		m.trunkChannels = (m.trunkChannels ?? []).filter((c) => !gone.has(c.system));
		for (const c of m.channels) {
			if (!gone.has(c.system)) continue;
			setTrunk(m, c, null);
			c.rxOnly = true;
		}
	} else if (table === "contacts") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.contact)) c.contact = null;
		for (const l of m.rxLists) l.contacts = l.contacts.filter((c) => !gone.has(c));
	} else if (table === "rxLists") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.rxList)) c.rxList = null;
	} else if (table === "aprs") {
		for (const c of [...m.channels, ...m.vfo]) if (c && gone.has(c.aprs)) c.aprs = null;
	}
}

// --- Receive-only bands ---

// The bands an FM channel only listens on (docs/spec/airband-and-broadcast.md): the air band, received in AM, and
// broadcast FM, played on the broadcast receiver. The frequency decides; the codeplug stores an FM channel. A radio
// without the receiver for one hears nothing there, and none transmits there. "air", "bcast" or null
export const AIR_HZ = [108000000, 137000000], BCAST_HZ = [65000000, 108000000];
export function listenBand(hz) {
	if (hz >= AIR_HZ[0] && hz < AIR_HZ[1]) return "air";
	if (hz >= BCAST_HZ[0] && hz < BCAST_HZ[1]) return "bcast";
	return null;
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
	// the settings area's keys (codeplug-settings.md 3): null is an absent key
	const areaKeys = KEYS.filter((row) => g[row.name] != null);
	for (const row of areaKeys) {
		if (!keyValid(row, g[row.name])) err("general", null, row.name, "range", { min: row.min, max: row.max });
	}
	// an area of a version the editor doesn't know reads as empty and is never written: a value set here is lost
	if (areaKeys.length && m.base && decodeArea(m.base.bytes).state === "newer") warn("general", null, areaKeys[0].name, "settings_area_newer");

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
		const band = listenBand(c.rxHz);
		if (band && c.mode === "dmr") warn("channels", c, "mode", `listen_dmr_${band}`);
		const txBand = listenBand(c.txHz);
		if (txBand && !c.rxOnly) warn("channels", c, "rxOnly", `listen_tx_${txBand}`);
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
	validateTrunk(m, err, warn);
	return out;
}

// Tier III (docs/spec/dmr-trunking.md 12): problems are the system's (table trunkSystems), a channel's tag the channel's
function validateTrunk(m, err, warn) {
	const systems = m.trunkSystems ?? [], chans = m.trunkChannels ?? [];
	const T = "trunkSystems";
	if (systems.length > T3_LIMITS.systems) err(T, null, null, "too_many", { max: T3_LIMITS.systems, count: systems.length });
	if (chans.length > T3_LIMITS.chanKeys) err(T, null, null, "t3_chans_too_many", { max: T3_LIMITS.chanKeys, count: chans.length });
	const range = (s, f, lo, hi) => {
		if (!(Number.isInteger(s[f]) && s[f] >= lo && s[f] <= hi)) err(T, s, f, "range", { min: lo, max: hi });
	};
	for (const s of systems) {
		if (!s.name) err(T, s, "name", "no_name");
		if (s.name.length > T3_LIMITS.name) err(T, s, "name", "too_long", { max: T3_LIMITS.name });
		range(s, "model", 0, 3);
		if (!(Number.isInteger(s.net) && s.net >= 0 && s.net <= netMax(s.model))) err(T, s, "net", "t3_net_range", { max: netMax(s.model) });
		range(s, "colorCode", 0, 15);
		range(s, "ownId", 0, ID_MAX);
		range(s, "fallbackHold", 0, 255);
		range(s, "planFirstLcn", 0, T3_LIMITS.lcn);
		range(s, "planSep", 0, 7);
		range(s, "planBaseHz", 0, 0xFFFFFFFF);
		range(s, "planSplitHz", 0, 0xFFFFFFFF);
		range(s, "leaveDbm", -128, 127);
		range(s, "powerSave", 0, 7);
		if (s.sites.length > T3_LIMITS.sites) err(T, s, "sites", "too_many", { max: T3_LIMITS.sites, count: s.sites.length });
		if (s.sites.some((x) => !Number.isInteger(x) || x < 0 || x > siteMax(s.model))) err(T, s, "sites", "range", { min: 0, max: siteMax(s.model) });
		if (s.key && !validKey(s.key)) err(T, s, "key", "t3_key_length");
		if (!(Number.isInteger(s.psn) && s.psn >= 0 && s.psn <= 0xFFFFFF)) err(T, s, "psn", "range", { min: 0, max: 0xFFFFFF });
		else if (s.psn && !s.key) warn(T, s, "psn", "t3_psn_no_key");
		if (!(Number.isInteger(s.pollBlocks) && s.pollBlocks >= 1 && s.pollBlocks <= 4)) err(T, s, "pollBlocks", "range", { min: 1, max: 4 });
		if (s.pollReply !== null && s.pollReply !== undefined && !encodePollReply(s.pollFormat, s.pollReply)) err(T, s, "pollReply", "t3_poll_reply", { max: POLL_MAX });
		if (s.suplData !== null && s.suplData !== undefined && s.suplData !== "" && !encodePollReply(s.suplFormat, s.suplData)) err(T, s, "suplData", "t3_supl_data", { max: POLL_MAX });
		const mine = chans.filter((c) => c.system === s);
		const ctrl = mine.filter((c) => c.control).length;
		if (!ctrl) warn(T, s, "controlChannels", "t3_no_control");
		else if (s.authenticates && !s.key) warn(T, s, "key", "t3_no_key");
		if (ctrl > T3_LIMITS.ctrl) warn(T, s, "controlChannels", "t3_ctrl_extra", { max: T3_LIMITS.ctrl });
		if (mine.length > T3_LIMITS.chans) warn(T, s, "controlChannels", "t3_chans_extra", { max: T3_LIMITS.chans });
		fallbackProblems(T, s, s.fallback, warn);
	}
	for (const c of chans) {
		if (typeof c.system === "number") {
			warn(T, null, null, "t3_chan_no_system", { number: c.system });
			continue;
		}
		const s = c.system;
		if (!(Number.isInteger(c.lcn) && c.lcn >= 1 && c.lcn <= T3_LIMITS.lcn)) err(T, s, "lcn", "range", { min: 1, max: T3_LIMITS.lcn });
		for (const f of ["rxHz", "txHz"]) if (!(c[f] > 0 && c[f] < 1e9)) err(T, s, f, "bad_freq");
		if (c.cc != null && !(Number.isInteger(c.cc) && c.cc >= 0 && c.cc <= 15)) err(T, s, "colorCode", "range", { min: 0, max: 15 });
	}
	for (const c of m.channels) {
		if (!c.system) continue;
		if (typeof c.system === "number") warn("channels", c, "system", "t3_tag_no_system", { number: c.system });
		if (!c.contact && !c.rxList) warn("channels", c, "contact", "t3_no_talkgroup");
		if (c.fallback) fallbackProblems("channels", c, c.fallback, warn);
	}
	if (!m.base) return;
	const state = decodeArea(m.base.bytes).state;
	if (state === "newer") {
		if (systems.length) warn(T, null, null, "t3_area_newer");
		return;
	}
	if (!systems.length && !chans.length && !m.channels.some((c) => c.system)) return;
	const { used, max } = areaUse(m);
	if (used > max) err(T, null, null, "settings_area_full", { used, max });
}

// A fallback must be a conventional channel the radio finds by its number and name
function fallbackProblems(table, item, f, warn) {
	if (typeof f === "number") warn(table, item, "fallback", "t3_fallback_stale", { number: f });
	else if (f && f.system) warn(table, item, "fallback", "t3_fallback_trunked");
}
