// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radioFoundry block of a qdmr YAML file (docs/spec/qdmr-yaml.md 3.7): what qdmr has no element for, carried in
// keys qdmr ignores, the way qdmr carries its own blocks for some radios. A top-level block holds the systems and the
// modes' radio-wide settings; a channel's block holds its mode and its Accept Invalid CRC setting. Generic: every
// field is written as the mode module (modes.js) names it, so a new mode needs nothing here. Keys are never written.

import { flow, Raw } from "./yaml.js";
import { KINDS, SYSTEM_TYPES, kindRow, typeRow } from "./modes.js";
import { newSystem, identityKey, systemTx, systemIdentityCheck, SHARE } from "./channelmodes.js";
import { sameValue } from "./modebytes.js";
import { isDmr, isP25, scopedContact, contactScope, resolveContactSystems, wantsP25Contacts } from "./contactmode.js";
import { LIMITS } from "./records.js";

const SHARING = Object.fromEntries(Object.entries(SHARE).map(([k, v]) => [v, k]));
const list = (f) => (typeof f === "function" ? f() : f ?? []);
const byName = (rows, v) => rows.find((r) => r.short === String(v) || r.id === v) ?? null;
// An entry's frequencies are the layer's, not its class's
const ENTRY_FREQS = [{ key: "rxHz", type: "freq" }, { key: "txHz", type: "freq" }];

// The fields' types by key: frequencies are written as text, hex fields as 0x numbers
const typesOf = (fields) => new Map(list(fields).map((f) => [f.key, f]));

// What a field set holds as written: the keys of a fresh set (and the module's optional ones), nothing else
function fieldsOut(values, fresh, fields, frequencyText, { optional = [], skip = () => false } = {}) {
	const types = typesOf(fields);
	const out = {};
	for (const k of [...Object.keys(fresh), ...optional]) {
		const v = values?.[k];
		if (v === undefined || k in out || skip(k, v)) continue;
		const f = types.get(k);
		if (f?.type === "freq") out[k] = v ? frequencyText(v) : 0;
		else if (f?.type === "hex" && Number.isInteger(v)) out[k] = new Raw("0x" + v.toString(16).toUpperCase().padStart(f.digits ?? 1, "0"));
		else out[k] = Array.isArray(v) ? flow(v) : v;
	}
	return flow(out);
}

// A hex field as read: a number (0x293 or 659) or hex text ("293", "0x293"); null for none
function hexIn(v) {
	if (typeof v === "number") return Number.isInteger(v) ? v : null;
	const m = /^\s*(?:0x)?([0-9a-f]+)\s*$/i.exec(String(v ?? ""));
	return m ? parseInt(m[1], 16) : null;
}

// Fields as read, onto a fresh set: only its keys (and the module's optional ones), each in its type. A key left out
// keeps its default
function fieldsIn(fresh, given, fields, { parseFrequency, untag }, optional = []) {
	const types = typesOf(fields);
	const out = { ...fresh };
	if (!given || typeof given !== "object") return out;
	for (const k of [...Object.keys(fresh), ...optional]) {
		if (!(k in given)) continue;
		const v = untag(given[k]), d = fresh[k], type = types.get(k)?.type;
		if (!(k in fresh)) {
			// an optional key: an integer or nothing
			if (Number.isInteger(Number(v)) && v !== null && v !== "") out[k] = Number(v);
		} else if (type === "freq") out[k] = v === 0 ? 0 : parseFrequency(v) ?? d;
		else if (type === "hex") out[k] = v === null ? d : hexIn(v) ?? d;
		else if (typeof d === "boolean") out[k] = v === true;
		else if (Array.isArray(d)) out[k] = Array.isArray(v) ? v.map((x) => Number(untag(x))).filter(Number.isFinite) : d;
		else if (typeof d === "number" || d === null) out[k] = v === null ? null : Number.isFinite(Number(v)) && v !== "" ? Number(v) : d;
		else out[k] = v == null ? d : String(v);
	}
	return out;
}

// An entry's fresh set: the layer's header fields, then its class's
const freshEntry = (cls) => ({ cls: cls.cls, group: 0xFF, flags: 0, control: false, ...cls.newEntry([]) });

// --- Export ---

// The block's parts for model m: {top (or undefined), channel(c) (a channel's block or undefined)}. chRef names a
// channel by its id in the file (null for none)
export function exportModes(m, chRef, note, frequencyText, { contactRef = () => undefined, listRef = () => undefined } = {}) {
	const systems = (m.systems ?? []).filter((s) => {
		if (typeRow(s.type) && s.fields) return true;
		note("mode_unknown");
		return false;
	});
	const sysId = new Map(systems.map((s, i) => [s, `sys${i + 1}`]));
	const air = m.contacts.filter(isP25), airIds = new Map(air.map((c, i) => [c, `p25c${i + 1}`]));
	const airContacts = air.map((c) => {
		const system = typeof c.system === "object" ? sysId.get(c.system) : undefined;
		const scope = contactScope(c);
		return flow({ ref: airIds.get(c), slot: c.number ?? undefined, mode: "P25", name: c.name, type: c.type, id: c.id, system,
			...(scopedContact(c) && !system ? { systemIndex: typeof c.system === "object" ? 255 : scope.system - 1, systemCheck: new Raw(`0x${scope.systemCheck.toString(16).toUpperCase().padStart(4, "0")}`) } : {}) });
	});
	const p25Lists = new Set(m.channels.filter(wantsP25Contacts).map((c) => c.rxList));
	const rxLists = m.rxLists.filter((l) => l.contacts.some(isP25) || p25Lists.has(l)).map((l) => flow({ list: listRef(l),
		contacts: flow(l.contacts.map((c) => typeof c === "number" ? `#p25:${c}` : isP25(c) ? airIds.get(c) : contactRef(c)).filter(Boolean)) }));
	const ref = (r) => (r && typeof r === "object" && chRef(r)) || undefined;
	const keys = systems.filter((s) => (s.secret && (s.secret.key || s.secret.raw)) || s.trafficKeys?.length).length;
	if (keys) note("mode_keys", keys);
	const out = systems.map((s) => {
		const mod = typeRow(s.type).module;
		return {
			id: sysId.get(s), type: typeRow(s.type).short, name: s.name, transmit: systemTx(s), ownId: s.ownId || 0,
			authenticates: s.needsKey, sharing: SHARING[s.sharing] ?? s.sharing, fallback: ref(s.fallback),
			settings: fieldsOut(s.fields, mod.newFields(), mod.fields, frequencyText),
			entries: s.entries.flatMap((e) => {
				const cls = mod.classes.find((x) => x.cls === e.cls);
				if (!cls) {
					note("mode_unknown");
					return [];
				}
				const skip = (k, v) => (k === "group" && !cls.group) || (k === "flags" && !v);
				return [fieldsOut(e, freshEntry(cls), [...list(cls.fields), ...ENTRY_FREQS], frequencyText, { skip })];
			}),
		};
	});
	// the radio-wide records: those set away from their defaults
	const settings = {};
	for (const k of KINDS) {
		const set = k.module.settings, v = m.modeSettings?.[k.id];
		if (set && v && !sameValue(v, set.decode(null))) settings[k.short] = fieldsOut(v, set.decode(null), set.fields, frequencyText);
	}
	const has = Object.keys(settings).length;
	const top = out.length || has || air.length || rxLists.length ? { systems: out.length ? out : undefined, modeSettings: has ? settings : undefined,
		airContacts, rxLists: rxLists.length ? rxLists : undefined } : undefined;

	const channel = (c) => {
		const b = {};
		if (c.kind) {
			const row = kindRow(c.kind);
			if (!row || !c.params) note("mode_unknown");
			else {
				const mod = row.module;
				Object.assign(b, { mode: row.short, system: c.system && typeof c.system === "object" ? sysId.get(c.system) : undefined, transmit: c.tagTx });
				if (c.baseOwn) b.ownFrequencies = true;
				for (const k of Object.keys(mod.flags ?? {})) if (c[k]) b[k] = true;
				if (ref(c.fallback)) b.fallback = ref(c.fallback);
				const p = fieldsOut(c.params, mod.newParams(), mod.fields, frequencyText, { optional: mod.optionalParams });
				if (Object.keys(p.value).length) b.settings = p;
			}
		}
		if (c.acceptBadCrc) b.acceptInvalidCrc = true;
		if (isP25(c.contact)) b.p25Contact = airIds.get(c.contact);
		else if (c.params?.p25Contacts && typeof c.contact === "number") b.p25ContactSlot = c.contact;
		return Object.keys(b).length ? flow(b) : undefined;
	};
	return { top, channel };
}

// --- Import ---

// The block's systems and channel modes into model m, after its channels are in place. top: the file's top-level block;
// chans: [channel, its block] for every channel; chById: channel by file id. A system with the identity (or, for a
// type without one, the type and name) of one the model had keeps that system's keys, which the file never holds.
// Returns false when the file has no block at all: the model's systems and radio-wide records stay as they were
export function importModes(m, top, chans, chById, note, values) {
	const { untag } = values;
	const ref = (r) => (r == null ? null : chById.get(String(untag(r))) ?? null);
	const any = (top && typeof top === "object") || chans.some(([, b]) => b);
	for (const [c, b] of chans) if (b && b.acceptInvalidCrc === true) c.acceptBadCrc = c.mode === "dmr";
	if (!any) {
		// the systems stay; a fallback to a channel the file replaced can't
		for (const s of m.systems ?? []) if (s.fallback && typeof s.fallback === "object" && !m.channels.includes(s.fallback)) s.fallback = null;
		return false;
	}

	const had = m.systems ?? [];
	const same = (s) => {
		const id = identityKey(s);
		return had.find((x) => x.type === s.type && (id != null ? identityKey(x) === id : x.name === s.name)) ?? null;
	};
	const systems = [], sysById = new Map();
	for (const x of Array.isArray(top?.systems) ? top.systems : []) {
		const row = x && typeof x === "object" ? byName(SYSTEM_TYPES, untag(x.type)) : null;
		if (!row) {
			note("mode_unknown");
			continue;
		}
		const mod = row.module;
		const s = newSystem(m, row.id, String(untag(x.name) ?? "").slice(0, 16));
		if (x.transmit != null) s.tx = x.transmit === true;
		s.ownId = Number(untag(x.ownId)) || 0;
		s.needsKey = x.authenticates === true;
		const share = untag(x.sharing);
		if (share != null) s.sharing = SHARE[share] ?? (Number.isInteger(share) ? share : s.sharing);
		s.fallback = ref(x.fallback);
		s.fields = fieldsIn(mod.newFields(), x.settings, mod.fields, values);
		for (const e of Array.isArray(x.entries) ? x.entries : []) {
			const cls = mod.classes.find((k) => k.cls === Number(untag(e?.cls ?? 0)));
			if (!cls) {
				note("mode_unknown");
				continue;
			}
			s.entries.push({ ...fieldsIn(freshEntry(cls), e, [...list(cls.fields), ...ENTRY_FREQS], values), cls: cls.cls, raw: null });
		}
		const was = same(s);
		if (was) Object.assign(s, { secret: was.secret, trafficKeys: was.trafficKeys ?? [] });
		systems.push(s);
		sysById.set(String(untag(x.id)), s);
	}
	systems.forEach((s, i) => (s.number = i + 1));
	m.systems = systems;
	m.orphanEntries = [];
	// An older YAML file can replace Systems without carrying contacts. Retained contacts follow the same complete
	// identity, never a newly occupied slot; a deleted or ambiguous owner becomes explicitly unresolved.
	for (const c of m.contacts.filter(scopedContact)) if (typeof c.system === "object") {
		const scope = contactScope(c), matches = systems.filter((s) => s.type === 3 && systemIdentityCheck(s) != null && systemIdentityCheck(c.system) != null && identityKey(s) === identityKey(c.system));
		c.system = matches.length === 1 ? matches[0] : 256;
		c.systemCheck = scope.systemCheck;
	}
	const airByRef = new Map();
	if (Array.isArray(top?.airContacts)) {
		m.contacts = m.contacts.filter(isDmr);
		for (const x of top.airContacts) {
			if (!x || x.mode !== "P25" || !["group", "unit", "all"].includes(x.type)) { note("mode_unknown"); continue; }
			const slot = Number(untag(x.slot));
			const c = { number: Number.isInteger(slot) && slot >= 1 && slot <= LIMITS.p25Contacts ? slot : null, mode: "p25", timeslot: 0,
				name: String(untag(x.name) ?? "").slice(0, LIMITS.name), type: x.type, id: Number(untag(x.id)) };
			if (x.system != null) c.system = sysById.get(String(untag(x.system))) ?? 256;
			else if (x.systemIndex != null) {
				const index = Number(untag(x.systemIndex));
				const check = hexIn(untag(x.systemCheck));
				const valid = check != null && check >= 0 && check <= 0xFFFF;
				c.system = Number.isInteger(index) && index >= 0 && index <= 255 && valid ? index + 1 : 256;
				c.systemCheck = valid ? check : 0xFFFF;
			}
			m.contacts.push(c);
			airByRef.set(String(untag(x.ref)), c);
		}
		resolveContactSystems(m);
	}
	const p25Lists = new Set();
	for (const x of Array.isArray(top?.rxLists) ? top.rxLists : []) {
		const l = values.listById?.get(String(untag(x?.list)));
		if (!l) { note("mode_unknown"); continue; }
		l.contacts = (Array.isArray(x.contacts) ? x.contacts : []).map((id) => {
			const ref = String(untag(id)), slot = /^#p25:(\d+)$/i.exec(ref);
			// The generic list owner holds slots1..1024, while P25 has341. Its last slot is a durable invalid P25
			// reference; FFFF would be discarded as a malformed generic list terminator on the next binary read.
			if (slot) { const n = Number(slot[1]); return n >= 1 && n <= LIMITS.contacts ? n : LIMITS.contacts; }
			return airByRef.get(ref) ?? values.contactById?.get(ref) ?? LIMITS.contacts;
		}).slice(0, LIMITS.rxListContacts);
		p25Lists.add(l);
	}

	// the radio-wide records: a kind the block leaves out takes its defaults
	m.modeSettings = {};
	const settings = top?.modeSettings && typeof top.modeSettings === "object" ? top.modeSettings : {};
	for (const k of KINDS) {
		const set = k.module.settings;
		if (set) m.modeSettings[k.id] = fieldsIn(set.decode(null), settings[k.short], set.fields, values);
	}

	for (const [c, b] of chans) {
		c.kind = 0;
		if (!b || b.mode == null) continue;
		const row = byName(KINDS, untag(b.mode));
		if (!row) {
			note("mode_unknown");
			continue;
		}
		const mod = row.module;
		Object.assign(c, {
			kind: row.id, system: b.system == null ? null : sysById.get(String(untag(b.system))) ?? null,
			tagTx: b.transmit == null ? !!mod.defaultTx : b.transmit === true, baseOwn: b.ownFrequencies === true,
			fallback: ref(b.fallback), params: fieldsIn(mod.newParams(), b.settings, mod.fields, values, mod.optionalParams), tagRaw: null,
		});
		for (const k of Object.keys(mod.flags ?? {})) c[k] = b[k] === true;
		if (mod.contactMode === "p25" && p25Lists.has(c.rxList)) c.params.p25Contacts = true;
		if (b.p25Contact != null || b.p25ContactSlot != null) {
			const slot = Number(untag(b.p25ContactSlot));
			c.contact = b.p25Contact != null ? airByRef.get(String(untag(b.p25Contact))) ?? 0xFFFF : Number.isInteger(slot) && slot >= 1 && slot <= 0xFFFF ? slot : 0xFFFF;
			// The reference's mode survives even when its row is missing: it must not become a legacy DMR slot.
			c.params.p25Contacts = true;
		}
		if (b.system != null && !c.system) note("mode_unknown");
	}
	return true;
}
