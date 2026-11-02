// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The page scripts' strings: the catalog the site build embeds in the page (<script id="i18n">, the page's language
// with English filled in), plural forms and numbers and dates in the page's language (docs/dev/website.md
// "Translating"). No DOM needed beyond reading the catalog, so the host tests use it with a catalog of their own.

// The page's language: <html lang>, which the site build sets
export const LANG = globalThis.document?.documentElement?.lang || "en";

export function catalog(strings, lang = LANG) {
	let rules = null;
	try {
		rules = new Intl.PluralRules(lang);
	} catch (e) { /* an unknown language: English rules below */ }
	const category = (n) => (rules ? rules.select(n) : n === 1 ? "one" : "other");
	// a number as the page's language writes it (grouping, decimal mark); options as Intl.NumberFormat takes them
	const num = (n, options) => {
		try {
			return new Intl.NumberFormat(lang, options).format(n);
		} catch (e) {
			return String(n);
		}
	};
	// key: a string, or a plural family (key.one, key.other, ... picked by the number n; other when n isn't one)
	const pick = (key, fmt) => {
		if (key in strings) return strings[key];
		if (!(`${key}.other` in strings)) return undefined;
		const form = typeof fmt.n === "number" ? strings[`${key}.${category(fmt.n)}`] : undefined;
		return form ?? strings[`${key}.other`];
	};
	// {name} placeholders filled from fmt; a number n is the count: written in the page's language
	const t = (key, fmt = {}) => (pick(key, fmt) ?? key).replace(/\{(\w+)\}/g, (_, k) => {
		if (!(k in fmt)) return `{${k}}`;
		return k === "n" && typeof fmt.n === "number" ? num(fmt.n) : fmt[k];
	});
	const has = (key) => key in strings || `${key}.other` in strings;
	// a catalog override of a text that comes from the repository (feature.<SWITCH>.title, ...), else that text
	const opt = (key, fallback, fmt = {}) => (has(key) ? t(key, fmt) : fallback);
	const date = (d, options) => {
		try {
			return d.toLocaleString(lang, options);
		} catch (e) {
			return d.toLocaleString(undefined, options);
		}
	};
	return { t, has, opt, num, date, lang };
}

// The page's own catalog
export const page = catalog(JSON.parse(globalThis.document?.getElementById("i18n")?.textContent || "{}"));
