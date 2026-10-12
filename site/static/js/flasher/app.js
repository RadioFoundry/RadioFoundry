// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The web flasher: a step-by-step page over the modules beside it. Three tasks:
//
//   install   pick the radio and the build, find out what runs on the radio (backing it up if it runs ODR), decide
//             the codec, write through the bootloader, check the result and complete the backup
//   backup    read everything a radio running ODR can give into a bundle the user keeps
//   restore   put a radio back from a bundle: exactly as it was on ODR, or back on stock firmware
//
// Everything runs in the page: release images come from this site, custom images from the build API, stock files
// and backups from the user's disk, which never leave it. Add #sim to the address for a simulated radio (no hardware;
// for trying the flow), #sim=odr for one that already runs ODR.

import { hex, sha256, toHex, fromHex, startsWith } from "./bytes.js";
import * as stockfw from "./stockfw.js";
import { makePlan, KEEP, MERGE } from "./plan.js";
import { verify, segments, forDevBuild } from "./manifest.js";
import { Dfu, WebUsbTransport, checkKey, usbSupported } from "./dfu.js";
import { Link, SerialStream, ODR_USB, ODR_NET, probe, serialSupported } from "./link.js";
import { readRadio, makeBundle, openBundle, bundleName, describeFirmware, CODEPLUG_SPAN } from "./backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug } from "./restore.js";
import { Selection } from "./select.js";
import { decideCodec } from "./decide.js";
import { readBootKey } from "./bootkey.js";
import { FakeRadio, simulation, randomBytes, rng } from "./fake.js";

// ---------------------------------------------------------------------------------------------------------------
// strings and DOM

const STRINGS = JSON.parse(document.getElementById("i18n")?.textContent || "{}");
const t = (key, fmt = {}) => (STRINGS[key] || key).replace(/\{(\w+)\}/g, (_, k) => (k in fmt ? fmt[k] : `{${k}}`));
const ROOT = document.body.dataset.root || "";

function h(tag, attrs = {}, ...children) {
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

const app = document.getElementById("flasher");

function download(name, bytes, type = "application/octet-stream") {
	const url = URL.createObjectURL(new Blob([bytes], { type }));
	const a = h("a", { href: url, download: name });
	document.body.append(a);
	a.click();
	a.remove();
	setTimeout(() => URL.revokeObjectURL(url), 10000);
}

async function readFile(input) {
	const f = input.files?.[0];
	return f ? { name: f.name, data: new Uint8Array(await f.arrayBuffer()) } : null;
}

function filePicker(label, accept, onFile) {
	const input = h("input", { type: "file", accept });
	input.addEventListener("change", async () => {
		const f = await readFile(input);
		if (f) onFile(f);
	});
	return h("label", { class: "file-pick" }, h("span", {}, label), input);
}

function progressBar() {
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

function logBox() {
	const el = h("pre", { class: "log", "aria-live": "off" });
	return { el, add(line) { el.textContent += line + "\n"; el.scrollTop = el.scrollHeight; } };
}

const notice = (text, kind = "") => h("p", { class: `notice ${kind}` }, text);
const button = (label, onclick, { primary = false, disabled = false, danger = false } = {}) =>
	h("button", { class: `btn${primary ? " btn-primary" : ""}${danger ? " btn-danger" : ""}`, type: "button", onclick, disabled }, label);

class NetworkModeError extends Error {}

function errorText(e) {
	if (e instanceof NetworkModeError) return t("flash.net.found");
	if (e?.name === "NotFoundError") return t("flash.err.no_device");
	if (e?.name === "SecurityError") return t("flash.err.security");
	return e?.message || String(e);
}

// ---------------------------------------------------------------------------------------------------------------
// state

// A fresh start; reset() puts these back (a Set, an array) rather than nulling them
const initialState = () => ({
	task: null, step: 0, data: null, radio: null, board: null, selection: null, sim: null, fakeRadio: null,
	firmware: null,		// {manifest, image, source}
	now: null,		// what runs on the radio: {kind: "odr", probe} | {kind: "stock", version} | {kind: "unknown"}
	stock: null,		// {name, data, info: identify()}
	key: null,
	decision: null,
	backupParts: [], backupDone: false,
	bundle: null, restoreTarget: null,
	link: null,
	customPicked: false,	// the Custom card picked before any feature changed
	build: null,		// a custom build in progress: {selection, text, error}
	useId: null,		// the flash log's id for this use (see useId)
	openGroups: new Set(),	// feature groups expanded on the build step
	openTip: null,		// the feature whose ⓘ bubble is open
});
const S = initialState();

// Custom builds and the flash log
const API = "https://odr.spencerfowler.com/v1/";
// minimal non-invasive reporting for helping the ODR project track programming failures; opt out with
// ?reporting=0
const FLASH_LOG = new URLSearchParams(location.search).get("reporting") !== "0";

async function api(method, path, body) {
	const r = await fetch(new URL(path, API), body ? { method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : { method });
	const j = await r.json().catch(() => ({}));
	if (!r.ok) throw new Error(j.detail || `HTTP ${r.status}`);
	return j;
}

// One id per use: the custom build requests and the write that follows share it; each write ends the use
const useId = () => (FLASH_LOG && !S.sim ? (S.useId ||= crypto.randomUUID()) : undefined);

// Fire and forget: a flash never waits for these or fails because of them
function flashStarted(what) {
	const id = useId();
	if (!id) return null;
	S.useId = null;
	const sent = fetch(new URL("flashes", API), { method: "POST", keepalive: true, headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ id, tool: "web", tool_version: S.data.site.version || "", ...what }) }).catch(() => {});
	return { id, sent };
}

async function flashFinished(started, error = null) {
	if (!started) return;
	await started.sent;
	const body = error ? { result: "fail", reason: errorText(error).slice(0, 300) } : { result: "success" };
	fetch(new URL(`flashes/${started.id}`, API), { method: "PATCH", keepalive: true, headers: { "Content-Type": "application/json" },
		body: JSON.stringify(body) }).catch(() => {});
}

const STEPS = {
	install: ["radio", "build", "now", "codec", "write", "done"],
	backup: ["radio", "backup"],
	restore: ["bundle", "restore"],
};

async function loadData() {
	const get = async (name) => (await fetch(`${ROOT}data/${name}`)).json();
	const [radios, features, variants, fb, releases, site] = await Promise.all(
		["radios.json", "features.json", "variants.json", "flash-boards.json", "releases.json", "site.json"].map(get));
	return { radios: radios.radios, features: features.features, groups: features.groups, variants: variants.variants, fb, releases: releases.releases, site };
}

const fb = () => (S.sim ? S.sim.fb : S.data.fb);
// Every image comes from the build service, so a radio needs only a bootloader the page speaks
const flashable = (r) => r.flash.backend === "anyroad-dfu" && !!S.data.fb.boards[r.flash.board]?.key_sha256;

// Build options (radio.json flash.build_options, such as the MD-380's band): all chosen, and the value a radio running ODR
// reports by its debug link name
const buildOptions = () => S.radio?.flash.build_options || [];
const optionsChosen = () => buildOptions().every((o) => S.buildOptions?.[o.name]);
function optionsFromProbe(p) {
	for (const o of buildOptions()) {
		const v = o.values.find((x) => x.board_name === p.board);
		if (v) S.buildOptions = { ...S.buildOptions, [o.name]: v.id };
	}
}
// the option a radio's debug link name says, if it contradicts the one chosen: [option, value] or null
function optionMismatch(p) {
	for (const o of buildOptions()) {
		const v = o.values.find((x) => x.board_name === p.board);
		if (v && S.buildOptions?.[o.name] && S.buildOptions[o.name] !== v.id) return [o, v];
	}
	return null;
}

// ---------------------------------------------------------------------------------------------------------------
// connections (real or simulated)

async function connectSerial({ quiet = false } = {}) {
	if (S.link) return S.link;
	let stream;
	if (S.fakeRadio) stream = S.fakeRadio.serialStream();
	else {
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		if (await networkModeSeen()) throw new NetworkModeError();
		const granted = (await navigator.serial.getPorts()).filter((p) => {
			const i = p.getInfo();
			return i.usbVendorId === ODR_USB.usbVendorId && i.usbProductId === ODR_USB.usbProductId;
		});
		stream = granted.length && quiet ? await SerialStream.open(granted[0]) : granted.length ? await SerialStream.open(granted[0]) : await SerialStream.request();
	}
	S.link = new Link(stream);
	return S.link;
}

// A radio in USB network mode that this page was allowed to see before (no prompt)
async function networkModeSeen() {
	if (!usbSupported()) return false;
	try {
		return (await navigator.usb.getDevices()).some((d) => d.vendorId === ODR_NET.vendorId && d.productId === ODR_NET.productId);
	} catch (e) {
		return false;
	}
}

// Under every "connect the radio" button: when the serial chooser lists nothing, look for the radio as a network
// adapter and say how to switch it back
function serialHelp() {
	const out = h("div");
	const check = h("a", { href: "#", class: "small", onclick: async (e) => {
		e.preventDefault();
		if (S.fakeRadio) return;
		if (!usbSupported()) return out.replaceChildren(notice(t("flash.err.no_usb"), "bad"));
		try {
			await navigator.usb.requestDevice({ filters: [ODR_NET] });
			out.replaceChildren(networkModeHelp());
		} catch (err) {
			out.replaceChildren(notice(err?.name === "NotFoundError" ? t("flash.net.none") : errorText(err)));
		}
	} }, t("flash.net.check"));
	return h("div", {}, h("p", { class: "small" }, check), out);
}

function networkModeHelp() {
	return h("div", { class: "notice info" },
		h("p", {}, h("b", {}, t("flash.net.found"))),
		h("ol", { class: "steps" }, h("li", {}, t("flash.net.step1")), h("li", {}, t("flash.net.step2")), h("li", {}, t("flash.net.step3"))),
		h("p", { class: "small" }, t("flash.net.why")));
}

function connectError(e) {
	return e instanceof NetworkModeError ? networkModeHelp() : notice(errorText(e), "bad");
}

async function dropSerial() {
	try { await S.link?.s.close(); } catch (e) { /* gone */ }
	S.link = null;
}

async function connectBootloader() {
	if (S.fakeRadio) {
		if (S.fakeRadio.mode !== "bootloader") throw new Error(t("flash.err.not_in_bootloader"));
		return S.fakeRadio.dfuTransport();
	}
	if (!usbSupported()) throw new Error(t("flash.err.no_usb"));
	return WebUsbTransport.request(S.board);
}

const KEY_STORE = (id) => `odr-key-${id}`;

// Windows needs a driver for the bootloader; nobody else should read about it
const onWindows = () => /win/i.test(navigator.userAgentData?.platform || navigator.platform || "");
const WINUSB_STORE = "odr-winusb-ok";

// Zadig steps (docs/user/02-installing.md). Open until this computer has reached a bootloader once, and always after a
// failed connect: without the driver Chrome either doesn't list the bootloader or can't open it.
function windowsDriverHelp({ failed = false } = {}) {
	let seen = false;
	try { seen = !!localStorage.getItem(WINUSB_STORE); } catch (e) { /* not remembered */ }
	const hex = (n) => n.toString(16).toUpperCase().padStart(4, "0");
	const usbId = S.board.dfu_ids ? S.board.dfu_ids.map(hex).join(" ") : "";
	return h("details", { class: `notice${failed ? " action" : ""}`, open: failed || !seen },
		h("summary", {}, t("flash.win.title")),
		failed ? h("p", {}, t("flash.win.failed")) : null,
		h("ol", { class: "steps" },
			h("li", {}, t("flash.win.step1"), " ", h("a", { href: "https://zadig.akeo.ie", target: "_blank", rel: "noopener" }, "zadig.akeo.ie")),
			h("li", {}, t("flash.win.step2")),
			h("li", {}, t("flash.win.step3", { product: S.board.dfu_product || "?", usb_id: usbId })),
			h("li", {}, t("flash.win.step4")),
			h("li", {}, t("flash.win.step5"))),
		h("p", { class: "small muted" }, t("flash.win.note")));
}

function savedKey(board) {
	try {
		const k = localStorage.getItem(KEY_STORE(board.id));
		return k ? fromHex(k) : null;
	} catch (e) {
		return null;
	}
}

// A radio that runs ODR hands over its bootloader key (bootkey.js), so updating it needs no stock file. Quiet when it
// can't: the stock file still works.
async function keyFromRadio(link) {
	if (S.key || !S.board?.key_sha256) return;
	const key = await readBootKey(link, S.board, fb().key_len);
	if (!key) return;
	S.key = key;
	S.keyFromRadio = true;
	if (!S.sim) try { localStorage.setItem(KEY_STORE(S.board.id), toHex(key)); } catch (e) { /* not remembered */ }
}

async function getKey(board) {
	if (S.key) return S.key;
	if (S.sim) return (S.key = S.sim.key);
	const saved = savedKey(board);
	if (saved) {
		try { return (S.key = await checkKey(saved, board)); } catch (e) { /* stale: derive again */ }
	}
	if (!S.stock) throw new Error(t("flash.err.need_stock_for_key"));
	S.key = await checkKey(await stockfw.deriveKey(S.stock.data, board.key_sha256, fb()), board);
	try { localStorage.setItem(KEY_STORE(board.id), toHex(S.key)); } catch (e) { /* not remembered */ }
	return S.key;
}

// ---------------------------------------------------------------------------------------------------------------
// layout

function render() {
	app.replaceChildren();
	if (!S.task) return app.append(startView());
	const steps = STEPS[S.task];
	app.append(h("ol", { class: "stepper" }, steps.map((s, i) =>
		h("li", { class: i < S.step ? "done" : "", "aria-current": i === S.step ? "step" : false }, t(`flash.step.${s}`)))));
	if (S.sim) app.append(notice(t("flash.sim.banner"), "info"));
	// One bar of buttons under every step: back, start over, and the step's own way forward, which the view sets (and
	// changes when it redraws) through S.forward
	const info = h("div", { class: "nav-info" }), fwd = h("div", { class: "nav-forward" });
	S.forward = (btn, text = null) => {
		fwd.replaceChildren(...[btn].filter(Boolean));
		info.replaceChildren(...[text].filter(Boolean));
	};
	const view = { radio: radioView, build: buildView, now: nowView, codec: codecView, write: writeView, done: doneView,
		backup: backupView, bundle: bundleView, restore: restoreView }[steps[S.step]];
	app.append(view());
	app.append(h("div", { class: "nav-bar" },
		h("div", { class: "nav-back" },
			S.step > 0 ? button("← " + t("flash.back"), () => { S.step--; render(); }) : null,
			h("button", { class: "btn btn-quiet", type: "button", onclick: () => reset() }, t("flash.start_over"))),
		info, fwd));
}

function next() {
	S.step++;
	render();
	app.scrollIntoView({ behavior: "smooth", block: "start" });
}

async function reset() {
	await dropSerial();
	const keep = { data: S.data, sim: S.sim, fakeRadio: S.fakeRadio };
	// exactly a fresh load: fields set along the way (buildOptions, keyFromRadio, ...) go, the rest start over
	for (const k of Object.keys(S)) delete S[k];
	Object.assign(S, initialState(), keep);
	render();
}

function panel(title, ...children) {
	return h("section", { class: "panel" }, h("h2", {}, title), ...children);
}

// ---------------------------------------------------------------------------------------------------------------
// start

function startView() {
	// one banner: both APIs missing (Safari, Firefox), or the one that is
	const usb = S.sim || usbSupported(), serial = S.sim || serialSupported();
	const support = !usb && !serial ? [notice(t("flash.support.none"), "bad")]
		: [!usb && notice(t("flash.support.usb"), "bad"), !serial && notice(t("flash.support.serial"), "bad")].filter(Boolean);
	const choice = (task, icon) => h("button", { class: "choice", type: "button", onclick: () => { S.task = task; S.step = 0; render(); } },
		h("strong", {}, `${icon} ${t(`flash.task.${task}`)}`), h("span", { class: "small muted" }, t(`flash.task.${task}.text`)));
	return h("div", { class: "flasher" },
		...support,
		panel(t("flash.start.title"),
			h("div", { class: "choice-grid" }, choice("install", "⤓"), choice("backup", "⎘"), choice("restore", "↺"))),
		h("details", { class: "small" }, h("summary", {}, t("flash.start.privacy_title")), h("p", {}, t("flash.start.privacy"))),
		h("p", { class: "small muted" }, S.sim ? h("a", { href: "#", onclick: (e) => { e.preventDefault(); location.hash = ""; location.reload(); } }, t("flash.sim.leave"))
			: h("a", { href: "#sim", onclick: () => setTimeout(() => location.reload(), 0) }, t("flash.sim.try"))));
}

// ---------------------------------------------------------------------------------------------------------------
// install and backup: the radio

function radioView() {
	const list = h("div", { class: "choice-grid" }, S.data.radios.map((r) => {
		const ok = S.task === "backup" ? r.backup.odr === "full" : flashable(r);
		return h("button", { class: "choice", type: "button", "aria-pressed": String(S.radio?.id === r.id), disabled: !ok,
			onclick: () => { selectRadio(r); next(); } },
		h("img", { src: ROOT + r.image, alt: "" }), h("strong", {}, r.name),
		h("span", { class: "small muted" }, r.also.join(", ")),
		h("span", { class: "small" }, ok ? t(`status.${r.status}`) : t("flash.radio.not_yet")));
	}));
	const detect = button(t("flash.radio.detect"), async () => {
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.err.no_answer"));
			const r = S.data.radios.find((x) => S.data.fb.boards[x.flash.board]?.debuglink_names.includes(p.board));
			if (!r) throw new Error(t("flash.err.unknown_board", { board: p.board }));
			selectRadio(r);
			optionsFromProbe(p);
			S.now = { kind: "odr", probe: p };
			await keyFromRadio(link);
			next();
		} catch (e) {
			status.replaceChildren(connectError(e));
		}
	});
	const status = h("div");
	return panel(t("flash.radio.title"), h("p", {}, t("flash.radio.lead")), list,
		h("div", { class: "actions" }, detect), h("p", { class: "small muted" }, t("flash.radio.detect_hint")), serialHelp(), status);
}

function selectRadio(r) {
	S.radio = r;
	S.board = fb().boards[r.flash.board];
	S.selection = new Selection(r, S.data.features, S.data.variants);
	S.selection.useVariant("full");
	S.firmware = null;
	S.buildOptions = {};
}

// ---------------------------------------------------------------------------------------------------------------
// install: the build (variant, a la carte)

function buildView() {
	const sel = S.selection;
	const wrap = h("div", { class: "flasher" });
	const vtitle = (v) => (t(`variant.${v.id}.title`) !== `variant.${v.id}.title` ? t(`variant.${v.id}.title`) : v.title);
	const gtitle = (g) => (t(`group.${slug(g)}`) !== `group.${slug(g)}` ? t(`group.${slug(g)}`) : g);
	const supportedCount = S.data.features.filter((f) => sel.supported(f.name)).length;
	// The build type follows the checkboxes: a selection equal to a prebuilt variant is that variant, anything else is
	// custom. The Custom card can also be picked by hand before any change (it opens the groups).
	const change = (name, value) => {
		sel.set(name, value);
		S.customPicked = false;
		draw();
	};
	// Continue gets the image first: built on request, then on to the next step
	const go = async () => {
		if (!S.firmware && !S.sim) await requestBuild(draw);
		if (S.firmware || S.sim) next();
	};
	const forward = S.forward;
	const draw = () => {
		const match = S.customPicked ? null : sel.matches();
		// an image built for another selection no longer applies
		if (S.firmware?.selection && S.firmware.selection !== selectionKey(sel)) S.firmware = null;
		const card = (checked, onclick, ...body) => h("button", { class: "choice", type: "button", role: "radio",
			"aria-checked": String(checked), onclick }, ...body);
		const options = buildOptions().map((o) => h("div", {},
			h("h3", {}, o.title), o.help ? h("p", { class: "small muted" }, o.help) : null,
			h("div", { class: "choice-grid", role: "radiogroup" }, o.values.map((v) =>
				card(S.buildOptions?.[o.name] === v.id, () => { S.buildOptions = { ...S.buildOptions, [o.name]: v.id }; draw(); },
					h("strong", {}, v.title), v.text ? h("span", { class: "small muted" }, v.text) : null)))));
		const cards = S.data.variants.map((v) => card(match === v.id, () => { sel.useVariant(v.id); S.customPicked = false; draw(); },
			h("strong", {}, vtitle(v)),
			h("span", { class: "small" }, v.summary),
			h("span", { class: "small muted" }, t("flash.build.count", { n: S.radio.config[v.id].length, total: supportedCount }))));
		cards.push(card(!match, () => {
			S.customPicked = true;
			for (const g of S.data.groups) S.openGroups.add(g);
			draw();
		},
		h("strong", {}, t("flash.build.custom")),
		h("span", { class: "small" }, t("flash.build.custom_card_text")),
		h("span", { class: "small muted" }, t("flash.build.count", { n: sel.on.size, total: supportedCount }))));
		// every feature this radio supports, one collapsible group per feature-matrix group, each with its count
		const groups = S.data.groups.map((g) => {
			const items = S.data.features.filter((f) => f.group === g && sel.supported(f.name));
			if (!items.length) return null;
			const on = items.filter((f) => sel.on.has(f.name)).length;
			const base = S.radio.config[sel.base];
			const changed = items.filter((f) => base.includes(f.name) !== sel.on.has(f.name)).length;
			const details = h("details", { class: "feature-group", open: S.openGroups.has(g),
				ontoggle: (e) => { if (e.target.open) S.openGroups.add(g); else S.openGroups.delete(g); } },
			h("summary", {}, h("span", { class: "group-name" }, gtitle(g)),
				h("span", { class: "group-count" }, t("flash.build.group_count", { on, total: items.length }),
					changed ? h("span", { class: "muted" }, " · ", t("flash.build.group_changed", { n: changed })) : null)),
			nest(items).map(({ f, depth }) => {
				const isOn = sel.on.has(f.name);
				const off = S.data.variants.find((v) => v.id === sel.base)?.off.find((o) => o.name === f.name);
				// a needed feature outside this group that is off: turning this on turns it on too
				const away = (f.needs?.all || []).filter((n) => !items.some((i) => i.name === n) && sel.supported(n) && !sel.on.has(n));
				// one line per feature; the help (written for developers) is the tooltip, the build's reason for leaving
				// a feature out and a needed feature that's off show inline
				const note = [off && !isOn ? off.why : null,
					away.length ? t("flash.build.needs", { names: away.map((n) => sel.byName[n]?.title || n).join(", ") }) : null].filter(Boolean);
				const tipId = `tip-${f.name}`;
				const tipOpen = S.openTip === f.name;
				return h("div", { class: `toggle${base.includes(f.name) !== isOn ? " changed" : ""}`, style: `--depth: ${depth}` },
					h("input", { type: "checkbox", id: `f-${f.name}`, checked: isOn, disabled: sel.required(f.name),
						onchange: (e) => change(f.name, e.target.checked) }),
					h("div", { class: "feature" },
						h("label", { for: `f-${f.name}` }, f.title),
						h("button", { class: "info", type: "button", "aria-expanded": String(tipOpen), "aria-controls": tipId,
							"aria-label": t("flash.build.info", { name: f.title }),
							onclick: (e) => { e.stopPropagation(); S.openTip = tipOpen ? null : f.name; draw(); focusInfo(f.name); } }, "i"),
						note.length ? h("span", { class: "why" }, note.join(" ")) : null,
						tipOpen ? featureTip(f, tipId) : null),
					h("code", { class: "small" }, f.name));
			}));
			return details;
		});
		const building = S.build && S.build.selection === selectionKey(sel);
		const d = sel.diff(match || sel.base);
		const baseTitle = vtitle(S.data.variants.find((v) => v.id === sel.base));
		const summary = h("div", {}, match ? h("b", {}, t("flash.build.prebuilt", { variant: vtitle(S.data.variants.find((v) => v.id === match)) }))
			: h("span", {}, h("b", {}, t("flash.build.custom")), " ",
				d.added.length || d.removed.length
					? t("flash.build.custom_diff", { added: d.added.length, removed: d.removed.length, variant: baseTitle })
					: t("flash.build.custom_start")));
		const busy = building && !S.build.error;
		const missing = buildOptions().find((o) => !S.buildOptions?.[o.name]);
		forward(button(t(busy ? "flash.nav.building" : missing ? "flash.nav.choose" : S.firmware || S.sim ? "flash.nav.to_now" : "flash.nav.build",
			{ what: missing?.title.toLowerCase() }), go, { primary: true, disabled: busy || !!missing }), summary);
		// every image, prebuilt choice or not, comes from the build service (or is the user's own)
		const image = panel(t("flash.build.image_title"),
			S.firmware ? notice(t("flash.build.image_ready", { what: S.firmware.source }), "ok")
				: building && S.build.error ? notice(S.build.error, "bad")
				: building ? h("p", { class: "muted" }, S.build.text)
				: h("p", {}, t(S.sim ? "flash.sim.image" : "flash.build.on_request")),
			h("details", { class: "small" }, h("summary", {}, t("flash.build.self_title")),
				h("p", {}, t("flash.build.self_text")), h("pre", {}, h("code", {}, sel.makeCommand(S.buildOptions))),
				filePicker(t("flash.build.own_image"), ".bin", (f) => loadOwnImage(f, draw)),
				filePicker(t("flash.build.own_manifest"), ".json", async (f) => { S.ownManifest = JSON.parse(new TextDecoder().decode(f.data)); })));
		wrap.replaceChildren(
			panel(t("flash.build.title"),
				S.radio.status !== "supported" ? notice(t("flash.build.untested", { name: S.radio.name }), "bad") : null,
				...options,
				h("p", {}, t("flash.build.lead")),
				h("div", { class: "choice-grid", role: "radiogroup" }, cards),
				h("h3", {}, t("flash.build.features_title")),
				h("div", { class: "feature-groups" }, groups)),
			image);
	};
	draw();
	// a redraw replaces the buttons: put the focus back on the one that was used
	const focusInfo = (name) => wrap.querySelector(`[aria-controls="tip-${name}"]`)?.focus();
	wrap.addEventListener("click", () => { if (S.openTip) { S.openTip = null; draw(); } });
	tipEscape = () => {
		if (!S.openTip || !wrap.isConnected) return;
		const name = S.openTip;
		S.openTip = null;
		draw();
		focusInfo(name);
	};
	return wrap;
}

// Escape closes the open ⓘ bubble (on the document: a redraw can leave the focus on the body)
let tipEscape = () => {};
document.addEventListener("keydown", (e) => { if (e.key === "Escape") tipEscape(); });

// The ⓘ bubble: what a feature does, what it needs, its flash cost on this radio
function featureTip(f, id) {
	const titles = (names) => names.map((n) => S.selection.byName[n]?.title || n).join(", ");
	const { all = [], any = [] } = f.needs || {};
	const needs = [all.length ? titles(all) : null, any.length ? t("flash.build.tip_any", { names: titles(any) }) : null].filter(Boolean);
	const bytes = f.bytes?.[S.radio.model];
	return h("div", { class: "tip", id, role: "note", onclick: (e) => e.stopPropagation() },
		h("p", {}, plainHelp(f.help)),
		f.notes ? h("p", { class: "muted" }, f.notes) : null,
		h("dl", { class: "kv-list small" },
			needs.length ? [h("dt", {}, t("flash.build.tip_needs")), h("dd", {}, needs.join("; "))] : null,
			bytes ? [h("dt", {}, t("flash.build.tip_size")), h("dd", {}, t("flash.build.tip_bytes", { n: bytes }))] : null,
			h("dt", {}, t("flash.build.tip_switch")), h("dd", {}, h("code", {}, `CONFIG_${f.name}`))));
}

// features.mk help without its source and spec references ("(needs DMR; core/net/http.c)")
function plainHelp(text) {
	const t = text.replace(/\s*\([^()]*(?:\/|\.md|needs )[^()]*\)/g, "").trim();
	return t.charAt(0).toUpperCase() + t.slice(1);
}

// A group's features as a tree: each sits under the feature it needs in the same group (the deepest one, when it
// needs several), in feature-matrix order. [{f, depth}]
function nest(items) {
	const byName = Object.fromEntries(items.map((f) => [f.name, f]));
	const depthMemo = {};
	const depth = (name, seen = new Set()) => {
		if (name in depthMemo) return depthMemo[name];
		if (seen.has(name) || !byName[name]) return 0;
		seen.add(name);
		const d = Math.max(-1, ...(byName[name].needs?.all || []).filter((n) => byName[n]).map((n) => depth(n, seen))) + 1;
		return (depthMemo[name] = d);
	};
	const parent = {};
	for (const f of items) {
		let best = null;
		for (const n of f.needs?.all || []) if (byName[n] && (best === null || depth(n) > depth(best))) best = n;
		parent[f.name] = best;
	}
	const out = [];
	const walk = (p, d) => {
		for (const f of items) if (parent[f.name] === p) { out.push({ f, depth: d }); walk(f.name, d + 1); }
	};
	walk(null, 0);
	return out;
}

const selectionKey = (sel) => `${sel.radio.id}:${JSON.stringify(S.buildOptions || {})}:${[...sel.on].sort().join(",")}`;

// Ask the build API for the current selection, wait for it, and take the image like a release's
async function requestBuild(redraw) {
	const sel = S.selection, key = selectionKey(sel);
	const current = () => S.build?.selection === key && selectionKey(S.selection) === key;
	S.build = { selection: key, text: t("flash.build.sending") };
	redraw();
	try {
		const rels = S.data.releases.filter((x) => x.board === S.radio.id);
		let b = await api("POST", "builds", { board: S.radio.id, ref: rels.length ? rels[rels.length - 1].version : "main", features: [...sel.on].sort(),
			options: S.buildOptions || {}, tool: "web", tool_version: S.data.site.version || "", task: "install", id: useId() });
		while (b.status === "queued" || b.status === "running") {
			if (!current()) return;
			S.build.text = b.status === "queued" && b.position ? t("flash.build.queued", { n: b.position }) : t("flash.build.building");
			redraw();
			await new Promise((ok) => setTimeout(ok, 3000));
			b = await api("GET", `builds/${b.id}`);
		}
		if (b.status !== "done") throw new Error(t("flash.build.failed", { error: b.error || b.status }));
		const [m, img] = await Promise.all([fetch(new URL(b.manifest, API)).then((r) => r.json()),
			fetch(new URL(b.image, API)).then((r) => r.arrayBuffer()).then((x) => new Uint8Array(x))]);
		await verify(m, img, fb(), S.board.id);
		for (const [name, value] of Object.entries(S.buildOptions || {}))
			if (m.build_options?.[name] !== value) throw new Error(t("flash.build.wrong_options", { name, value }));
		if (!current()) return;
		// variant: the prebuilt choice this is, for the flash log (the service names every image it builds "custom")
		S.firmware = { manifest: m, image: img, source: t("flash.build.built", { version: m.version }), origin: "custom",
			variant: sel.matches() || "custom", build: b.id, selection: key };
		S.build = null;
	} catch (e) {
		if (S.build?.selection === key) S.build.error = t("flash.build.unavailable", { error: errorText(e) });
	}
	redraw();
}

async function loadOwnImage(f, redraw) {
	try {
		if (startsWith(f.data, fb().magic)) throw new Error(t("flash.err.stock_as_image"));
		const m = S.ownManifest || await forDevBuild(S.board, f.data);
		const board = await verify(m, f.data, fb(), S.board.id);
		S.firmware = { manifest: m, image: f.data, source: f.name, origin: "own", board };
	} catch (e) {
		alert(errorText(e));
	}
	redraw();
}

async function ensureFirmware() {
	if (S.firmware) return S.firmware;
	if (S.sim) {
		const img = randomBytes(rng(99), 200 * 1024);
		return (S.firmware = { manifest: await forDevBuild(S.board, img), image: img, source: t("flash.sim.image_name") });
	}
	throw new Error(t("flash.build.no_image"));
}

function slug(text) {
	return text.toLowerCase().replace(/[^\w\- ]/g, "").replace(/ /g, "-");
}

// ---------------------------------------------------------------------------------------------------------------
// install: what runs on the radio now

function nowView() {
	const out = h("div", { class: "flasher" });
	const forward = S.forward;
	const draw = () => {
		const opts = h("div", { class: "choice-grid" },
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "odr"), onclick: checkOdr },
				h("strong", {}, t("flash.now.odr")), h("span", { class: "small muted" }, t("flash.now.odr_text"))),
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "stock"), onclick: () => { S.now = { kind: "stock", version: null }; draw(); } },
				h("strong", {}, t("flash.now.stock")), h("span", { class: "small muted" }, t("flash.now.stock_text"))),
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "unknown"), onclick: () => { S.now = { kind: "unknown" }; draw(); } },
				h("strong", {}, t("flash.now.unknown")), h("span", { class: "small muted" }, t("flash.now.unknown_text"))));
		const detail = h("div");
		if (S.now?.kind === "odr" && S.now.probe) {
			const p = S.now.probe;
			detail.append(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.board")), h("dd", {}, p.board),
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, p.codec || "?"),
				p.chip ? [h("dt", {}, t("flash.now.uid")), h("dd", {}, p.chip.uid)] : null),
			S.backupDone ? notice(t("flash.backup.done_before"), "ok") : backupNow({ when: "before", onDone: draw }));
		} else if (S.now?.kind === "stock") {
			const versions = S.radio.flash.stock_versions;
			detail.append(h("label", {}, t("flash.now.which_version"), " ",
				h("select", { onchange: (e) => { S.now.version = e.target.value || null; } },
					h("option", { value: "" }, t("flash.now.version_unknown")),
					versions.map((v) => h("option", { value: v, selected: S.now.version === v }, v)))),
			notice(t(S.radio.backup.stock === "none" ? "flash.now.stock_backup_later" : "flash.now.stock_backup_planned"), "info"));
		}
		out.replaceChildren(panel(t("flash.now.title"), opts, detail, status));
		// on ODR the backup comes first; skipping it is a choice the button says out loud
		const skip = S.now?.kind === "odr" && !S.backupDone;
		forward(button(t(skip ? "flash.nav.skip_backup" : "flash.nav.to_codec"), next, { primary: !skip, disabled: !S.now }));
	};
	const status = h("div");
	async function checkOdr() {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.err.no_answer"));
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			const odd = optionMismatch(p);
			if (odd) throw new Error(t("flash.err.wrong_option", { board: p.board, option: odd[0].title.toLowerCase(), value: odd[1].title }));
			S.now = { kind: "odr", probe: p };
			await keyFromRadio(link);
			status.replaceChildren();
		} catch (e) {
			await dropSerial();
			status.replaceChildren(connectError(e), serialHelp());
			return;
		}
		draw();
	}
	draw();
	return out;
}

// The backup panel used on an ODR radio (before an update, the backup task, after a first install)
function backupNow({ when = "task", onDone = null } = {}) {
	const box = h("div", { class: "panel" });
	const prog = progressBar();
	const internalSize = S.board.flash_end - S.board.app_base;
	// what each choice reads, in bytes, and what readRadio is told
	const choices = {
		all: { bytes: internalSize + S.radio.backup.spi_size, what: { internal: true, spi: "full" } },
		firmware: { bytes: internalSize, what: { internal: true, spi: "none" } },
		codeplug: { bytes: CODEPLUG_SPAN, what: { internal: false, spi: "codeplug" } },
	};
	let pick = "all";
	const times = {};
	const run = async () => {
		box.querySelectorAll("button, input").forEach((b) => { b.disabled = true; });
		box.append(prog.el);
		let started = null;
		try {
			const link = await connectSerial();
			const p = S.now?.probe || await probe(link);
			// the backup task is a use of its own; the backups around an install belong to the install
			if (S.task === "backup") started = flashStarted({ task: "backup", source: pick, radio: S.board.id, firmware: p.git || "", features: p.features || "" });
			const t0 = performance.now();
			S.backupParts = await readRadio(link, S.board, S.radio, {
				what: choices[pick].what, progress: (part, d, total) => prog.set(d, total, t(`flash.backup.part.${part}`)) });
			S.readRate = choices[pick].bytes / Math.max(0.5, (performance.now() - t0) / 1000);
			if (S.stock) S.backupParts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: S.stock.data, complete: true });
			const after = when === "after";
			const meta = {
				radio: { board: S.radio.id, name: S.radio.name, chip: p.chip, version: p.version },
				firmware_now: { kind: "odr", git: p.git, codec: p.codec, features: p.features },
				firmware_before: after && S.now?.kind === "stock" ? { kind: "stock", version: S.now.version, stock_file_sha256: S.stock?.info.sha || null } : undefined,
				contents: pick,
				notes: after && S.now?.kind !== "odr" ? [t("flash.backup.note_after")] : [],
			};
			const bytes = await makeBundle(meta, S.backupParts, { git: S.data.site.git, url: location.origin });
			download(bundleName(meta), bytes, "application/zip");
			S.backupDone = true;
			flashFinished(started);
			const incomplete = S.backupParts.filter((x) => !x.complete && x.what !== "spi-flash").map((x) => x.file);
			box.append(notice(t("flash.backup.saved", { size: Math.round(bytes.length / 1024) }), "ok"));
			if (incomplete.length) box.append(notice(t("flash.backup.incomplete", { files: incomplete.join(", ") })));
			onDone?.();
		} catch (e) {
			flashFinished(started, e);
			box.append(notice(errorText(e), "bad"));
			box.querySelectorAll("button, input").forEach((b) => { b.disabled = false; });
		}
	};
	const option = (id) => h("label", { class: "toggle" },
		h("input", { type: "radio", name: "backup-what", checked: id === pick, onchange: () => { pick = id; } }),
		h("span", {}, h("b", {}, t(`flash.backup.opt.${id}`)),
			h("span", { class: "why" }, t(`flash.backup.opt.${id}_text`), " ", times[id] = h("span", {}, estimate(choices[id].bytes)))),
		h("span"));
	box.append(h("h3", {}, t(`flash.backup.title_${when}`)),
		h("p", {}, t(when === "after" && S.now?.kind !== "odr" ? "flash.backup.after_lead" : "flash.backup.lead")),
		option("all"), option("firmware"), option("codeplug"),
		h("div", { class: "actions" }, button(t("flash.backup.run"), run, { primary: true })));
	// the estimates come from this radio: a short timed read, unless a backup already measured it
	if (!S.readRate) {
		measureRate().then(() => { for (const id of Object.keys(choices)) times[id]?.replaceChildren(estimate(choices[id].bytes)); });
	}
	return box;
}

// How fast this radio's memory reads over its serial port: 64 KB from the memory chip, timed
async function measureRate() {
	try {
		const link = await connectSerial();
		const t0 = performance.now();
		const r = await link.cpsDump(1, 0, 0x10000);
		if (r.complete) S.readRate = 0x10000 / Math.max(0.01, (performance.now() - t0) / 1000);
	} catch (e) { /* no estimate */ }
}

function estimate(bytes) {
	const mb = bytes / 1048576;
	const size = mb >= 1 ? `${mb.toFixed(mb >= 10 ? 0 : 1)} MB` : `${Math.round(bytes / 1024)} KB`;
	if (!S.readRate) return t("flash.backup.time_unknown", { size });
	const s = bytes / S.readRate;
	return s < 60 ? t("flash.backup.time_lt1", { size }) : t("flash.backup.time_min", { size, n: Math.round(s / 60) });
}

// ---------------------------------------------------------------------------------------------------------------
// install: codec and key

function codecView() {
	const out = h("div", { class: "flasher" });
	const forward = S.forward;
	const draw = async () => {
		let fw;
		try {
			fw = await ensureFirmware();
		} catch (e) {
			out.replaceChildren(panel(t("flash.codec.title"), notice(errorText(e), "bad")));
			forward(null);
			return;
		}
		const haveStock = !!(S.stock && fb().profiles[S.stock.info.sha]);
		const keyKnown = S.sim || !!S.key || !!savedKey(S.board);
		let err = null;
		try {
			S.decision = decideCodec(S.board, fw.manifest, {
				haveStock, requested: S.requested || null,
				probe: S.now?.kind === "odr" ? S.now.probe : null,
				stockInstalled: S.now?.kind === "stock" ? S.now.version : null,
			});
		} catch (e) {
			S.decision = null;
			err = e.reason || e.message;
		}
		// The stock file is needed when the codec has to come from it, or the bootloader key can't come from anywhere
		// else. Whatever the case, the page says what happens and what the user has to do.
		const needs = [];
		if (!S.decision && !haveStock) needs.push(`${err} ${t("flash.codec.need_codec")}`);
		if (S.decision && !keyKnown && !haveStock) needs.push(t("flash.codec.need_key"));
		const ready = S.decision && (keyKnown || haveStock);
		const plan = S.decision ? `${t(`flash.codec.mode.${S.decision.mode}`)}: ${S.decision.why}.` : null;
		const keyLine = S.keyFromRadio ? t("flash.codec.key_from_radio") : keyKnown && !S.sim ? t("flash.codec.key_saved") : null;
		const picker = S.sim ? button(t("flash.sim.use_stock"), async () => { await useStock({ name: "simulated-stock.bin", data: S.sim.stock }); })
			: filePicker(t("flash.codec.stock_pick"), ".bin", useStock);
		const loadedText = S.stock && t("flash.codec.stock_loaded", { name: S.stock.name, what: S.stock.info.known || t("flash.codec.stock_unknown") });
		const loaded = !S.stock ? null : S.stock.info.known ? h("p", {}, "✓ ", loadedText) : notice(loadedText, "bad");
		const where = h("p", {}, t("flash.codec.stock_files", { files: S.board.codec.sources.join(", ") }), " ",
			S.radio.flash.stock_download ? h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.codec.stock_download"))
				: t("flash.codec.stock_maker"));
		const parts = [h("p", {}, t("flash.codec.lead"))];
		if (needs.length) {
			parts.push(h("div", { class: "notice action" }, h("strong", {}, t("flash.codec.action_title")),
				h("ul", {}, needs.map((n) => h("li", {}, n))), where, picker, loaded));
			if (!S.decision && S.now?.kind !== "odr") parts.push(h("p", { class: "small muted" }, t("flash.codec.or_connect")));
		} else if (!S.decision) {
			parts.push(notice(err, "bad"));
		} else {
			parts.push(h("div", { class: "notice ok" }, h("strong", {}, t(haveStock && S.stock ? "flash.codec.ready_title" : "flash.codec.none_title")),
				h("p", {}, plan), keyLine ? h("p", {}, keyLine) : null, loaded));
			if (S.decision.risk) parts.push(h("p", { class: "small muted" }, S.decision.risk[0].toUpperCase() + S.decision.risk.slice(1) + "."));
		}
		// writing the codec from the stock file anyway, or forcing a mode: always there, never in the way
		parts.push(h("details", { class: "small" }, h("summary", {}, t("flash.codec.advanced")),
			needs.length ? null : h("div", {}, h("p", {}, t("flash.codec.anyway_text")), where, picker),
			h("label", {}, t("flash.codec.force"), " ", h("select", { onchange: (e) => { S.requested = e.target.value || null; draw(); } },
				h("option", { value: "" }, t("flash.codec.force_auto")),
				h("option", { value: KEEP, selected: S.requested === KEEP }, t("flash.codec.mode.keep")),
				h("option", { value: MERGE, selected: S.requested === MERGE }, t("flash.codec.mode.merge"))))));
		parts.push(h("p", { class: "small muted" }, t("flash.codec.image", { what: fw.source, size: fw.image.length })));
		out.replaceChildren(panel(t("flash.codec.title"), ...parts));
		forward(button(t("flash.nav.to_write"), next, { primary: true, disabled: !ready }));
	};
	async function useStock(f) {
		S.stock = { ...f, info: await stockfw.identify(f.data, fb()) };
		if (!S.keyFromRadio) S.key = null;
		draw();
	}
	out.append(h("p", { class: "muted" }, t("flash.loading")));
	draw();
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// writing through the bootloader (install and restore)

function planTable(plan) {
	return h("div", { class: "table-wrap" }, h("table", { class: "plan" },
		h("thead", {}, h("tr", {}, h("th", {}, t("flash.plan.sector")), h("th", {}, t("flash.plan.address")), h("th", {}, t("flash.plan.size")), h("th", {}, t("flash.plan.action")))),
		h("tbody", {}, plan.steps.map((st) => h("tr", {},
			h("td", {}, st.sector.index), h("td", {}, `${hex(st.sector.addr)}`), h("td", {}, `${st.sector.size / 1024} KB`),
			h("td", { class: st.action === "write" || st.action === "erase" ? "write" : st.action === "keep" ? "keep" : "" },
				st.action === "write" ? t("flash.plan.write", { content: st.content, n: st.blocks.length })
					: st.action === "erase" ? t("flash.plan.erase") : st.action === "keep" ? t("flash.plan.keep") : t("flash.plan.skip")))))));
}

// The bootloader panel: instructions, connect, write. makePlanFn returns the plan once the key is known.
// what: the flash log's description of this write (task, source, radio, firmware, variant, features, build)
function bootloaderPanel(title, makePlanFn, onDone, what) {
	const box = h("section", { class: "panel" });
	const log = logBox();
	const prog = progressBar();
	const status = h("div");
	let plan = null, transport = null;
	const draw = async () => {
		try {
			const key = await getKey(S.board);
			plan = await makePlanFn(key);
		} catch (e) {
			box.replaceChildren(h("h2", {}, title), notice(errorText(e), "bad"));
			return;
		}
		const fp = h("div");
		const connect = button(t("flash.write.connect"), async () => {
			try {
				transport = await connectBootloader();
				const f = await transport.fingerprint?.();
				if (f) {
					log.add(`bootloader: ${f.string1 || "?"} / ${f.product || "?"} / serial ${f.serial || "?"} / ${f.interfaces?.[0] || ""}`);
					if (S.board.dfu_string1 && f.string1 && f.string1 !== S.board.dfu_string1) fp.append(notice(t("flash.write.unexpected", { what: f.string1 })));
				}
				status.replaceChildren(notice(t("flash.write.found"), "ok"));
				if (onWindows() && !S.fakeRadio) try { localStorage.setItem(WINUSB_STORE, "1"); } catch (e) { /* not remembered */ }
				winHelp?.replaceChildren();
				write.disabled = false;
				connect.disabled = true;
			} catch (e) {
				status.replaceChildren(...h("div", {}, notice(errorText(e), "bad"),
					onWindows() && usbSupported() ? windowsDriverHelp({ failed: true }) : null).childNodes);
				winHelp?.replaceChildren();
			}
		});
		const winHelp = onWindows() ? h("div", {}, windowsDriverHelp()) : null;
		const write = button(t("flash.write.go"), async () => {
			write.disabled = true;
			box.append(prog.el, log.el);
			let started = null;
			try {
				await dropSerial();
				started = flashStarted(what);
				await new Dfu(transport, { log: log.add }).run(plan, S.key, (d, total) => prog.set(d, total, t("flash.write.writing")));
				flashFinished(started);
				log.add(t("flash.write.finished"));
				await transport.close?.();
				onDone();
			} catch (e) {
				flashFinished(started, e);
				status.replaceChildren(notice(t("flash.write.failed", { error: errorText(e) }), "bad"));
				write.disabled = false;
			}
		}, { primary: true, disabled: true });
		// through h(): the DOM's own replaceChildren writes a null as the text "null"
		box.replaceChildren(...h("div", {}, h("h2", {}, title),
			h("details", { class: "small" }, h("summary", {}, t("flash.write.plan", { erase: plan.erase.length, blocks: plan.totalBlocks })), planTable(plan),
				h("pre", { class: "log" }, plan.describe().join("\n"))),
			h("h3", {}, t("flash.write.bootloader_title")),
			h("ol", { class: "steps" }, S.radio.flash.bootloader.map((s) => h("li", {}, s))),
			S.fakeRadio ? button(t("flash.sim.enter_bootloader"), () => { S.fakeRadio.enterBootloader(); log.add("(simulated) radio in its bootloader"); }) : null,
			winHelp,
			h("div", { class: "actions" }, connect, write), fp, status).childNodes);
	};
	box.append(h("p", { class: "muted" }, t("flash.loading")));
	draw();
	return box;
}

function writeView() {
	const fw = S.firmware;
	return bootloaderPanel(t("flash.write.title"), async () => {
		let codec = [];
		if (S.decision.mode === MERGE) codec = (await stockfw.extract(S.stock.data, S.key, fb())).segments;
		return makePlan(S.board, segments(fw.manifest, fw.image), S.decision.mode, codec);
	}, next, { task: "install", source: fw.origin || "", radio: S.board.id, firmware: String(fw.manifest.version || ""),
		variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null });
}

// ---------------------------------------------------------------------------------------------------------------
// install: after the write

function doneView() {
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const check = button(t("flash.done.check"), async () => {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		await dropSerial();
		try {
			const p = await probe(await connectSerial());
			if (!p) throw new Error(t("flash.err.no_answer"));
			const good = !S.decision.expectCodec || p.codec === S.board.codec.id;
			status.replaceChildren(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, p.codec)),
			notice(good ? t("flash.done.ready") : t("flash.done.no_codec"), good ? "ok" : "bad"));
			if (!S.backupDone) {
				S.now = { ...S.now, probe: S.now?.kind === "odr" ? p : S.now?.probe, after: p };
				status.append(backupNow({ when: "after" }));
			}
		} catch (e) {
			await dropSerial();
			status.replaceChildren(connectError(e));
		}
	}, { primary: true });
	box.append(panel(t("flash.done.title"), h("p", {}, t("flash.done.lead")), h("div", { class: "actions" }, check), serialHelp(), status));
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// backup task

function backupView() {
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const connect = button(t("flash.backup.connect"), async () => {
		try {
			const p = await probe(await connectSerial());
			if (!p) throw new Error(t("flash.err.no_answer"));
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			S.now = { kind: "odr", probe: p };
			status.replaceChildren(h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, p.codec || "?")),
			h("details", { class: "small" }, h("summary", {}, t("flash.backup.add_stock")),
				filePicker(t("flash.codec.stock_pick"), ".bin", async (f) => { S.stock = { ...f, info: await stockfw.identify(f.data, fb()) }; })),
			backupNow({ when: "task" }));
		} catch (e) {
			await dropSerial();
			status.replaceChildren(connectError(e));
		}
	}, { primary: true });
	box.append(panel(t("flash.backup.task_title", { name: S.radio.name }), h("p", {}, t("flash.backup.task_lead")),
		S.radio.backup.stock === "none" ? notice(t("flash.backup.stock_none"), "info") : null,
		h("div", { class: "actions" }, connect), serialHelp(), status));
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// restore task

function bundleView() {
	const status = h("div");
	return panel(t("flash.restore.title"), h("p", {}, t("flash.restore.lead")),
		filePicker(t("flash.restore.pick"), ".zip", async (f) => {
			try {
				S.bundle = await openBundle(f.data);
				const r = S.data.radios.find((x) => x.id === S.bundle.meta.radio.board);
				if (!r) throw new Error(t("flash.err.unknown_board", { board: S.bundle.meta.radio.board }));
				selectRadio(r);
				const st = S.bundle.parts.get("stock-firmware");
				if (st) S.stock = { name: "stock-firmware.bin", data: st.data, info: await stockfw.identify(st.data, fb()) };
				next();
			} catch (e) {
				status.replaceChildren(notice(errorText(e), "bad"));
			}
		}), status);
}

function restoreView() {
	const b = S.bundle, meta = b.meta;
	const box = h("div", { class: "flasher" });
	const avail = targets(b);
	if (!S.restoreTarget) S.restoreTarget = avail[0] || null;
	const summary = h("dl", { class: "kv-list" },
		h("dt", {}, t("flash.now.board")), h("dd", {}, meta.radio.name),
		h("dt", {}, t("flash.restore.taken")), h("dd", {}, meta.created),
		meta.radio.chip ? [h("dt", {}, t("flash.now.uid")), h("dd", {}, meta.radio.chip.uid)] : null,
		h("dt", {}, t("flash.restore.fw_now")), h("dd", {}, describeFirmware(meta.firmware_now)),
		meta.firmware_before ? [h("dt", {}, t("flash.restore.fw_before")), h("dd", {}, describeFirmware(meta.firmware_before))] : null,
		h("dt", {}, t("flash.restore.parts")), h("dd", {}, [...b.parts.values()].map((p) => `${p.file}${p.complete ? "" : " (incomplete)"}`).join(", ")));
	const choices = h("div", { class: "choice-grid" }, ["odr", "codeplug", "stock"].map((tg) =>
		h("button", { class: "choice", type: "button", disabled: !avail.includes(tg), "aria-pressed": String(S.restoreTarget === tg),
			onclick: () => { S.restoreTarget = tg; render(); } },
		h("strong", {}, t(`flash.restore.to_${tg}`)), h("span", { class: "small muted" }, t(`flash.restore.to_${tg}_text`)))));
	const parts = [panel(t("flash.restore.what"), summary, choices)];
	// the stock file: the application to write back, or (any target) where the bootloader key comes from
	if (S.restoreTarget === "codeplug") parts.push(afterRestore());
	else if (S.restoreTarget && !S.stock && (S.restoreTarget === "stock" || (!S.sim && !S.key && !savedKey(S.board))))
		parts.push(panel(t("flash.codec.stock_title"), h("p", {}, t("flash.restore.stock_needed")),
			filePicker(t("flash.codec.stock_pick"), ".bin", async (f) => { S.stock = { ...f, info: await stockfw.identify(f.data, fb()) }; render(); })));
	else if (S.restoreTarget) {
		parts.push(bootloaderPanel(t("flash.restore.write_title"), async (key) =>
			(S.restoreTarget === "odr" ? planInternal(b, S.board) : planStock(S.stock.data, key, S.board, fb())), () => {
			if (S.fakeRadio && S.restoreTarget === "stock") S.fakeRadio.running = "stock";
			box.append(afterRestore());
		}, { task: "restore", source: S.restoreTarget, radio: S.board.id,
			firmware: S.restoreTarget === "stock" ? "stock" : describeFirmware(meta.firmware_now) }));
	}
	box.append(...parts);
	return box;
}

function afterRestore() {
	if (S.restoreTarget === "stock") {
		return panel(t("flash.restore.done_stock_title"), h("p", {}, t("flash.restore.done_stock")),
			notice(t("flash.restore.spi_note"), "info"));
	}
	const status = h("div");
	const prog = progressBar();
	const go = button(t("flash.restore.codeplug_go"), async () => {
		go.disabled = true;
		status.replaceChildren(prog.el);
		try {
			await dropSerial();
			const link = await connectSerial();
			const spi = S.bundle.parts.get("spi-flash");
			const now = await link.cpsDump(1, 0, Math.min(spi.data.length, 0x120000), { progress: (d, n) => prog.set(d, n, t("flash.restore.comparing")) });
			const sectors = codeplugSectors(spi.data, now.data);
			const refused = await writeCodeplug(link, sectors, (d, n) => prog.set(d, n, t("flash.restore.writing_codeplug")));
			status.append(notice(refused.length ? t("flash.restore.codeplug_refused", { n: refused.length, total: sectors.length })
				: t("flash.restore.codeplug_done", { n: sectors.length }), refused.length ? "" : "ok"));
		} catch (e) {
			status.append(connectError(e));
			go.disabled = false;
		}
	}, { primary: true, disabled: !S.bundle.parts.get("spi-flash") });
	return panel(t("flash.restore.codeplug_title"), h("p", {}, t("flash.restore.codeplug_lead")), go, serialHelp(), status);
}

// ---------------------------------------------------------------------------------------------------------------

async function main() {
	if (!FLASH_LOG) document.getElementById("flash-log-off")?.replaceWith(t("flash.log.is_off"));
	// the privacy contact is put together here, so address scrapers don't find it in the page or the repository
	const mail = ["tseuqer-ycavirp-rdo", "moc.relwofrecneps"].map((x) => [...x].reverse().join("")).join("@");
	const a = document.getElementById("privacy-mail");
	if (a) { a.href = `mailto:${mail}`; a.textContent = mail; }
	try {
		S.data = await loadData();
	} catch (e) {
		app.replaceChildren(notice(t("flash.err.load"), "bad"));
		return;
	}
	if (location.hash.includes("sim")) {
		S.sim = await simulation(S.data.fb, "dm1701");
		S.fakeRadio = new FakeRadio(S.sim, { state: location.hash.includes("sim=odr") ? "odr" : "stock", spiSize: 0x400000 });
		// a smaller SPI flash keeps the simulated backup quick
		for (const r of S.data.radios) if (r.id === "dm1701") r.backup = { ...r.backup, spi_size: 0x400000 };
	}
	S.backupParts = [];
	const m = location.hash.match(/radio=(\w+)/);
	const r = m && S.data.radios.find((x) => x.id === m[1]);
	if (r && flashable(r)) {
		S.task = "install";
		selectRadio(r);
		S.step = 1;
	}
	render();
}

main();
