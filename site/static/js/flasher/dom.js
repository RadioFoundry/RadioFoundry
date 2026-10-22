// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher page's DOM helpers and strings, shared by app.js and the views beside it (c7000ui.js).

import { note } from "../diag.js";

const STRINGS = JSON.parse(globalThis.document?.getElementById("i18n")?.textContent || "{}");
export const t = (key, fmt = {}) => (STRINGS[key] || key).replace(/\{(\w+)\}/g, (_, k) => (k in fmt ? fmt[k] : `{${k}}`));
export const ROOT = globalThis.document?.body.dataset.root || "";

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
	return isNaN(d) ? String(iso || "") : d.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
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

export function progressBar() {
	const bar = h("span");
	const text = h("span", { class: "small muted" });
	const el = h("div", {}, h("div", { class: "progress", role: "progressbar", "aria-valuemin": 0, "aria-valuemax": 100 }, bar), text);
	return {
		el,
		set(done, total, label = "") {
			const pct = total ? Math.round((100 * done) / total) : 0;
			bar.style.width = pct + "%";
			el.firstChild.setAttribute("aria-valuenow", pct);
			text.textContent = `${label} ${pct}%`.trim();
		},
	};
}

export function logBox() {
	const el = h("pre", { class: "log", "aria-live": "off" });
	return { el, add(line) { el.textContent += line + "\n"; el.scrollTop = el.scrollHeight; note(line); } };
}

export const notice = (text, kind = "") => h("p", { class: `notice ${kind}` }, text);
export const button = (label, onclick, { primary = false, disabled = false, danger = false, quiet = false } = {}) =>
	h("button", { class: `btn${primary ? " btn-primary" : ""}${danger ? " btn-danger" : ""}${quiet ? " btn-quiet" : ""}`, type: "button", onclick, disabled }, label);

export function panel(title, ...children) {
	return h("section", { class: "panel" }, h("h2", {}, title), ...children);
}
