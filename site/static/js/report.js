// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bug reports for the web tools. Every failure message offers "Report a Problem": a zip with the session log
// (diag.js), the browser's details and, if the user ticks it, their codeplug, to download and email. When the site
// is built with an error reporting address (site.json "reports"), errors are also sent automatically with the log's
// last steps (diag.trail) but no wire log or radio data, and the dialog can send the whole report too. ?reporting=0 turns sending off.

import * as diag from "./diag.js";
import { h, t, ROOT, button, download } from "./flasher/dom.js";
import { zip } from "./flasher/zip.js";
import { reportBody, sendProblemReport, contactOk, practiceFetch } from "./problemreport.js";

// The Sentry browser bundle, loaded only when the site has a reporting address
const SENTRY_JS = "https://browser.sentry-cdn.com/10.75.3/bundle.min.js";
const SENTRY_SRI = "sha384-jS9CKUOSAB+VN9i8uqR3dDKFThyW/VFLVW7VU8FJIEA6hwAy3aY71i329xbTIxaN";

export const REPORTING = typeof location === "undefined" || new URLSearchParams(location.search).get("reporting") !== "0";

// Errors go out automatically (Sentry is set up): a page can say "it has been reported" only then
export const reportingActive = () => !!sentry;

let attachmentsFn = () => [];
let sentry = null;		// the Sentry global once it's loaded and set up
let site = {};
let backend = null;		// {api, meta: () => what the page decided}: Send goes to the project's backend (problemreport.js)
let adapterNow = null;		// the USB device in use (flasher/adapter.js), as a tag on every error report

// tool: "editor" or "flasher". context: () => the tool's state for a report (no personal data: it is also sent
// with automatic error reports). attachments: () => [{name, label, data: () => Uint8Array, kind?, checked?}],
// offered unchecked unless checked. backend: {api, meta: () => reportBody's meta}: Send posts the report and the checked
// files there (with an email field), even with ?reporting=0, since the user asks for it; practice() true: answered
// by practiceFetch, nothing sent
export async function initReporting({ tool, context, attachments, backend: be }) {
	if (attachments) attachmentsFn = attachments;
	if (be) backend = be;
	diag.configure({ tool, context });
	addEventListener("error", (ev) => diag.error("uncaught", ev.error || new Error(ev.message || "script error")));
	addEventListener("unhandledrejection", (ev) => diag.error("unhandled", ev.reason));
	try {
		site = await (await fetch(`${ROOT}data/site.json`)).json();
	} catch (e) {
		diag.note("site.json not loaded", String(e?.message || e));
	}
	diag.configure({ version: site.version || site.git || "" });
	diag.note(`${tool} opened`, { version: site.version, git: site.git });
	if (REPORTING && site.reports?.sentry_dsn) await startSentry(site.reports.sentry_dsn, tool).catch((e) => diag.note("error reporting off", String(e?.message || e)));
}

function loadScript(src, integrity) {
	return new Promise((resolve, reject) => {
		const s = h("script", { src, integrity, crossorigin: "anonymous" });
		s.onload = resolve;
		s.onerror = () => reject(new Error(`could not load ${src}`));
		document.head.append(s);
	});
}

// Every string and number in a value, scrubbed one by one: scrubbing its JSON text instead turns a number (a DMR ID
// in a list) into [id] without quotes, the parse throws and Sentry drops the event
function scrubDeep(x) {
	if (typeof x === "string") return diag.scrub(x);
	if (typeof x === "number") {
		const t = diag.scrub(String(x));
		return t === String(x) ? x : t;
	}
	if (Array.isArray(x)) return x.map(scrubDeep);
	if (x && typeof x === "object") return Object.fromEntries(Object.entries(x).map(([k, v]) => [k, scrubDeep(v)]));
	return x;
}

// Strings in an event that came from the page: callsigns, IDs and email addresses out
export function scrubEvent(ev) {
	const s = (x) => (typeof x === "string" ? diag.scrub(x) : x);
	if (ev.message) ev.message = s(ev.message);
	for (const v of ev.exception?.values || []) v.value = s(v.value);
	if (ev.request?.url) ev.request.url = ev.request.url.split("?")[0];
	if (ev.contexts?.rfoundry) ev.contexts.rfoundry = scrubDeep(ev.contexts.rfoundry);
	ev.user = undefined;
	return ev;
}

async function startSentry(dsn, tool) {
	await loadScript(SENTRY_JS, SENTRY_SRI);
	const S = globalThis.Sentry;
	S.init({
		dsn,
		release: site.git ? `rfoundry-site@${site.git}` : undefined,
		environment: location.hostname,
		sendDefaultPii: false,
		defaultIntegrations: false,		// no console, click or fetch breadcrumbs: only what diag.error hands over
		normalizeDepth: 6,			// the context's params and trail are nested deeper than the default 3 keeps
		beforeSend: (ev, hint) => (hint?.attachments?.length ? ev : scrubEvent(ev)),
	});
	S.setTag("tool", tool);
	S.setTag("web_serial", "serial" in navigator);
	S.setTag("web_usb", "usb" in navigator);
	if (adapterNow) S.setTag("adapter_chip", adapterNow.chip || "none");
	sentry = S;
	// an event the ingest didn't take (blocked, offline, rate limited: no status, or not 2xx): its "reported" line goes
	// back to the report button. A send that worked always has a status
	S.getClient?.()?.on?.("afterSendEvent", (ev, res) => {
		const code = res?.statusCode;
		if (!(code >= 200 && code < 300)) undelivered(ev?.event_id);
	});
	diag.setSink((rec, e) => {
		if (rec.expected) return;
		const err = e instanceof Error ? e : new Error(rec.message);
		// the id ties the details the user may add (reportedLine) to this event
		rec.eventId = sentry.captureException(err, {
			tags: { stage: rec.stage, ...(rec.code ? { code: String(rec.code) } : {}) },
			contexts: { rfoundry: { ...diag.context(), ...(rec.params ? { params: rec.params } : {}), ...(adapterNow ? { adapter: adapterNow } : {}),
				trail: diag.trail(40) } },
		});
	});
	diag.note("error reporting on");
}

// The USB device the page now talks to ({vid, pid, chip, product} or null): a tag on the error reports from here on
export function setAdapter(a) {
	adapterNow = a || null;
	sentry?.setTag("adapter_chip", adapterNow?.chip || "none");
}

const stamp = () => new Date().toISOString().slice(0, 16).replace(/[-:]/g, "").replace("T", "-");

async function bundle(description, picked) {
	const rep = diag.report(description);
	const files = [{ name: "report.json", data: JSON.stringify(rep, null, 1) }];
	for (const a of picked) {
		try {
			files.push({ name: a.name, data: a.data() });
		} catch (e) {
			rep.attachmentErrors = [...(rep.attachmentErrors || []), `${a.name}: ${e?.message || e}`];
			files[0].data = JSON.stringify(rep, null, 1);
		}
	}
	return { rep, files };
}

// "Report a Problem": opens the dialog
export function reportButton({ quiet = true } = {}) {
	return button(t("report.button"), () => reportDialog(), { quiet });
}

// A line under a failure message: the report button with a word on what it's for
export function reportLine() {
	return h("p", { class: "report-line small" }, t("report.lead"), " ", reportButton());
}

const shown = new Map();	// event id -> its reported line
const refused = new Set();	// events the ingest refused
function undelivered(id) {
	if (!id) return;
	refused.add(id);
	const el = shown.get(id);
	if (el?.isConnected) el.replaceWith(reportLine());
}

// Under a failure message: when the error was just sent (error reporting on, not one the user caused), that it was
// reported and will be looked into, with a box to add what the user was doing (sent as Sentry feedback on that
// event) and the full report button; otherwise the report line; nothing for an error the user caused
// rec: the record of the error shown (diag.error's return); by default the error handled last
export function reportedLine(rec = diag.currentError()) {
	if (rec?.expected) return null;
	if (!sentry || !rec?.eventId || refused.has(rec.eventId)) return reportLine();
	const text = h("textarea", { rows: 2, maxlength: 1000, class: "report-text", placeholder: t("report.reported_hint") });
	const status = h("span", { class: "small", "aria-live": "polite" });
	const send = button(t("report.reported_send"), async () => {
		const message = text.value.trim();
		if (!message) return text.focus();
		send.disabled = true;
		try {
			const payload = { message: diag.scrub(message), associatedEventId: rec.eventId };
			if (sentry.captureFeedback) sentry.captureFeedback(payload);
			else sentry.captureMessage(`Details: ${payload.message.slice(0, 200)}`, { level: "info", tags: { details_for: rec.eventId } });
			if (!(await sentry.flush(8000))) throw new Error("not delivered in 8 s");
			status.textContent = t("report.reported_thanks");
			text.disabled = true;
		} catch (e) {
			diag.note("details not sent", String(e?.message || e));
			status.textContent = t("report.send_failed");
			send.disabled = false;
		}
	});
	const el = h("div", { class: "report-reported small" },
		h("p", {}, t("report.reported", { id: String(rec.eventId).slice(0, 8) })), text,
		h("p", { class: "report-line" }, send, status, reportButton()));
	shown.set(rec.eventId, el);
	return el;
}

// A failure shown to the user: the message (what happened and the likely cause, from the tool's error text) and,
// under it, that it was reported (reportedLine)
export function errorNotice(text, rec = diag.currentError()) {
	return h("div", { class: "notice bad" }, h("p", {}, text), reportedLine(rec));
}

const kb = (n) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`);

export function reportDialog() {
	const text = h("textarea", { rows: 4, class: "report-text", placeholder: t("report.describe_hint") });
	const boxes = attachmentsFn().map((a) => {
		const box = h("input", { type: "checkbox", checked: !!a.checked });
		const size = a.size ? ` (${kb(a.size)})` : "";
		return { a, box, el: h("label", { class: "report-check" }, box, " ", a.label, size) };
	});
	const email = backend ? h("input", { type: "email", class: "report-email", maxlength: 254, autocomplete: "email", placeholder: t("report.email_hint") }) : null;
	const result = h("div", { "aria-live": "polite" });
	const picked = () => boxes.filter((b) => b.box.checked).map((b) => b.a);
	const d = h("dialog", { class: "confirm report-dialog", "aria-labelledby": "report-title" });
	const save = button(t("report.download"), async () => {
		const { files } = await bundle(text.value, picked());
		download(`rfoundry-bug-report-${diag.report().tool || "web"}-${stamp()}.zip`, await zip(files), "application/zip");
		result.replaceChildren(h("p", { class: "notice ok" }, t("report.downloaded")));
	}, { primary: !sentry && !backend });
	// the project's backend: the report and the checked files, the user's message and address if given
	const toBackend = async () => {
		if (!contactOk(email.value)) {
			result.replaceChildren(h("p", { class: "notice bad" }, t("report.email_bad")));
			return email.focus();
		}
		send.disabled = true;
		result.replaceChildren(h("p", { class: "muted" }, t("report.sending")));
		try {
			const meta = { ...backend.meta(), trail: diag.trail(200).map((l) => diag.scrub(l)) };
			const body = reportBody(meta, { message: text.value, contact: email.value, result: meta.result || "failed" });
			const r = await sendProblemReport({ api: backend.api, body, files: picked(), ...(backend.practice?.() ? { fetchFn: practiceFetch } : {}) });
			diag.note("problem report sent", { id: r.id, files: r.sent.length, refused: r.refused.map((x) => x.why) });
			result.replaceChildren(h("div", { class: "notice ok" }, h("p", {}, t("report.sent_backend", { id: r.id })),
				r.refused.length ? h("ul", {}, r.refused.map((x) => h("li", {}, t("report.file_refused", { name: x.name, why: t(`report.refused.${x.why}`) })))) : null));
		} catch (e) {
			diag.note("problem report not sent", String(e?.message || e));
			result.replaceChildren(h("p", { class: "notice bad" }, t("report.send_failed_backend", { error: String(e?.message || e) })));
			send.disabled = false;
		}
	};
	const toSentry = async () => {
		send.disabled = true;
		try {
			const { rep, files } = await bundle(text.value, picked());
			const id = sentry.withScope((scope) => {
				for (const f of files) scope.addAttachment({ filename: f.name, data: f.data });
				scope.setTag("user_report", "yes");
				scope.setContext("rfoundry", rep.context);
				const last = rep.errors.at(-1);
				return sentry.captureMessage(`Bug report: ${(text.value || last?.message || "no description").slice(0, 120)}`, "error");
			});
			await sentry.flush(8000);
			result.replaceChildren(h("p", { class: "notice ok" }, t("report.sent", { id: String(id).slice(0, 8) })));
		} catch (e) {
			diag.note("report not sent", String(e?.message || e));
			result.replaceChildren(h("p", { class: "notice bad" }, t("report.send_failed")));
			send.disabled = false;
		}
	};
	const send = backend || sentry ? button(t("report.send"), backend ? toBackend : toSentry, { primary: true }) : null;
	d.append(...[	// append() would write a null as "null"
		h("h2", { id: "report-title" }, t("report.title")),
		h("p", {}, t(backend ? "report.text_backend" : sentry ? "report.text_send" : "report.text")),
		h("label", { class: "report-label" }, backend ? t("report.describe_optional") : t("report.describe"), text),
		email ? h("label", { class: "report-label" }, t("report.email"), email) : null,
		email ? h("p", { class: "small muted" }, t("report.optional_note")) : null,
		...boxes.map((b) => b.el),
		boxes.length ? h("p", { class: "small muted" }, t(backend ? "report.attach_note_files" : "report.attach_note")) : null,
		backend ? h("p", { class: "small muted" }, t("report.kept")) : null,
		result,
		h("div", { class: "confirm-actions" }, button(t("report.close"), () => d.close()), save, send),
	].filter(Boolean));
	d.addEventListener("close", () => d.remove());
	document.body.append(d);
	d.showModal();
	text.focus();
	return d;
}
