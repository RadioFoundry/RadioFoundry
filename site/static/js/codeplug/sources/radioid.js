// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// RadioID.net, the DMR ID registry, as a contacts source: users from its user.csv (downloaded by the user) or from its
// public API for a few regions at a time. Users come out as {id, call, name, last, city, state, country}; the editor
// picks regions, filters them, and formats each user's text for the DMR ID database or a contact name.
//
// The API sends no CORS headers, so a page can't read it directly. Until RadioID allows this site's origin, requests
// that fail directly go through a public CORS proxy (PROXIES). The user's own API key (optional; RadioID asks for one)
// goes only on direct requests, in X-API-Token: never through the proxy, which would see it. Pages come 200 users at
// a time, one request after another, as RadioID asks of clients.

import { COUNTRIES, ABBREV } from "./regions.js";

export const ID = "radioid";
export const API = "https://database.radioid.net/api/dmr/user/";
export const DOWNLOAD = "https://radioid.net/static/user.csv";
export const SITE = "https://radioid.net/";
export const PER_PAGE = 200;
// Above this many pages a selection is better served by user.csv (about 5 s a page through the proxy)
export const MAX_PAGES = 40;

// Public CORS proxies, tried in order; the first that answers is kept for the session. Each: the URL for a RadioID
// URL, and the RadioID reply out of its answer. corsfix answers only on sites registered with it (and localhost).
export const PROXIES = [
	{ name: "corsfix", url: (u) => `https://proxy.corsfix.com/?${u}`, unwrap: (text) => text },
	{ name: "allorigins", url: (u) => `https://api.allorigins.win/get?url=${encodeURIComponent(u)}`, unwrap: (text) => JSON.parse(text).contents },
	{ name: "allorigins-raw", url: (u) => `https://api.allorigins.win/raw?url=${encodeURIComponent(u)}`, unwrap: (text) => text },
];

export { COUNTRIES };

// --- user.csv ---

const FIELDS = {
	id: ["radio_id", "radioid", "id", "dmr_id", "dmrid", "dmr id", "radio id"],
	call: ["callsign", "call", "call_sign", "call sign"],
	name: ["first_name", "fname", "firstname", "first name", "name"],
	last: ["last_name", "surname", "lastname", "last name"],
	city: ["city"],
	state: ["state", "province"],
	country: ["country"],
};
const NO_HEADER = ["id", "call", "name", "last", "city", "state", "country"];

// Users from RadioID's user.csv (or a file with the same columns, with or without a header row), sorted by ID, one per
// ID. Lines that aren't a user (no numeric ID or no call sign) are skipped.
export function parseUsers(text) {
	text = String(text).replace(/^﻿/, "");
	const lines = text.split(/\r?\n/);
	let cols = NO_HEADER, start = 0;
	const first = splitLine(lines[0] || "");
	if (first.length && !/^\d+$/.test(first[0].trim())) {
		const mapped = first.map((h) => Object.keys(FIELDS).find((k) => FIELDS[k].includes(h.trim().toLowerCase())) || null);
		if (mapped.includes("id") && mapped.includes("call")) {
			cols = mapped;
			start = 1;
		}
	}
	const seen = new Set(), out = [];
	for (let i = start; i < lines.length; i++) {
		if (!lines[i]) continue;
		const f = splitLine(lines[i]);
		const u = { id: 0, call: "", name: "", last: "", city: "", state: "", country: "" };
		cols.forEach((k, j) => {
			if (k && j < f.length) u[k] = f[j].trim();
		});
		const id = /^\d+$/.test(u.id) ? Number(u.id) : 0;
		if (!id || id > 0xFFFFFF || !u.call || seen.has(id)) continue;
		seen.add(id);
		u.id = id;
		u.call = u.call.toUpperCase();
		out.push(u);
	}
	return out.sort((a, b) => a.id - b.id);
}

// One CSV line (user.csv has no fields with line breaks)
function splitLine(line) {
	const out = [];
	let field = "", quoted = false;
	for (let i = 0; i < line.length; i++) {
		const c = line[i];
		if (quoted) {
			if (c === '"' && line[i + 1] === '"') {
				field += '"';
				i++;
			} else if (c === '"') quoted = false;
			else field += c;
		} else if (c === '"' && !field) quoted = true;
		else if (c === ",") {
			out.push(field);
			field = "";
		} else field += c;
	}
	out.push(field);
	return out;
}

// --- The API ---

export function fromApi(results) {
	return (results || []).map((r) => ({
		id: Number(r.id ?? r.radio_id) || 0, call: String(r.callsign || "").toUpperCase(), name: String(r.fname || "").trim(),
		last: String(r.surname || "").trim(), city: String(r.city || "").trim(), state: String(r.state || "").trim(),
		country: String(r.country || "").trim(),
	})).filter((u) => u.id >= 1 && u.id <= 0xFFFFFF && u.call);
}

export function regionUrl({ country, state }, page = 1) {
	const q = new URLSearchParams({ country });
	if (state) q.set("state", state);
	q.set("page", String(page));
	return `${API}?${q}`;
}

export class SourceError extends Error {
	constructor(code, params = {}) {
		super(code);
		Object.assign(this, { code, params });
	}
}

// fetchImpl and wait are replaceable for tests
export class RadioIdApi {
	constructor({ key = "", fetchImpl = (...a) => fetch(...a), proxies = PROXIES, wait = (ms) => new Promise((r) => setTimeout(r, ms)), gap = 300, timeout = 45000 } = {}) {
		Object.assign(this, { key, fetchImpl, proxies, wait, gap, timeout });
		this.direct = true;	// until a direct request fails; then only the proxies
		this.good = null;	// the proxy that last answered, tried first
		this.cache = new Map();	// url -> parsed page
		this.via = null;	// "direct" or "proxy", for the page to say
	}

	async page(region, n, signal) {
		const url = regionUrl(region, n);
		if (this.cache.has(url)) return this.cache.get(url);
		let data = null, last = null;
		const proxies = this.good ? [this.good, ...this.proxies.filter((p) => p !== this.good)] : this.proxies;
		// public proxies fail now and then: one more pass over them after a pause
		for (let pass = 0; pass < 2 && !data; pass++) {
			if (pass) await this.wait(2000);
			const tries = [...(this.direct && !pass ? [{ name: "direct" }] : []), ...proxies];
			for (const p of tries) {
				const direct = p.name === "direct";
				try {
					const text = await this.get(direct ? url : p.url(url), signal, direct && this.key ? { "X-API-Token": this.key } : {});
					data = JSON.parse(direct ? text : p.unwrap(text));
					this.via = direct ? "direct" : "proxy";
					if (!direct) this.good = p;
					break;
				} catch (e) {
					if (signal?.aborted) throw e;
					if (direct) this.direct = false;
					last = e;
				}
			}
		}
		if (!data) throw new SourceError("api_failed", { error: last?.message || String(last) });
		if (!Array.isArray(data.results)) throw new SourceError("api_failed", { error: "unexpected reply" });
		this.cache.set(url, data);
		return data;
	}

	async get(url, signal, headers = {}) {
		const ctrl = new AbortController();
		const stop = () => ctrl.abort();
		signal?.addEventListener("abort", stop, { once: true });
		const timer = setTimeout(stop, this.timeout);
		try {
			const r = await this.fetchImpl(url, { signal: ctrl.signal, headers: { Accept: "application/json", ...headers } });
			if (!r.ok) throw new Error(`HTTP ${r.status}`);
			return await r.text();
		} finally {
			clearTimeout(timer);
			signal?.removeEventListener("abort", stop);
		}
	}

	// How many users a region has (its first page, kept for fetch())
	async count(region, signal) {
		const d = await this.page(region, 1, signal);
		return { count: d.count ?? d.results.length, pages: d.pages ?? 1 };
	}

	// Every user of a region; progress(page, pages)
	async fetch(region, { progress = () => {}, signal } = {}) {
		const first = await this.page(region, 1, signal);
		const pages = first.pages ?? 1;
		const out = fromApi(first.results);
		progress(1, pages);
		for (let n = 2; n <= pages; n++) {
			if (signal?.aborted) throw new DOMException("stopped", "AbortError");
			await this.wait(this.gap);
			out.push(...fromApi((await this.page(region, n, signal)).results));
			progress(n, pages);
		}
		return out;
	}
}

// --- Regions and filters ---

// [{country, count, states: [{name, count}]}] of a set of users, most users first
export function regionsOf(users) {
	const map = new Map();
	for (const u of users) {
		const c = u.country || "";
		if (!map.has(c)) map.set(c, { country: c, count: 0, states: new Map() });
		const e = map.get(c);
		e.count++;
		const s = u.state || "";
		e.states.set(s, (e.states.get(s) || 0) + 1);
	}
	return [...map.values()].sort((a, b) => b.count - a.count).map((e) => ({
		country: e.country, count: e.count,
		states: [...e.states].filter(([s]) => s).map(([name, count]) => ({ name, count })).sort((a, b) => a.name.localeCompare(b.name)),
	}));
}

const key = (s) => String(s || "").trim().toLowerCase();

// Users in the regions ([{country, state?}]; none = all), with call signs starting with one of the prefixes, in the ID
// range
export function select(users, { regions = [], prefixes = [], idMin = 0, idMax = 0xFFFFFF } = {}) {
	const whole = new Set(), part = new Set();
	for (const r of regions) (r.state ? part.add(`${key(r.country)}\n${key(r.state)}`) : whole.add(key(r.country)));
	const pre = prefixes.map((p) => p.trim().toUpperCase()).filter(Boolean);
	return users.filter((u) => {
		if (u.id < idMin || u.id > idMax) return false;
		if (pre.length && !pre.some((p) => u.call.startsWith(p))) return false;
		if (!regions.length) return true;
		const c = key(u.country);
		return whole.has(c) || part.has(`${c}\n${key(u.state)}`);
	});
}

// --- Text ---

export const TOKENS = ["call", "name", "first", "last", "city", "state", "st", "country", "cc", "id"];

const CC = new Map(COUNTRIES.map((c) => [key(c.name), c.cc]));
const ABBR = new Map(Object.entries(ABBREV).flatMap(([c, m]) => Object.entries(m).map(([s, a]) => [`${key(c)}\n${key(s)}`, a])));

function values(u) {
	const name = u.name.replace(/\s+/g, " ").trim();
	return {
		call: u.call, name, first: name.split(" ")[0] || "", last: u.last, city: u.city, state: u.state,
		st: ABBR.get(`${key(u.country)}\n${key(u.state)}`) || u.state, country: u.country, cc: CC.get(key(u.country)) || u.country,
		id: String(u.id),
	};
}

// The template with each {token} replaced. An empty value drops out with the text just before it, so
// "{call}, {city} {st}" gives "N0CALL AL" for a user without a city. The text before the first token and after the
// last stay when anything is shown. upper: all capitals; then cut to max characters
export function format(u, template, { upper = false, max = 0 } = {}) {
	const v = values(u);
	const segs = String(template).split(/(\{[a-z]+\})/);	// literal, token, literal, ... literal
	let out = "", shown = false;
	for (let i = 1; i < segs.length; i += 2) {
		const name = segs[i].slice(1, -1);
		const val = TOKENS.includes(name) ? v[name] : segs[i];
		if (!val) continue;
		out += (shown ? segs[i - 1] : "") + val;
		shown = true;
	}
	if (shown) {
		const lastTok = segs.length - 2;
		const lastName = lastTok > 0 ? segs[lastTok].slice(1, -1) : "";
		out = segs[0] + out + (lastTok > 0 && (!TOKENS.includes(lastName) || v[lastName]) ? segs[segs.length - 1] : "");
	} else if (segs.length === 1) out = segs[0];
	out = out.replace(/\s+/g, " ").trim();
	if (upper) out = out.toUpperCase();
	return max ? out.slice(0, max).trim() : out;
}
