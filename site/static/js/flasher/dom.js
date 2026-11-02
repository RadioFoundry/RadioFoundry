// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flasher page's DOM helpers and strings, shared by app.js and the views beside it (c7000ui.js).

import { note } from "../diag.js";
import { page, LANG } from "../i18n.js";

// the page's strings (../i18n.js): t(key, fmt), tOr(key, repository text), num(n) and date(d, options) in its language
export const { t, num } = page;
export const tOr = page.opt;
export const date = page.date;
export { LANG };
export const ROOT = globalThis.document?.body.dataset.root || "";
// the root of the page's language (data-home): links to other pages, so they stay in the visitor's language
export const HOME = globalThis.document?.body.dataset.home ?? ROOT;

export function h(tag, attrs = {}, ...children) {
	const el = document.createElement(tag);
	for (const [k, v] of Object.entries(attrs || {})) {
		if (v === false || v === null || v === undefined) continue;
		if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
		else if (k === "class") el.className = v;
		else el.setAttribute(k, v === true ? "" : v);
	}
	for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) el.append(c instanceof Node ? c : String(c));
	return el;
}

// An SVG element (createElement would make an HTML one that doesn't draw)
export function svg(tag, attrs = {}, ...children) {
	const el = document.createElementNS("http://www.w3.org/2000/svg", tag);
	for (const [k, v] of Object.entries(attrs || {})) if (v !== false && v !== null && v !== undefined) el.setAttribute(k, v);
	for (const c of children.flat()) if (c) el.append(c);
	return el;
}

export function download(name, bytes, type = "application/octet-stream") {
	const url = URL.createObjectURL(new Blob([bytes], { type }));
	const a = h("a", { href: url, download: name });
	document.body.append(a);
	a.click();
	a.remove();
	setTimeout(() => URL.revokeObjectURL(url), 10000);
}

// A bundle's ISO time as people read it, in their own time zone
export function localTime(iso) {
	const d = new Date(iso);
	return isNaN(d) ? String(iso || "") : date(d, { dateStyle: "medium", timeStyle: "short" });
}

// What a backup holds, in words: one name per part, its file name when there's no word for it
export function partNames(parts) {
	return [...parts.values()].map((p) => {
		const k = `flash.part.${p.what}`;
		return (t(k) !== k ? t(k) : p.file) + (p.complete ? "" : ` (${t("flash.part.incomplete")})`);
	}).join(", ");
}

export async function readFile(input) {
	const f = input.files?.[0];
	return f ? { name: f.name, data: new Uint8Array(await f.arrayBuffer()) } : null;
}

export function filePicker(label, accept, onFile) {
	const input = h("input", { type: "file", accept });
	input.addEventListener("change", async () => {
		const f = await readFile(input);
		if (f) onFile(f);
	});
	return h("label", { class: "file-pick" }, h("span", {}, label), input);
}

// A bar that never looks frozen: it shimmers while work is under way (set() below 100%), and once nothing has changed
// for STALL_S seconds it says how long it has been waiting. Work that ends short of 100% (an error, a flash task
// finished) ends it with stopBars(): a failed step must not keep saying it waits for the radio.
const STALL_S = 4;
const running = new Set();

// Ends every bar's shimmer and wait line, wherever it stopped: errorNotice(), a "bad" notice() and flashFinished() call it
export function stopBars() {
	for (const stop of [...running]) stop();
}

export function progressBar() {
	const bar = h("span");
	const text = h("span", { class: "small muted" });
	const wait = h("span", { class: "small muted progress-wait", role: "status", hidden: true });
	const track = h("div", { class: "progress", role: "progressbar", "aria-valuemin": 0, "aria-valuemax": 100 }, bar);
	const el = h("div", {}, track, text, wait);
	let timer = null, changed = 0, key = "";
	const stop = () => { clearInterval(timer); timer = null; running.delete(stop); wait.hidden = true; track.className = "progress"; };
	const tick = () => {
		if (!el.isConnected) return stop();
		const s = Math.floor((Date.now() - changed) / 1000);
		wait.hidden = s < STALL_S;
		if (!wait.hidden) wait.textContent = t("flash.progress.waiting", { s });
	};
	return {
		el,
		set(done, total, label = "") {
			const pct = total ? Math.round((100 * done) / total) : 0;
			bar.style.width = pct + "%";
			track.setAttribute("aria-valuenow", pct);
			text.textContent = `${label} ${pct}%`.trim();
			// any new position or wording counts as life
			const now = `${done}/${total}/${label}`;
			if (now !== key) { key = now; changed = Date.now(); wait.hidden = true; }
			if (pct >= 100) return stop();
			track.className = "progress active";
			if (!timer) { timer = setInterval(tick, 1000); running.add(stop); }
		},
	};
}

export function logBox() {
	const el = h("pre", { class: "log", "aria-live": "off" });
	return { el, add(line) { el.textContent += line + "\n"; el.scrollTop = el.scrollHeight; note(line); } };
}

export const notice = (text, kind = "") => {
	if (kind === "bad") stopBars();
	return h("p", { class: `notice ${kind}` }, text);
};
// A turning circle for a wait with no progress to show. A redraw makes a new one: the delay keeps it at the angle the old
// one had, so a redraw every few seconds doesn't make it jump.
export const spinner = () => {
	const period = globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches ? 2400 : 800;	// as in site.css
	return h("span", { class: "spinner", "aria-hidden": "true", style: `animation-delay: -${Date.now() % period}ms` });
};
export const button = (label, onclick, { primary = false, disabled = false, danger = false, quiet = false } = {}) =>
	h("button", { class: `btn${primary ? " btn-primary" : ""}${danger ? " btn-danger" : ""}${quiet ? " btn-quiet" : ""}`, type: "button", onclick, disabled }, label);

export function panel(title, ...children) {
	return h("section", { class: "panel" }, h("h2", {}, title), ...children);
}
