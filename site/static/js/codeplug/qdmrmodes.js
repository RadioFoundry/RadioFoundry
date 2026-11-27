// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radioFoundry block of a qdmr YAML file (docs/spec/qdmr-yaml.md 3.7): what qdmr has no element for, carried in
// keys qdmr ignores, the way qdmr carries its own blocks for some radios. A top-level block holds the systems and the
// modes' radio-wide settings; a channel's block holds its mode and its Accept Invalid CRC setting. Generic: every
// field is written as the mode module (modes.js) names it, so a new mode needs nothing here. Keys are never written.

import { flow, Raw } from "./yaml.js";
import { KINDS, SYSTEM_TYPES, kindRow, typeRow } from "./modes.js";
import { newSystem, identityKey, systemTx, SHARE } from "./channelmodes.js";
import { sameValue } from "./modebytes.js";

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
export function exportModes(m, chRef, note, frequencyText) {
	const systems = (m.systems ?? []).filter((s) => {
		if (typeRow(s.type) && s.fields) return true;
		note("mode_unknown");
		return false;
	});
	const sysId = new Map(systems.map((s, i) => [s, `sys${i + 1}`]));
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
	const top = out.length || has ? { systems: out.length ? out : undefined, modeSettings: has ? settings : undefined } : undefined;

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
		if (b.system != null && !c.system) note("mode_unknown");
	}
	return true;
}
