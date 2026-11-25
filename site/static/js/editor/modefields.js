// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Turns the fields a mode module declares (codeplug/mode_*.js: {key, label, type, min, max, options, ...}) into the
// fields of the editor's forms (views.js formDialog), and parses what was typed for them. No mode is named here.
//
// Field types: "int" (min..max; blank: the field's blank value, or "any" (null) with any), "hex" (the same in hex),
// "freq" (MHz to Hz; zero: blank is 0), "bool", "select" (options: [value, label or i18n key]), "list" (whole numbers
// split on spaces or commas, at most count), "hexbytes" (bytes octets in hex, or blank), "text".
// A select whose options(r) read other fields of the record (a key list from the channel's system) sets dynamic: the
// form then rebuilds its list as those fields change, instead of keeping the one built when the form opened.

import { parseFreq, mhz } from "../codeplug/fields.js";

const lim = (v, r) => (typeof v === "function" ? v(r) : v);
export const label = (t, s) => (typeof s === "string" && s.startsWith("editor.") ? t(s) : s);

// What was typed for spec, for record r: {value} or {error}
export function parseSpec(spec, raw, t, r = {}) {
	const s = String(raw ?? "").trim();
	const lo = lim(spec.min, r), hi = lim(spec.max, r);
	const range = () => ({ error: t("editor.err.range", { min: spec.type === "hex" ? lo.toString(16).toUpperCase() : lo, max: spec.type === "hex" ? hi.toString(16).toUpperCase() : hi }) });
	switch (spec.type) {
	case "int":
	case "hex": {
		if (s === "" || (spec.any && s === "*")) return { value: spec.any ? null : typeof spec.blank === "function" ? spec.blank(r) : spec.blank ?? 0 };
		const n = spec.type === "hex" ? (/^(0x)?[0-9a-f]+$/i.test(s) ? parseInt(s.replace(/^0x/i, ""), 16) : NaN) : Number(s);
		return Number.isInteger(n) && n >= lo && n <= hi ? { value: n } : range();
	}
	case "freq": {
		if (s === "" && spec.zero) return { value: 0 };
		const hz = parseFreq(s);
		return hz === null ? { error: t("editor.err.bad_freq") } : { value: hz };
	}
	case "hexbytes": {
		// a key: exactly spec.bytes octets in hex (spaces, colons and dashes ignored), or none
		const x = s.replace(/[\s:-]/g, "").toUpperCase();
		if (x === "") return { value: "" };
		return new RegExp(`^[0-9A-F]{${2 * spec.bytes}}$`).test(x) ? { value: x } : { error: t("editor.err.hex_bytes", { n: spec.bytes }) };
	}
	case "list": {
		const xs = s.split(/[\s,;]+/).filter(Boolean).map(Number);
		if (xs.length > spec.count || xs.some((n) => !Number.isInteger(n) || n < lo || n > hi)) return { error: t("editor.err.list", { min: lo, max: hi, n: spec.count }) };
		return { value: xs };
	}
	default:
		return { value: s };
	}
}

// The text a field shows for value v
export function formatSpec(spec, v, t) {
	if (v == null) return spec.any ? "" : "";
	switch (spec.type) {
	case "hex": return spec.format ? spec.format(v) : v.toString(16).toUpperCase();
	case "freq": return v ? mhz(v) : "";
	case "list": return v.join(", ");
	case "select": return label(t, spec.options().find(([k]) => k === v)?.[1] ?? "");
	case "bool": return v ? "✓" : "";
	default: return String(v);
	}
}

// A form field for spec, its value held in holder.get(r)[spec.key]; holder.put(r, key, v) stores one (a new object,
// so undo's copies stay apart), holder.on(r) says whether the field applies to r at all (a field of another mode
// neither shows, nor sets, nor complains)
export function specField(spec, t, holder, id) {
	const on = holder.on ?? (() => true);
	const f = {
		key: `${id}.${spec.key}`,
		label: label(t, spec.label),
		get: (r) => (on(r) ? holder.get(r)?.[spec.key] : undefined),
		show: (r) => on(r) && (!spec.show || spec.show(holder.get(r) ?? {}, r)),
		help: spec.help ? t(spec.help) : null,
	};
	if (spec.type === "bool") return { ...f, type: "bool", set: (r, v) => on(r) && holder.put(r, spec.key, !!v) };
	if (spec.type === "select") {
		return { ...f, type: "select", dynamicOptions: !!spec.dynamic, options: (r) => spec.options(r).map(([v, l]) => [v, label(t, l)]), set: (r, v) => on(r) && holder.put(r, spec.key, v) };
	}
	return {
		...f, type: "text",
		format: (v) => formatSpec(spec, v === "" ? null : v, t),
		parse: (r, k, raw) => {
			if (!on(r)) return;
			const res = parseSpec(spec, raw, t, holder.get(r) ?? {});
			if (res.error) return res.error;
			holder.put(r, spec.key, res.value);
		},
	};
}
