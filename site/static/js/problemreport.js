// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A problem report to the project's backend (POST /v1/reports, then each file the user left ticked to
// /v1/reports/{id}/files): only when the user presses Send in Report a Problem (report.js). A file the backend refuses
// (too large, too many, too late, rate limited) is listed with the reason and the report still counts. No DOM here, so
// the Node tests can use it.

export const KINDS = new Set(["stock", "backup", "readout"]);
const TRAIL_MAX = 200, LINE_MAX = 300, ERROR_MAX = 4000, MESSAGE_MAX = 2000, CONTACT_MAX = 254;

// Why the backend refused a file, from its status (README "POST /v1/reports/{id}/files")
const REFUSED = { 400: "empty", 404: "report-gone", 409: "too-many", 410: "too-late", 413: "too-large", 415: "wrong-type", 422: "bad-kind", 429: "rate", 507: "store-full" };

const cut = (s, n) => (s === undefined || s === null ? undefined : String(s).slice(0, n));

// The request body: what the page knows (meta) and what the user wrote. Optional fields left out when empty
export function reportBody(meta, { message = "", contact = "", result = "failed" } = {}) {
	const body = {
		result, tool: meta.tool || "web", tool_version: cut(meta.tool_version, 40), board: cut(meta.board, 32), task: cut(meta.task, 16),
		step: cut(meta.step, 40), flash_id: meta.flash_id || undefined, decisions: meta.decisions || undefined,
		trail: (meta.trail || []).slice(-TRAIL_MAX).map((l) => String(l).slice(0, LINE_MAX)),
		error: cut(meta.error, ERROR_MAX), adapter: meta.adapter || undefined,
		message: cut(message.trim(), MESSAGE_MAX) || undefined, contact: cut(contact.trim(), CONTACT_MAX) || undefined,
	};
	for (const k of Object.keys(body)) if (body[k] === undefined || body[k] === "") delete body[k];
	return body;
}

// A plausible email address or nothing (the backend checks the same: x@y.z)
export const contactOk = (s) => !s.trim() || /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s.trim());

async function detail(r) {
	try {
		const j = await r.json();
		return typeof j.detail === "string" ? j.detail : `HTTP ${r.status}`;
	} catch (e) {
		return `HTTP ${r.status}`;
	}
}

// api: the backend's base URL (".../v1/"). files: [{name, kind, data: () => Uint8Array}]. Returns {id, sent: [name],
// refused: [{name, why, detail}]}; throws only when the report itself was not taken
export async function sendProblemReport({ api, body, files = [], fetchFn = globalThis.fetch }) {
	const r = await fetchFn(new URL("reports", api), { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
	if (r.status !== 201 && r.status !== 200) throw new Error(await detail(r));
	const j = await r.json();
	const out = { id: j.id, sent: [], refused: [] };
	for (const f of files) {
		if (!KINDS.has(f.kind)) {
			out.refused.push({ name: f.name, why: "bad-kind", detail: f.kind });
			continue;
		}
		let data;
		try {
			data = f.data();
		} catch (e) {
			out.refused.push({ name: f.name, why: "unreadable", detail: String(e?.message || e) });
			continue;
		}
		const q = new URLSearchParams({ kind: f.kind, name: String(f.name).slice(0, 200), token: j.token });
		try {
			const fr = await fetchFn(new URL(`reports/${j.id}/files?${q}`, api), { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: data });
			if (fr.ok) out.sent.push(f.name);
			else out.refused.push({ name: f.name, why: REFUSED[fr.status] || "failed", detail: await detail(fr) });
		} catch (e) {
			out.refused.push({ name: f.name, why: "network", detail: String(e?.message || e) });
		}
	}
	return out;
}

// Practice mode (#sim): the same exchange, answered here, nothing sent
export async function practiceFetch(url, { body } = {}) {
	const reply = (status, j) => ({ status, ok: status < 300, json: async () => j });
	if (String(url).includes("/files")) return body?.length ? reply(201, { name: "practice", len: body.length }) : reply(400, { detail: "empty" });
	return reply(201, { id: "practice", token: "practice", upload_until: "", max_files: 3, file_max_bytes: 0, max_bytes: 0 });
}
