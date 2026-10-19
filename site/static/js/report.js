// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Bug reports for the web tools. Every failure message offers "Report a Problem": a zip with the session log
// (diag.js), the browser's details and, if the user ticks it, their codeplug, to download and email. When the site
// is built with an error reporting address (site.json "reports"), errors are also sent automatically without the
// log or any radio data, and the dialog can send the whole report too. ?reporting=0 turns sending off.

import * as diag from "./diag.js";
import { h, t, ROOT, button, download } from "./flasher/dom.js";
import { zip } from "./flasher/zip.js";

// The Sentry browser bundle, loaded only when the site has a reporting address
const SENTRY_JS = "https://browser.sentry-cdn.com/10.75.3/bundle.min.js";
const SENTRY_SRI = "sha384-jS9CKUOSAB+VN9i8uqR3dDKFThyW/VFLVW7VU8FJIEA6hwAy3aY71i329xbTIxaN";

export const REPORTING = typeof location === "undefined" || new URLSearchParams(location.search).get("reporting") !== "0";

let attachmentsFn = () => [];
let sentry = null;		// the Sentry global once it's loaded and set up
let site = {};

// tool: "editor" or "flasher". context: () => the tool's state for a report (no personal data: it is also sent
// with automatic error reports). attachments: () => [{name, label, data: () => Uint8Array}], offered unticked.
export async function initReporting({ tool, context, attachments }) {
	if (attachments) attachmentsFn = attachments;
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

// Strings in an event that came from the page: callsigns, IDs and email addresses out
function scrubEvent(ev) {
	const s = (x) => (typeof x === "string" ? diag.scrub(x) : x);
	if (ev.message) ev.message = s(ev.message);
	for (const v of ev.exception?.values || []) v.value = s(v.value);
	if (ev.request?.url) ev.request.url = ev.request.url.split("?")[0];
	if (ev.contexts?.odr) ev.contexts.odr = JSON.parse(diag.scrub(JSON.stringify(ev.contexts.odr)));
	ev.user = undefined;
	return ev;
}

async function startSentry(dsn, tool) {
	await loadScript(SENTRY_JS, SENTRY_SRI);
	const S = globalThis.Sentry;
	S.init({
		dsn,
		release: site.git ? `odr-site@${site.git}` : undefined,
		environment: location.hostname,
		sendDefaultPii: false,
		defaultIntegrations: false,		// no console, click or fetch breadcrumbs: only what diag.error hands over
		beforeSend: (ev, hint) => (hint?.attachments?.length ? ev : scrubEvent(ev)),
	});
	S.setTag("tool", tool);
	S.setTag("web_serial", "serial" in navigator);
	S.setTag("web_usb", "usb" in navigator);
	sentry = S;
	diag.setSink((rec, e) => {
		if (rec.expected) return;
		const err = e instanceof Error ? e : new Error(rec.message);
		sentry.captureException(err, {
			tags: { stage: rec.stage, ...(rec.code ? { code: String(rec.code) } : {}) },
			contexts: { odr: { ...diag.context(), ...(rec.params ? { params: rec.params } : {}) } },
		});
	});
	diag.note("error reporting on");
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

export function reportDialog() {
	const text = h("textarea", { rows: 4, class: "report-text", placeholder: t("report.describe_hint") });
	const boxes = attachmentsFn().map((a) => {
		const box = h("input", { type: "checkbox" });
		return { a, box, el: h("label", { class: "report-check" }, box, " ", a.label) };
	});
	const result = h("div", { "aria-live": "polite" });
	const picked = () => boxes.filter((b) => b.box.checked).map((b) => b.a);
	const d = h("dialog", { class: "confirm report-dialog", "aria-labelledby": "report-title" });
	const save = button(t("report.download"), async () => {
		const { files } = await bundle(text.value, picked());
		download(`odr-bug-report-${diag.report().tool || "web"}-${stamp()}.zip`, await zip(files), "application/zip");
		result.replaceChildren(h("p", { class: "notice ok" }, t("report.downloaded")));
	}, { primary: !sentry });
	const send = sentry ? button(t("report.send"), async () => {
		send.disabled = true;
		try {
			const { rep, files } = await bundle(text.value, picked());
			const id = sentry.withScope((scope) => {
				for (const f of files) scope.addAttachment({ filename: f.name, data: f.data });
				scope.setTag("user_report", "yes");
				scope.setContext("odr", rep.context);
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
	}, { primary: true }) : null;
	d.append(
		h("h2", { id: "report-title" }, t("report.title")),
		h("p", {}, t(sentry ? "report.text_send" : "report.text")),
		h("label", { class: "report-label" }, t("report.describe"), text),
		...boxes.map((b) => b.el),
		boxes.length ? h("p", { class: "small muted" }, t("report.attach_note")) : null,
		result,
		h("div", { class: "confirm-actions" }, button(t("report.close"), () => d.close()), save, send),
	);
	d.addEventListener("close", () => d.remove());
	document.body.append(d);
	d.showModal();
	text.focus();
	return d;
}
