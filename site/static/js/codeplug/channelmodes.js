// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The channel-mode layer of the editing model (docs/spec/channel-modes.md): channel tags, systems, their entries and
// secrets, and the kinds' radio-wide records, in the codeplug settings area. Generic: a kind's or a type's bytes are
// its mode module's (modes.js); a kind or type this editor doesn't know is kept byte for byte, and so is every byte a
// module doesn't cover. A record left as it was keeps its bytes.
//
// On a channel (model.js): kind (0 plain), and for a channel of a kind: system (a system, a number as read, or null),
// tagTx (transmit allowed), baseOwn (the record stands on its own), fallback (a channel, a number as read, or null
// for the system's), the kind's named flag bits (module.flags), params (the module's fields; null for an unknown
// kind) and tagRaw (the tag as read). Its record (mode, rxOnly, and for a kind without frequencies of its own the
// frequencies) is the safe stand-in (2.3), kept so by syncModes().
// m.systems: {number 1-255 (key 0x0100 + number - 1), type, name, sharing, ownId, tx, needsKey, fallback, fields
// (the module's; null for an unknown type), entries: [{cls, group, control, flags, chan, rxHz, txHz, the class's
// fields, raw}], secret (the module's fields, {raw} for a type without, null for none), flagsRaw, raw}.
// m.modeSettings: {kind id: the module's fields}.

import { decodeArea, getValue, setValue, deleteValue, dedupe, putArea, KV_ADDR, KV_SIZE, KV_HDR, KEYS } from "./kv.js";
import { getName, putName } from "./fields.js";
import { KINDS, kindRow, typeRow, radioRuns } from "./modes.js";
import { u16, u32, put16, put32, sameBytes, sameValue, nameCheck } from "./modebytes.js";

export const MODE_KEY = { system: 0x0100, entry: 0x0200, secret: 0x0300, tag: 0x0400, setting: 0x0A00 };
export const MODE_LIMITS = { systems: 255, entries: 256, tags: 1024, name: 16 };
export const TAG = { tx: 0x01, baseOwn: 0x02, fallback: 0x04, reserved: 0x08 };
export const SYS = { needsKey: 0x01, tx: 0x02 };
export const SHARE = { off: 0, identity: 1, whole: 2 };
const TAG_HDR = 5, TAG_FB = 4, SYS_HDR = 28, ENTRY_HDR = 4;

const inFamily = (key, base, count) => key >= base && key < base + count;
const find = (list, n) => list.find((x) => x.number === n) || n;

// --- Bytes ---

// A tag (2.1): {kind, system, check, flags, fbChan, fbCheck, params}; null when the value is no tag
export function decodeTag(v) {
	if (!v || v.length < TAG_HDR || v[0] === 0 || v[0] === 0xFF) return null;
	const fb = (v[4] & TAG.fallback) !== 0;
	if (fb && v.length < TAG_HDR + TAG_FB) return null;
	return { kind: v[0], system: v[1], check: u16(v, 2), flags: v[4], fbChan: fb ? u16(v, 5) : 0, fbCheck: fb ? u16(v, 7) : 0, params: v.subarray(fb ? TAG_HDR + TAG_FB : TAG_HDR) };
}

export function encodeTag(t) {
	const at = t.flags & TAG.fallback ? TAG_HDR + TAG_FB : TAG_HDR;
	const v = new Uint8Array(at + t.params.length);
	v[0] = t.kind;
	v[1] = t.system;
	put16(v, 2, t.check);
	v[4] = t.flags;
	if (at > TAG_HDR) {
		put16(v, 5, t.fbChan);
		put16(v, 7, t.fbCheck);
	}
	v.set(t.params, at);
	return v;
}

// A tag is live for a channel record (2.3): the record's name, and a stand-in shape unless the base stands on its own
export const tagLive = (t, rec) => !!t && !!rec && t.check === nameCheck(rec.name) && ((t.flags & TAG.baseOwn) !== 0 || (rec.mode === "dmr" && rec.rxOnly));

// A system record's header (7.1) and its block; null when the value is no system
export function decodeSystemRecord(v) {
	if (!v || v.length < SYS_HDR || v[0] === 0 || v[0] === 0xFF) return null;
	return { type: v[0], flags: v[1], sharing: v[2], idLen: v[3], ownId: u32(v, 4), fbChan: u16(v, 8), fbCheck: u16(v, 10), name: getName(v, 12, 16), block: v.subarray(SYS_HDR) };
}

// A system entry (7.2): {system, cls, group, flags, body}; null when too short
export function decodeEntryRecord(v) {
	if (!v || v.length < ENTRY_HDR) return null;
	return { system: v[0], cls: v[1], group: v[2], flags: v[3], body: v.subarray(ENTRY_HDR) };
}

// A system's identity (7.1): its type and the first idLen octets of its block, as text: two systems with the same are
// the same system
export function identityKey(s) {
	const row = typeRow(s.type);
	if (!row) return null;
	const b = row.module.encode(s.fields, null).subarray(0, row.module.idLen);
	return `${s.type}:${[...b].join(",")}`;
}

// --- Decoding ---

// The channel a fallback names: the channel when the check matches its name, else the number as stored; null for none
function fallbackRef(m, n, check) {
	if (!n) return null;
	const c = m.channels.find((x) => x.number === n);
	return c && nameCheck(c.name) === check ? c : n;
}

function decodeEntry(s, e, raw) {
	const cls = typeRow(s.type)?.module.classes.find((x) => x.cls === e.cls);
	const out = { cls: e.cls, group: e.group, control: (e.flags & 1) !== 0, flags: e.flags & ~1 };
	if (e.cls === 0) Object.assign(out, { chan: u16(e.body, 0), rxHz: u32(e.body, 2), txHz: u32(e.body, 6) });
	if (cls) Object.assign(out, cls.decode(e.body));
	out.raw = raw;
	return out;
}

const entryFields = (e) => {
	const { raw, ...rest } = e;
	return rest;
};

// The modes in an image's bytes into the model (after its channels)
export function decodeModes(m, b) {
	const area = decodeArea(b);
	const entries = area.state === "valid" ? dedupe(area.entries) : [];
	m.systems = [];
	m.orphanEntries = [];
	m.modeSettings = {};
	for (let n = 0; n < MODE_LIMITS.systems; n++) {
		const raw = getValue(entries, MODE_KEY.system + n), h = decodeSystemRecord(raw);
		if (!h) continue;
		const mod = typeRow(h.type)?.module;
		const sec = getValue(entries, MODE_KEY.secret + n);
		m.systems.push({
			number: n + 1, type: h.type, name: h.name, sharing: h.sharing, ownId: h.ownId,
			tx: (h.flags & SYS.tx) !== 0, needsKey: (h.flags & SYS.needsKey) !== 0, flagsRaw: h.flags,
			fallback: null, fbChan: h.fbChan, fbCheck: h.fbCheck,
			fields: mod ? mod.decode(h.block) : null, entries: [],
			secret: sec ? (mod?.secret?.decode(sec) ?? { raw: sec }) : null, raw,
		});
	}
	const fam = entries.filter((e) => inFamily(e.key, MODE_KEY.entry, MODE_LIMITS.entries)).sort((a, b) => a.key - b.key);
	for (const { value } of fam) {
		const e = decodeEntryRecord(value);
		const s = e && m.systems.find((x) => x.number === e.system + 1);
		if (s) s.entries.push(decodeEntry(s, e, value));
		else m.orphanEntries.push(value);
	}
	for (const c of m.channels) {
		c.kind = 0;
		const raw = getValue(entries, MODE_KEY.tag + c.number - 1), t = decodeTag(raw);
		if (!tagLive(t, c)) continue;
		const mod = kindRow(t.kind)?.module;
		Object.assign(c, {
			kind: t.kind, system: t.system === 0xFF ? null : find(m.systems, t.system + 1), tagTx: (t.flags & TAG.tx) !== 0,
			baseOwn: (t.flags & TAG.baseOwn) !== 0, fallback: null, params: mod ? mod.decode(t.params) : null, tagRaw: raw,
		});
		for (const [k, bit] of Object.entries(mod?.flags ?? {})) c[k] = (t.flags & bit) !== 0;
	}
	// fallbacks name channels: resolved once every channel has its kind
	for (const c of m.channels) {
		const t = c.kind ? decodeTag(c.tagRaw) : null;
		if (t && t.flags & TAG.fallback) c.fallback = fallbackRef(m, t.fbChan, t.fbCheck);
	}
	for (const s of m.systems) {
		s.fallback = fallbackRef(m, s.fbChan, s.fbCheck);
		delete s.fbChan;
		delete s.fbCheck;
	}
	for (const k of KINDS) {
		const set = k.module.settings;
		if (set) m.modeSettings[k.id] = set.decode(getValue(entries, MODE_KEY.setting + k.id));
	}
}

// --- The record a channel of a kind keeps (2.3) ---

// The base record's fields for channel c: the safe stand-in (DMR, receive only) unless the tag says the base stands
// on its own; a kind without frequencies of its own takes its system's first control channel's
export function standIn(m, c) {
	if (!c.kind || c.baseOwn) return {};
	const out = { mode: "dmr", rxOnly: true };
	const mod = kindRow(c.kind)?.module;
	if (mod && mod.ownFreqs === false && c.system && typeof c.system === "object") {
		const ctl = c.system.entries.find((e) => e.cls === 0 && e.control && e.rxHz);
		if (ctl) Object.assign(out, { rxHz: ctl.rxHz, txHz: ctl.txHz || ctl.rxHz });
	}
	return out;
}

// Every channel of a kind brought to its record
export function syncModes(m) {
	for (const c of m.channels) if (c.kind) Object.assign(c, standIn(m, c));
}

// Channel c made kind (0: plain, mode "fm" or "dmr"). Fields of the kind it had are dropped; transmit allowed carries
// over as the record's receive-only bit and back
export function setKind(m, c, kind, mode = "dmr") {
	if (!kind) {
		if (c.kind) {
			// a kind without frequencies of its own left its system's in the record: never a place to transmit (as
			// removeModes)
			c.rxOnly = !c.tagTx || (!c.baseOwn && kindRow(c.kind)?.module.ownFreqs === false);
			Object.assign(c, { system: null, tagTx: false, baseOwn: false, fallback: null, params: null, tagRaw: null });
		}
		c.kind = 0;
		c.mode = mode;
		return;
	}
	if (c.kind === kind) return;
	const mod = kindRow(kind).module;
	const tx = c.kind ? c.tagTx : !c.rxOnly;
	setKind(m, c, 0);
	Object.assign(c, { kind, system: null, tagTx: mod.defaultTx && tx, baseOwn: false, fallback: null, params: mod.newParams(), tagRaw: null });
	for (const k of Object.keys(mod.flags ?? {})) c[k] = false;
	if (mod.systemType) c.system = m.systems.find((s) => s.type === mod.systemType) ?? null;
	Object.assign(c, standIn(m, c));
}

export function newSystem(m, type, name) {
	const mod = typeRow(type).module;
	return { number: null, type, name, sharing: SHARE.off, ownId: 0, tx: mod.defaultTx, needsKey: false, flagsRaw: 0, fallback: null, fields: mod.newFields(), entries: [], secret: null, raw: null };
}

// --- Writing ---

// A fallback reference as stored: number and check. A number as read keeps the check stored with it (was: the
// stored number and check)
function fallbackOut(ref, wasChan, wasCheck) {
	if (!ref) return { chan: 0, check: 0 };
	if (typeof ref === "object") return { chan: ref.number || 0, check: ref.number ? nameCheck(ref.name) : 0 };
	return { chan: ref, check: ref === wasChan ? wasCheck : 0 };
}

function encodeSystem(s, prev) {
	const was = decodeSystemRecord(prev);
	const row = typeRow(s.type);
	if (!row) return prev ?? s.raw;	// a type this editor doesn't know: as found
	const mod = row.module;
	const old = was && was.type === s.type ? was.block : null;
	const block = old && sameValue(mod.decode(old), s.fields) ? old : mod.encode(s.fields, old);
	const v = new Uint8Array(SYS_HDR + block.length);
	v[0] = s.type;
	v[1] = ((was ? was.flags : 0) & ~(SYS.tx | SYS.needsKey)) | (s.tx ? SYS.tx : 0) | (s.needsKey ? SYS.needsKey : 0);
	v[2] = s.sharing & 0xFF;
	v[3] = mod.idLen;
	put32(v, 4, s.ownId || 0);
	const fb = fallbackOut(s.fallback, was?.fbChan, was?.fbCheck);
	put16(v, 8, fb.chan);
	put16(v, 10, fb.check);
	if (was && was.name === s.name) v.set(prev.subarray(12, SYS_HDR), 12);
	else putName(v, 12, 16, s.name);
	v.set(block, SYS_HDR);
	return v;
}

function encodeEntry(s, e) {
	const n = s.number - 1;
	if (e.raw) {
		const d = decodeEntryRecord(e.raw);
		if (d.system === n && sameValue(entryFields(decodeEntry(s, d, null)), entryFields({ ...e, raw: null }))) return e.raw;
	}
	const row = typeRow(s.type);
	if (!row) return e.raw;
	const cls = row.module.classes.find((x) => x.cls === e.cls);
	let body = e.raw ? e.raw.slice(ENTRY_HDR) : new Uint8Array(0);
	if (e.cls === 0) {
		const v = new Uint8Array(Math.max(10, body.length));
		v.set(body);
		put16(v, 0, e.chan ?? 0);
		put32(v, 2, e.rxHz || 0);
		put32(v, 6, e.txHz || 0);
		body = v;
	}
	if (cls) body = cls.encode(e, body);
	const v = new Uint8Array(ENTRY_HDR + body.length);
	v[0] = n;
	v[1] = e.cls;
	v[2] = e.group ?? 0xFF;
	v[3] = (e.flags & ~1) | (e.control ? 1 : 0);
	v.set(body, ENTRY_HDR);
	return v;
}

// The settings area's entries with the model's modes in place of those stored. baseNames: a function that gives the
// names of the channels the image the model came from stores, by number (model.js knows the record layout)
export function modeEntries(m, entries, baseNames = () => new Map()) {
	let out = entries;
	const set = (key, v) => {
		if (!sameBytes(getValue(out, key), v)) out = setValue(out, key, v);
	};
	const del = (key) => {
		if (getValue(out, key)) out = deleteValue(out, key);
	};
	const systems = (m.systems ?? []).filter((s) => s.number >= 1 && s.number <= MODE_LIMITS.systems);

	for (let n = 0; n < MODE_LIMITS.systems; n++) {
		const key = MODE_KEY.system + n, prev = getValue(out, key);
		const s = systems.find((x) => x.number === n + 1);
		if (!s) {
			// a system the model no longer has was deleted: its secret and its type's own records with it
			const was = decodeSystemRecord(prev);
			if (was) {
				del(key);
				del(MODE_KEY.secret + n);
				for (const k of typeRow(was.type)?.module.ownedKeys(n) ?? []) del(k);
			}
			continue;
		}
		set(key, encodeSystem(s, prev));
		const skey = MODE_KEY.secret + n, sp = getValue(out, skey);
		const sec = typeRow(s.type)?.module.secret;
		if (s.secret == null) del(skey);
		else if (s.secret.raw) set(skey, s.secret.raw);
		else if (!(sp && sameValue(sec.decode(sp), s.secret))) set(skey, sec.encode(s.secret, sp));
	}

	// Entries: the family rewritten from 0x0200 up, in system order, when any record changed
	const want = [];
	for (const s of [...systems].sort((a, b) => a.number - b.number)) for (const e of s.entries) want.push(encodeEntry(s, e));
	const nums = new Set(systems.map((s) => s.number - 1));
	want.push(...(m.orphanEntries ?? []).filter((v) => !nums.has(v[0])));
	want.length = Math.min(want.length, MODE_LIMITS.entries);
	const have = out.filter((e) => inFamily(e.key, MODE_KEY.entry, MODE_LIMITS.entries)).sort((a, b) => a.key - b.key);
	if (!(have.length === want.length && have.every((e, i) => e.key === MODE_KEY.entry + i && sameBytes(e.value, want[i])))) {
		out = out.filter((e) => !inFamily(e.key, MODE_KEY.entry, MODE_LIMITS.entries));
		want.forEach((v, i) => (out = setValue(out, MODE_KEY.entry + i, v)));
	}

	// Tags: one for each channel of a kind, at its number. Another goes when no channel is at its number, or when its
	// check is the name there or the name the image had there (2.4); else it is stale and kept
	const tagged = new Map();
	for (const c of m.channels) if (c.kind && c.number) tagged.set(MODE_KEY.tag + c.number - 1, c);
	const names = new Map(m.channels.filter((c) => c.number).map((c) => [c.number, c.name]));
	let was = null;
	for (const e of out.filter((x) => inFamily(x.key, MODE_KEY.tag, MODE_LIMITS.tags))) {
		if (tagged.has(e.key)) continue;
		const n = e.key - MODE_KEY.tag + 1, t = decodeTag(e.value);
		was ??= baseNames();
		if (!names.has(n) || (t && (t.check === nameCheck(names.get(n)) || (was.has(n) && t.check === nameCheck(was.get(n)))))) del(e.key);
	}
	for (const [key, c] of tagged) {
		const prev = getValue(out, key), here = decodeTag(prev), read = decodeTag(c.tagRaw);
		// the tag the channel came with: the one at its number, else the one it had where it was read
		const src = here && here.kind === c.kind ? here : read && read.kind === c.kind ? read : null;
		const mod = kindRow(c.kind)?.module;
		let params = src ? src.params : new Uint8Array(0);
		if (mod && !(src && sameValue(mod.decode(src.params), c.params))) params = mod.encode(c.params, src?.params ?? null);
		let flags = (src ? src.flags & (0xF0 | TAG.reserved) : 0) | (c.tagTx ? TAG.tx : 0) | (c.baseOwn ? TAG.baseOwn : 0);
		for (const [k, bit] of Object.entries(mod?.flags ?? {})) flags = c[k] ? flags | bit : flags & ~bit;
		const fb = fallbackOut(c.fallback, src?.fbChan, src?.fbCheck);
		if (fb.chan) flags |= TAG.fallback;
		const sys = c.system == null ? 0xFF : (typeof c.system === "number" ? c.system : c.system.number) - 1;
		set(key, encodeTag({ kind: c.kind, system: sys & 0xFF, check: nameCheck(c.name), flags, fbChan: fb.chan, fbCheck: fb.check, params }));
	}

	// The kinds' radio-wide records: written when a field changed; none when the module says so
	for (const k of KINDS) {
		const st = k.module.settings, s = m.modeSettings?.[k.id];
		if (!st || !s) continue;
		const key = MODE_KEY.setting + k.id, prev = getValue(out, key);
		if (sameValue(st.decode(prev), s)) continue;
		const v = st.encode(s, prev);
		if (v) set(key, v);
		else del(key);
	}
	return out;
}

// The model's modes written into the image's settings area, only when they change it. An area of a newer version is
// never written (validate() warns)
export function encodeModes(m, b, baseNames) {
	const area = decodeArea(b);
	if (area.state === "newer") return;
	const entries = area.state === "valid" ? dedupe(area.entries) : [];
	const out = modeEntries(m, entries, baseNames);
	if (out !== entries) putArea(b, out);
}

// The octets the area's entries would take with the model written (the general settings' keys too), and the most it
// holds
export function modeAreaUse(m, b, baseNames) {
	const area = decodeArea(b);
	const entries = area.state === "valid" ? dedupe(area.entries) : [];
	let out = modeEntries(m, entries, baseNames);
	for (const row of KEYS) if (m.general?.[row.name] != null && !getValue(out, row.key)) out = [...out, { key: row.key, value: new Uint8Array(row.size) }];
	const hdr = area.state === "valid" ? b[KV_ADDR + 5] : KV_HDR;
	return { used: out.reduce((n, e) => n + 4 + e.value.length, 0), max: KV_SIZE - hdr };
}

// --- Editing ---

// A channel's references to removed channels and systems dropped. Channels on a removed system become plain, receive
// only: their record holds the system's frequencies, where they must not transmit
export function removeModes(m, table, gone) {
	if (table === "channels") {
		for (const x of [...m.channels, ...(m.systems ?? [])]) if (x.fallback != null && gone.has(x.fallback)) x.fallback = null;
	} else if (table === "systems") {
		for (const c of m.channels) {
			if (!c.kind || !gone.has(c.system)) continue;
			setKind(m, c, 0, "dmr");
			c.rxOnly = true;
		}
	}
}

// --- Validation ---

// Problems of the modes, in validate()'s terms. features: the connected radio's (null: none)
export function validateModes(m, err, warn, { features = null, baseNames } = {}) {
	const systems = m.systems ?? [];
	const S = "systems";
	if (systems.length > MODE_LIMITS.systems) err(S, null, null, "too_many", { max: MODE_LIMITS.systems, count: systems.length });
	const nEntries = systems.reduce((n, s) => n + s.entries.length, 0);
	if (nEntries > MODE_LIMITS.entries) err(S, null, null, "mode_entries_too_many", { max: MODE_LIMITS.entries, count: nEntries });
	const ids = new Map();
	for (const s of systems) {
		const row = typeRow(s.type);
		if (!row) {
			warn(S, s, "type", "mode_type_unknown", { type: s.type });
			continue;
		}
		const mod = row.module;
		if (!s.name) err(S, s, "name", "no_name");
		if (s.name.length > MODE_LIMITS.name) err(S, s, "name", "too_long", { max: MODE_LIMITS.name });
		if (!(Number.isInteger(s.ownId) && s.ownId >= 0 && s.ownId <= 0xFFFFFFFF)) err(S, s, "ownId", "range", { min: 0, max: 0xFFFFFFFF });
		const problem = { err: (f, code, p = {}) => err(S, s, f, code, p), warn: (f, code, p = {}) => warn(S, s, f, code, p) };
		mod.validate(s, s.fields, s.entries, problem);
		if (mod.classes.some((c) => c.control) && !s.entries.some((e) => e.cls === 0 && e.control)) warn(S, s, "entries", "mode_no_control");
		if (s.needsKey && !s.secret) warn(S, s, "secret", "mode_no_secret");
		if (s.secret && !s.secret.raw && mod.secret && !mod.secret.valid(s.secret)) err(S, s, "secret", "mode_secret_bad");
		if (typeof s.fallback === "number") warn(S, s, "fallback", "mode_fallback_stale", { number: s.fallback });
		else if (s.fallback?.kind) warn(S, s, "fallback", "mode_fallback_kind");
		const id = identityKey(s);
		if (ids.has(id)) warn(S, s, "fields", "mode_same_identity", { other: ids.get(id).name });
		else ids.set(id, s);
	}
	for (const c of m.channels) {
		if (!c.kind) continue;
		const row = kindRow(c.kind);
		if (!row) {
			warn("channels", c, "mode", "mode_kind_unknown", { kind: c.kind });
			continue;
		}
		if (features != null && !radioRuns(features, c.kind)) warn("channels", c, "mode", "mode_not_in_radio", { mode: row.short });
		const mod = row.module;
		if (mod.systemType) {
			if (!c.system) err("channels", c, "system", "mode_no_system");
			else if (typeof c.system === "number") warn("channels", c, "system", "mode_system_missing", { number: c.system });
			else if (c.system.type !== mod.systemType) err("channels", c, "system", "mode_system_type");
		}
		if (typeof c.fallback === "number") warn("channels", c, "fallback", "mode_fallback_stale", { number: c.fallback });
		else if (c.fallback?.kind) warn("channels", c, "fallback", "mode_fallback_kind");
		mod.validate?.(c, c.params, { err: (f, code, p = {}) => err("channels", c, f, code, p), warn: (f, code, p = {}) => warn("channels", c, f, code, p) });
	}
	for (const k of KINDS) {
		const st = k.module.settings, s = m.modeSettings?.[k.id];
		if (st && s) st.validate(s, { err: (f, code, p = {}) => err("general", null, f, code, p), warn: (f, code, p = {}) => warn("general", null, f, code, p) });
	}
	if (!m.base) return;
	const state = decodeArea(m.base.bytes).state;
	const any = systems.length || m.channels.some((c) => c.kind);
	if (state === "newer") {
		if (any) warn(S, null, null, "mode_area_newer");
		return;
	}
	if (!any) return;
	const { used, max } = modeAreaUse(m, m.base.bytes, baseNames);
	if (used > max) err(S, null, null, "settings_area_full", { used, max });
}

// --- Secrets (8) ---

// The families that hold secrets: system secrets and traffic keys
const SECRET_FAMILIES = [[MODE_KEY.secret, 0x100], [0x1900, 0x400]];
const isSecret = (key) => SECRET_FAMILIES.some(([b, n]) => inFamily(key, b, n));

// How many secrets an image's settings area holds
export function countSecrets(b) {
	const a = decodeArea(b);
	return a.state === "valid" ? dedupe(a.entries).filter((e) => isSecret(e.key)).length : 0;
}

// An image's bytes with every secret taken out of its settings area (a file export without keys)
export function withoutSecrets(b) {
	const a = decodeArea(b);
	if (a.state !== "valid") return;
	const entries = dedupe(a.entries);
	const kept = entries.filter((e) => !isSecret(e.key));
	if (kept.length !== entries.length) putArea(b, kept);
}
