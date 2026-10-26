// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the web tools remember of a session, for bug reports: the steps they took, a summary of each frame on the
// wire (never its data), and every error with its stack and causes. Errors also go to the browser console. Kept in
// memory only: nothing leaves the page unless the user downloads a report, or error reporting is on (report.js).
// No DOM here, so the Node tests and the transports can use it.

const EVENTS_MAX = 1500;
const ERRORS_MAX = 50;

const events = [];
const errors = [];
const recs = new WeakMap();	// each error object's record
let current = null;		// the record of the error handled last (recorded now, or again)
const started = Date.now();
let info = { tool: "", version: "" };
let contextFn = () => ({});
let sink = null;

// tool: "editor" or "flasher"; version: the site's build; context: () => what the tool knows now (radio, task)
export function configure({ tool, version, context } = {}) {
	if (tool !== undefined) info.tool = tool;
	if (version !== undefined) info.version = version;
	if (context) contextFn = context;
}

// sink(record, error): called for every error recorded (report.js sends it on when reporting is on)
export function setSink(fn) {
	sink = fn;
}

const since = () => Date.now() - started;

function push(e) {
	events.push(e);
	if (events.length > EVENTS_MAX) events.splice(0, events.length - EVENTS_MAX);
}

// A step worth seeing in a report
export function note(text, data) {
	push({ t: since(), kind: "note", text, ...(data !== undefined ? { data: plain(data) } : {}) });
	if (globalThis.console?.debug) console.debug("[rfoundry]", text, ...(data !== undefined ? [data] : []));
}

// One frame on the wire, as text (command, address, length, result): reports only, not the console
export function wire(text) {
	push({ t: since(), kind: "wire", text });
}

// Errors the user caused or chose: kept in the log, never sent
export function expected(e) {
	return e?.name === "NotFoundError" || e?.name === "AbortError" || e?.code === "stopped" || e?.message === "stopped";
}

// Record an error once (the same object reaching two handlers is one error), print it and pass it to the sink.
// stage: what was being done ("write codeplug", "flash/write"). Returns the record, the first one for an error seen
// before.
export function error(stage, e, extra) {
	if (e && typeof e === "object" && recs.has(e)) {
		current = recs.get(e);
		return current;
	}
	const rec = { t: since(), stage, ...describe(e), ...(extra !== undefined ? { extra: plain(extra) } : {}), expected: expected(e) };
	if (e && typeof e === "object") recs.set(e, rec);
	current = rec;
	errors.push(rec);
	if (errors.length > ERRORS_MAX) errors.shift();
	push({ t: rec.t, kind: "error", text: `${stage}: ${rec.message}` });
	if (globalThis.console?.error) console.error(`[rfoundry] ${stage} failed:`, e, ...(extra !== undefined ? [extra] : []));
	try {
		sink?.(rec, e);
	} catch {
		// a reporting failure never becomes the user's problem
	}
	return rec;
}

// An error as plain data: name, message, code and params (TransportError), stack and its causes
export function describe(e, depth = 0) {
	if (!e || typeof e !== "object") return { name: typeof e, message: String(e) };
	const out = { name: e.name || e.constructor?.name || "Error", message: String(e.message ?? e) };
	if (e.code !== undefined) out.code = e.code;
	if (e.params && Object.keys(e.params).length) out.params = plain(e.params);
	if (e.stack) out.stack = String(e.stack).split("\n").slice(0, 30).join("\n");
	if (e.cause !== undefined && depth < 4) out.cause = describe(e.cause, depth + 1);
	return out;
}

// Something JSON can hold: byte arrays become their length, deep objects stop
export function plain(v, depth = 0) {
	if (v === null || v === undefined || typeof v === "boolean" || typeof v === "string") return v;
	if (typeof v === "number") return Number.isFinite(v) ? v : String(v);
	if (typeof v === "bigint") return v.toString();
	if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) return `<${v.byteLength} bytes>`;
	if (v instanceof Error) return describe(v);
	if (depth > 4) return "…";
	if (Array.isArray(v)) return v.slice(0, 50).map((x) => plain(x, depth + 1));
	if (typeof v === "object") {
		const o = {};
		for (const [k, x] of Object.entries(v).slice(0, 60)) if (typeof x !== "function") o[k] = plain(x, depth + 1);
		return o;
	}
	return String(v);
}

// The browser and what it supports
export function environment() {
	const n = globalThis.navigator || {};
	const env = {
		userAgent: n.userAgent || "",
		platform: n.userAgentData?.platform || n.platform || "",
		brands: n.userAgentData?.brands?.map((b) => `${b.brand} ${b.version}`) || [],
		mobile: n.userAgentData?.mobile ?? null,
		languages: n.languages ? [...n.languages] : [],
		webSerial: "serial" in n,
		webUsb: "usb" in n,
		online: n.onLine ?? null,
		timeZone: (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone; } catch { return ""; } })(),
	};
	if (globalThis.screen) env.screen = `${screen.width}x${screen.height}@${globalThis.devicePixelRatio || 1}`;
	if (globalThis.location) env.page = location.pathname + location.hash;
	return env;
}

export function context() {
	try {
		return plain(contextFn());
	} catch (e) {
		return { failed: String(e?.message || e) };
	}
}

// The whole report: what the user wrote, the browser, the tool's state, the errors and the log
export function report(description = "") {
	return {
		format: "rfoundry-bug-report/1",
		tool: info.tool,
		version: info.version,
		created: new Date().toISOString(),
		sessionSeconds: Math.round(since() / 1000),
		description,
		environment: environment(),
		context: context(),
		errors: errors.slice(),
		log: events.slice(),
	};
}

export const lastError = () => errors.at(-1) || null;
// The record of the error handled last, for the message that shows it (report.js reportedLine)
export const currentError = () => current;

// The last n steps and errors, for an automatic error report: what led up to the error (the port chosen, what the
// radio said), one line each, cut to 300 characters. Not the wire log, not the messages shown (they name the user's
// files), no USB serial numbers
export function trail(n = 20) {
	return events.filter((e) => e.kind !== "wire" && !String(e.text).startsWith("shown:")).slice(-n).map((e) => {
		const line = `${(e.t / 1000).toFixed(1)}s ${e.text}${e.data === undefined ? "" : ` ${JSON.stringify(e.data)}`}`
			.replace(/(serial["\s:=]+)(?=[0-9a-z]*\d)[0-9a-z]{6,}/gi, "$1[serial]");
		return line.length > 300 ? `${line.slice(0, 299)}…` : line;	// 300 at most, as the flash log takes them
	});
}

// For the tests
export function reset() {
	events.length = 0;
	errors.length = 0;
	current = null;
	contextFn = () => ({});
	sink = null;
	info = { tool: "", version: "" };
}

// Personal data out of text that leaves the browser without the user reading it first: callsigns, DMR IDs, email
// addresses. Downloaded reports keep it (the user sees and sends them); automatic reports go through this.
export function scrub(text) {
	return String(text)
		.replace(/[\w.+-]+@[\w-]+\.[\w.-]+/g, "[email]")
		.replace(/\b(?:[A-Z]{1,2}|\d[A-Z]|[A-Z]\d)\d[A-Z]{1,4}\b/g, "[call]")
		.replace(/(?<![\w.x])\d{6,8}(?![\w.])/g, "[id]");
}
