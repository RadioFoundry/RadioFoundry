// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The editor's RadioID source (static/js/codeplug/sources/): user.csv parsing, the API client against a fake fetch
// (direct, then the proxy; the user's key never sent through it; pages; cache), region selection, text templates, and
// the outputs (ID database, contacts). Nothing here reaches the network.

import { test } from "node:test";
import assert from "node:assert/strict";

import { parseUsers, fromApi, regionsOf, select, format, RadioIdApi, regionUrl, COUNTRIES } from "../static/js/codeplug/sources/radioid.js";
import { writeIddb, eraseIddb, addContacts, iddbEnd, iddbSize } from "../static/js/codeplug/sources/output.js";
import * as iddb from "../static/js/codeplug/iddb.js";
import { empty, newContact } from "../static/js/codeplug/model.js";
import { History } from "../static/js/editor/undo.js";

const CSV = [
	"RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY",
	"3101276,n4nqv,Spencer M,,Auburn,Alabama,United States",
	"1023007,VA3BOC,Hans Juergen,,Cornwall,Ontario,Canada",
	"2620001,DL1ABC,Jürgen,,München,Bayern,Germany",
	"3101277,KD4XYZ,\"Smith, Jo\",,Opelika,ALABAMA,United States",
	"3101276,DUP,,,,,",
	"abc,N0NE,,,,,",
	"3101300,,NoCall,,,,",
].join("\r\n");

const user = (o = {}) => ({ id: 3101276, call: "N4NQV", name: "Spencer M", last: "", city: "Auburn", state: "Alabama", country: "United States", ...o });

test("parseUsers: header row, quotes, duplicates and bad lines dropped, sorted by ID", () => {
	const u = parseUsers("﻿" + CSV);
	assert.deepEqual(u.map((x) => x.id), [1023007, 2620001, 3101276, 3101277]);
	assert.equal(u[2].call, "N4NQV");
	assert.equal(u[3].name, "Smith, Jo");
	assert.equal(u[1].city, "München");
});

test("parseUsers: a file without a header row uses RadioID's column order", () => {
	const u = parseUsers("3101276,N4NQV,Spencer,,Auburn,Alabama,United States\n");
	assert.deepEqual(u, [user({ name: "Spencer" })]);
});

test("regionsOf and select: countries, states case-insensitively, prefixes, ID range", () => {
	const u = parseUsers(CSV);
	const r = regionsOf(u);
	assert.equal(r[0].country, "United States");
	assert.equal(r[0].count, 2);
	assert.deepEqual(select(u, { regions: [{ country: "united states", state: "alabama" }] }).map((x) => x.id), [3101276, 3101277]);
	assert.deepEqual(select(u, { regions: [{ country: "Canada" }, { country: "Germany", state: "Bayern" }] }).map((x) => x.id), [1023007, 2620001]);
	assert.deepEqual(select(u, { prefixes: ["kd4", " "] }).map((x) => x.id), [3101277]);
	assert.deepEqual(select(u, { idMin: 3000000, idMax: 3101276 }).map((x) => x.id), [3101276]);
	assert.equal(select(u, {}).length, 4);
});

test("format: tokens, abbreviations, country codes, empty values drop with their separator", () => {
	const u = user();
	assert.equal(format(u, "{call} {first} {city} {st}"), "N4NQV Spencer Auburn AL");
	assert.equal(format(u, "{call} {name} ({cc})"), "N4NQV Spencer M (US)");
	assert.equal(format(user({ city: "" }), "{call}, {city} {st}"), "N4NQV AL");
	assert.equal(format(user({ city: "" }), "{call} ({city})"), "N4NQV");
	assert.equal(format(u, "{call} {name} {city}", { upper: true, max: 16 }), "N4NQV SPENCER M");
	assert.equal(format(user({ country: "Canada", state: "Ontario" }), "{st}/{cc}"), "ON/CA");
	assert.equal(format(user({ country: "Germany", state: "Bayern" }), "{st}"), "Bayern");
	assert.equal(format(u, "{id} {nope}"), "3101276 {nope}");
});

test("the region list has every country once, with ISO codes", () => {
	assert.ok(COUNTRIES.length > 150);
	assert.ok(COUNTRIES.every((c) => /^[A-Z]{2}$/.test(c.cc)));
	assert.ok(COUNTRIES.find((c) => c.name === "United States").states.includes("Alabama"));
});

// A fake RadioID: pages of a region's users; the direct URL fails as a browser without CORS headers would
function fakeFetch({ users, directWorks = false, log = [] }) {
	return async (url, opts) => {
		log.push({ url, headers: opts.headers });
		// corsfix refuses (an unregistered site); allorigins /get wraps the reply
		if (url.startsWith("https://proxy.corsfix.com/")) return { ok: false, status: 403, text: async () => "{}" };
		const proxied = url.startsWith("https://api.allorigins.win/get?url=");
		if (!proxied && !directWorks) throw new TypeError("Failed to fetch");
		const real = new URL(proxied ? decodeURIComponent(url.slice(url.indexOf("=") + 1)) : url);
		const page = Number(real.searchParams.get("page"));
		const st = real.searchParams.get("state");
		const all = users.filter((u) => u.country === real.searchParams.get("country") && (!st || u.state === st));
		const results = all.slice((page - 1) * 2, page * 2).map((u) => ({ id: u.id, callsign: u.call, fname: u.name, surname: "", city: u.city, state: u.state, country: u.country }));
		const body = JSON.stringify({ count: all.length, page, pages: Math.max(1, Math.ceil(all.length / 2)), results });
		return { ok: true, status: 200, text: async () => (proxied ? JSON.stringify({ contents: body }) : body) };
	};
}

const MANY = Array.from({ length: 5 }, (_, i) => user({ id: 3101000 + i, call: `K4A${i}` }));

test("API: direct fails, then the proxy; pages fetched in order; the key never goes to the proxy", async () => {
	const log = [];
	const api = new RadioIdApi({ key: "secret", fetchImpl: fakeFetch({ users: MANY, log }), wait: async () => {} });
	const seen = [];
	const got = await api.fetch({ country: "United States", state: "Alabama" }, { progress: (n, of) => seen.push([n, of]) });
	assert.deepEqual(got.map((u) => u.id), MANY.map((u) => u.id));
	assert.deepEqual(seen, [[1, 3], [2, 3], [3, 3]]);
	assert.equal(api.via, "proxy");
	assert.equal(log[0].headers["X-API-Token"], "secret");	// the one direct try
	assert.ok(log.slice(1).every((l) => !("X-API-Token" in l.headers)));
	// direct, corsfix, allorigins for page 1; then allorigins only (no direct try again, the working proxy first)
	assert.deepEqual(log.map((l) => new URL(l.url).host), ["database.radioid.net", "proxy.corsfix.com", "api.allorigins.win", "api.allorigins.win", "api.allorigins.win"]);
	// a second fetch of the same region comes from the cache
	await api.fetch({ country: "United States", state: "Alabama" });
	assert.equal(log.length, 5);
});

test("API: direct requests when RadioID allows them, with the key", async () => {
	const log = [];
	const api = new RadioIdApi({ key: "k", fetchImpl: fakeFetch({ users: MANY, directWorks: true, log }), wait: async () => {} });
	assert.deepEqual(await api.count({ country: "United States" }), { count: 5, pages: 3 });
	assert.equal(api.via, "direct");
	assert.equal(log[0].url, regionUrl({ country: "United States" }, 1));
	assert.equal(log[0].headers["X-API-Token"], "k");
});

test("API: a failure everywhere is reported, and a stop ends the fetch", async () => {
	const api = new RadioIdApi({ fetchImpl: async () => ({ ok: false, status: 503, text: async () => "" }), wait: async () => {} });
	await assert.rejects(api.count({ country: "Chad" }), { code: "api_failed" });
	const ctrl = new AbortController();
	const slow = new RadioIdApi({ fetchImpl: fakeFetch({ users: MANY }), wait: async () => ctrl.abort() });
	await assert.rejects(slow.fetch({ country: "United States" }, { signal: ctrl.signal }), { name: "AbortError" });
});

test("fromApi keeps only users with an ID and a call sign", () => {
	assert.deepEqual(fromApi([{ id: 1, callsign: "a1a" }, { id: 0, callsign: "X" }, { radio_id: 2, callsign: "" }]).map((u) => u.call), ["A1A"]);
});

test("ID database: written into a new image, read back, sized against the radio, undone", () => {
	const m = empty();
	const before = m.base;
	const h = new History();
	h.record(m);
	const users = parseUsers(CSV);
	const r = writeIddb(m, users, { template: "{call} {first} {st}", textLen: 16 });
	assert.notEqual(m.base, before);
	assert.equal(r.count, 4);
	assert.equal(m.iddb.count, 4);
	assert.deepEqual(iddb.read(m.base.bytes).map((x) => x.text), ["VA3BOC Hans ON", "DL1ABC Jurgen Ba", "N4NQV Spencer AL", "KD4XYZ Smith AL"]);
	h.undo(m);
	assert.equal(m.base, before);
	assert.equal(m.iddb, null);
	const end = iddbEnd({ codeplug: { base: 0, end: 0x120000 } });
	assert.equal(end, 0x120000);
	assert.equal(iddbEnd(null), Infinity);
	const s = iddbSize(1e6, 32, end);
	assert.equal(s.fit, iddb.capacity(iddb.recordSize(32), end));
	assert.equal(writeIddb(m, Array.from({ length: s.fit + 3 }, (_, i) => user({ id: i + 1 })), { template: "{call}", textLen: 32, end }).dropped, 3);
	eraseIddb(m);
	assert.equal(m.iddb, null);
});

test("contacts: private calls, existing IDs skipped, names in Latin-1 and 16 characters, the list's limit kept", () => {
	const m = empty();
	m.contacts.push(newContact({ number: 1, name: "Me", id: 3101276, type: "private" }));
	const r = addContacts(m, [...parseUsers(CSV), user({ id: 4600001, call: "BG7ABC", name: "Łukasz 王" })], { template: "{call} {name} {city}" });
	assert.deepEqual(r, { added: 4, existing: 1, full: 0 });
	assert.deepEqual(m.contacts.slice(1).map((c) => c.name), ["VA3BOC Hans Juer", "DL1ABC Jürgen Mü", "KD4XYZ Smith, Jo", "BG7ABC Lukasz Au"]);
	assert.ok(m.contacts.every((c) => c.type === "private"));
	const big = empty();
	const many = Array.from({ length: 1030 }, (_, i) => user({ id: i + 1, call: `K${i}` }));
	assert.deepEqual(addContacts(big, many, { template: "{call}" }), { added: 1024, existing: 0, full: 6 });
});
