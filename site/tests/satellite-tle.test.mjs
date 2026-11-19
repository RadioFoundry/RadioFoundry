// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { buildTleBundle, DEFAULT_SELECTED_IDS, filterFmSatellites, MAX_SELECTED_PROFILES,
	parseTleCatalog, SATELLITES } from "../static/js/satellite-tle.js";

const SAMPLE = `ISS (ZARYA)\n1 25544U 98067A   26317.20662175  .00008559  00000+0  16132-3 0  9990\n2 25544  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594859\nSO-50\n1 27607U 02058C   26316.49281663  .00001582  00000+0  20311-3 0  9999\n2 27607  64.5539  30.0380 0068124 235.0518 124.4178 14.83333417286454\nAO-91\n1 43017U 17073E   26317.60524444  .00008151  00000+0  33970-3 0  9990\n2 43017  97.4443 181.7665 0144887 171.0350 189.3523 15.14807395487992\n`;

test("site satellite profiles match the firmware frequency table", async () => {
	const source = await readFile(new URL("../../core/satellite/frequency.c", import.meta.url), "utf8");
	const firmware = [...source.matchAll(/\{\s*(\d+)u,\s*"[^"]+",\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\s*\}/g)]
		.map((m) => m.slice(1).map(Number));
	assert.deepEqual(SATELLITES.map(({ id, rxHz, txHz, toneHz }) => [id, rxHz, txHz, toneHz]), firmware);
	assert.deepEqual(DEFAULT_SELECTED_IDS, [25544, 27607, 43017]);
});

test("only known FM satellites are included in the selectable catalog", () => {
	const records = [...SATELLITES.map(({ id, name }) => ({ catalog_id: id, name })),
		{ catalog_id: 99001, name: "Non-FM test object" }];
	assert.deepEqual(filterFmSatellites(records).map(({ catalog_id }) => catalog_id), SATELLITES.map(({ id }) => id));
});

test("AMSAT proxy requests and serves uncached bulletins", async () => {
	const source = await readFile(new URL("../../functions/api/amsat-tle.js", import.meta.url), "utf8");
	const { onRequestGet } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
	const oldFetch = globalThis.fetch;
	try {
		globalThis.fetch = async (url, options) => {
			assert.equal(url, "https://www.amsat.org/tle/daily-bulletin.txt");
			assert.equal(options.cache, "no-store");
			return new Response(SAMPLE);
		};
		const response = await onRequestGet();
		assert.equal(response.headers.get("cache-control"), "no-store");
		assert.equal(await response.text(), SAMPLE);
		globalThis.fetch = async () => new Response("", { status: 503 });
		assert.equal((await onRequestGet()).status, 502);
	} finally {
		globalThis.fetch = oldFetch;
	}
});

test("the page refreshes AMSAT before export and retains checked satellites through search", async () => {
	class Element {
		constructor() { this.children = []; this.listeners = {}; this.value = ""; this.dataset = {}; }
		addEventListener(type, handler) { this.listeners[type] = handler; }
		async fire(type) { return this.listeners[type]?.({ target: this }); }
		append(...children) { this.children.push(...children); }
		replaceChildren(...children) { this.children = children; }
		querySelectorAll(selector) {
			const descendants = this.children.flatMap((child) => [child, ...child.querySelectorAll("*")]);
			return selector === "*" ? descendants : descendants.filter((child) =>
				child.type === "checkbox" && (selector !== "input:checked" || child.checked));
		}
		click() {}
	}
	const ids = ["satellite-tle-file", "tle-amsat", "satellite-tle-options", "satellite-tle-build",
		"satellite-tle-push", "satellite-tle-status", "tle-provider", "satellite-tle-search"];
	const elements = Object.fromEntries(ids.map((id) => [id, new Element()]));
	const strings = await readFile(new URL("../i18n/en.json", import.meta.url), "utf8");
	const oldDocument = globalThis.document, oldFetch = globalThis.fetch;
	const oldCreate = URL.createObjectURL, oldRevoke = URL.revokeObjectURL, oldTimeout = globalThis.setTimeout;
	const navigatorDescriptor = Object.getOwnPropertyDescriptor(globalThis, "navigator");
	const downloads = [], requests = [];
	let linkModule, savedLink;
	let responseText;
	const catalogAt = (daysAgo) => {
		const date = new Date(Date.now() - daysAgo * 86400000);
		const day = Math.floor((Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate()) -
			Date.UTC(date.getUTCFullYear(), 0, 1)) / 86400000) + 1;
		const epoch = `${String(date.getUTCFullYear() % 100).padStart(2, "0")}${String(day).padStart(3, "0")}.00000000`;
		return SAMPLE.split("\n").map((line) => {
			if (!line.startsWith("1 ")) return line;
			const body = line.slice(0, 18) + epoch + line.slice(32, 68);
			const sum = [...body].reduce((total, char) =>
				total + (char >= "0" && char <= "9" ? Number(char) : char === "-" ? 1 : 0), 0);
			return body + sum % 10;
		}).join("\n");
	};
	try {
		globalThis.document = {
			getElementById: (id) => id === "i18n" ? { textContent: strings } : elements[id] || null,
			createElement: () => new Element(),
		};
		globalThis.fetch = async (url, options) => {
			requests.push({ url, options });
			return new Response(responseText);
		};
		URL.createObjectURL = (blob) => { downloads.push(blob); return "blob:test"; };
		URL.revokeObjectURL = () => {};
		globalThis.setTimeout = (fn) => { fn(); return 0; };
		await import("../static/js/tletool.js");
		responseText = catalogAt(8);
		const fetchCatalog = globalThis.fetch;
		let resolveFetch;
		globalThis.fetch = (...args) => new Promise((resolve) => {
			resolveFetch = async () => resolve(await fetchCatalog(...args));
		});
		const loading = elements["tle-amsat"].fire("click");
		assert.ok(elements["satellite-tle-build"].disabled && elements["satellite-tle-push"].disabled &&
			elements["satellite-tle-file"].disabled);
		await resolveFetch();
		await loading;
		globalThis.fetch = fetchCatalog;
		elements["satellite-tle-search"].value = "ISS";
		await elements["satellite-tle-search"].fire("input");
		responseText = catalogAt(0);
		await elements["satellite-tle-build"].fire("click");
		const bundle = JSON.parse(await downloads[0].text());
		assert.deepEqual(bundle.profile_ids, [25544, 27607, 43017]);
		assert.equal(bundle.records[0].line1, parseTleCatalog(responseText)[0].line1);
		assert.equal(requests.length, 2);
		assert.ok(requests.every(({ options }) => options.cache === "no-store"));

		responseText = catalogAt(0).split("AO-91\n")[0];
		await elements["satellite-tle-build"].fire("click");
		assert.equal(downloads.length, 1);
		assert.match(elements["satellite-tle-status"].textContent, /missing selected IDs: 43017/);

		elements["satellite-tle-search"].value = "";
		await elements["satellite-tle-search"].fire("input");
		for (const checkbox of elements["satellite-tle-options"].querySelectorAll("input")) {
			if (checkbox.value !== "25544") {
				checkbox.checked = false;
				await checkbox.fire("change");
			}
		}
		linkModule = await import("../static/js/flasher/link.js");
		savedLink = { open: linkModule.SerialStream.open, version: linkModule.Link.prototype.version,
			chip: linkModule.Link.prototype.chip, request: linkModule.Link.prototype.request };
		let opened = 0, transferred = [];
		Object.defineProperty(globalThis, "navigator", { configurable: true, value: {
			serial: { requestPort: async () => ({}) },
		} });
		linkModule.SerialStream.open = async () => { opened++; return { close: async () => {} }; };
		linkModule.Link.prototype.version = async () => ({ board: "test" });
		linkModule.Link.prototype.chip = async () => null;
		linkModule.Link.prototype.request = async (_command, payload) => {
			switch (payload[0]) {
			case 0: return new Uint8Array([7, 10, 0, 1, 0]);
			case 9: return new Uint8Array([1, 0]);
			case 3: {
				transferred.push(...payload.slice(5));
				return new Uint8Array([transferred.length & 255, transferred.length >> 8]);
			}
			case 12: case 4: return new Uint8Array(0);
			case 1: {
				const reply = new Uint8Array(9), view = new DataView(reply.buffer);
				reply[0] = 1;
				view.setUint32(1, 25544, true);
				const date = new Date();
				view.setUint32(5, Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate()) / 1000, true);
				return reply;
			}
			case 11: {
				const reply = new Uint8Array(15), view = new DataView(reply.buffer);
				reply[0] = 1;
				view.setUint32(1, 25544, true);
				view.setUint32(5, 437800000, true);
				view.setUint32(9, 145990000, true);
				view.setUint16(13, 670, true);
				return reply;
			}
			default: throw new Error(`unexpected operation ${payload[0]}`);
			}
		};
		responseText = catalogAt(0);
		await elements["satellite-tle-push"].fire("click");
		assert.equal(opened, 1);
		assert.match(elements["satellite-tle-status"].textContent, /profiles synced/);
		assert.ok(new TextDecoder().decode(Uint8Array.from(transferred)).includes(parseTleCatalog(responseText)[0].line1));
		assert.equal(requests.length, 4);

		globalThis.fetch = async () => new Response("", { status: 404 });
		await elements["satellite-tle-build"].fire("click");
		assert.equal(downloads.length, 1);
		assert.match(elements["satellite-tle-status"].textContent, /AMSAT API route was not found/);
		await elements["satellite-tle-push"].fire("click");
		assert.equal(opened, 1);
		assert.match(elements["satellite-tle-status"].textContent, /AMSAT API route was not found/);
		const fileText = catalogAt(1);
		elements["satellite-tle-file"].files = [{ size: fileText.length, text: async () => fileText }];
		await elements["satellite-tle-file"].fire("change");
		await elements["satellite-tle-build"].fire("click");
		assert.equal(downloads.length, 2);
		assert.equal(JSON.parse(await downloads[1].text()).records[0].line1, parseTleCatalog(fileText)[0].line1);
	} finally {
		if (savedLink) {
			linkModule.SerialStream.open = savedLink.open;
			linkModule.Link.prototype.version = savedLink.version;
			linkModule.Link.prototype.chip = savedLink.chip;
			linkModule.Link.prototype.request = savedLink.request;
		}
		if (navigatorDescriptor) Object.defineProperty(globalThis, "navigator", navigatorDescriptor);
		else delete globalThis.navigator;
		globalThis.document = oldDocument;
		globalThis.fetch = oldFetch;
		URL.createObjectURL = oldCreate;
		URL.revokeObjectURL = oldRevoke;
		globalThis.setTimeout = oldTimeout;
	}
});

test("parses a named TLE catalog and builds the selected JSON bundle", () => {
	const catalog = parseTleCatalog(`Daily bulletin\n\n${SAMPLE}`);
	const now = new Date("2026-11-14T00:00:00Z");
	const bundle = buildTleBundle(catalog, [43017, 25544], { now });
	assert.deepEqual(Object.keys(bundle), ["schema_version", "provider", "fetched_at", "profile_ids", "profiles", "records"]);
	assert.equal(bundle.schema_version, 3);
	assert.deepEqual(bundle.profile_ids, [25544, 43017]);
	assert.deepEqual(bundle.profiles.map(({ catalog_id }) => catalog_id), [25544, 43017]);
	assert.equal(bundle.fetched_at, "2026-11-14T00:00:00Z");
	assert.deepEqual(bundle.records.map((record) => record.catalog_id), [25544, 43017]);
	assert.equal(bundle.records[0].name, "ISS (ZARYA)");
	assert.equal(SATELLITES.find(({ id }) => id === 25544).name, "ISS");
});

test("rejects malformed, stale, and unselected catalog data", () => {
	const catalog = parseTleCatalog(SAMPLE);
	assert.throws(() => buildTleBundle(catalog, []), /Select at least one satellite/);
	assert.throws(() => buildTleBundle(catalog, [40908]), /missing selected IDs/);
	assert.throws(() => buildTleBundle(catalog, [...SATELLITES.slice(0, MAX_SELECTED_PROFILES + 1).map(({ id }) => id)],
		{ now: new Date("2026-11-14T00:00:00Z") }), /no more than 10/);
	const badChecksum = catalog.map((record) => ({ ...record }));
	badChecksum[0].line1 = badChecksum[0].line1.slice(0, 68) + (badChecksum[0].line1[68] === "0" ? "1" : "0");
	assert.throws(() => buildTleBundle(badChecksum, [25544], { now: new Date("2026-11-14T00:00:00Z") }), /checksum/);
	assert.throws(() => buildTleBundle(catalog, [25544], { now: new Date("2027-01-01T00:00:00Z") }), /30-day age limit/);
	assert.throws(() => parseTleCatalog("Name\n1 25544\n"), /incomplete TLE pair/);
});

test("accepts bulletin satellites with manually supplied frequency profiles", () => {
	const customText = `Manual satellite\n1 99001U 26001A   26317.20662175  .00008559  00000+0  16132-3 0  9998\n2 99001  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594858\n`;
	const catalog = parseTleCatalog(customText);
	const profile = { catalog_id: 99001, rx_hz: 145900000, tx_hz: 435100000, tx_tone_tenths: 885 };
	const bundle = buildTleBundle(catalog, [99001], {
		now: new Date("2026-11-14T00:00:00Z"), profiles: [profile],
	});
	assert.deepEqual(bundle.profiles, [profile]);
	assert.throws(() => buildTleBundle(catalog, [99001], { profiles: [] }), /enter valid RX/);
	assert.throws(() => buildTleBundle(catalog, [99001], {
		profiles: [{ ...profile, tx_hz: 1000000000 }],
	}), /enter valid RX/);
});