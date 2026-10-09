// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The web flasher: a step-by-step page over the modules beside it. Three tasks:
//
//   install   pick the radio and the build, find out what runs on the radio (backing it up if it runs ODR), decide
//             the codec, write through the bootloader, check the result and complete the backup
//   backup    read everything a radio running ODR can give into a bundle the user keeps
//   restore   put a radio back from a bundle: exactly as it was on ODR, or back on stock firmware
//
// Everything runs in the page: firmware images come from this site, stock files and backups from the user's disk,
// and nothing is sent anywhere. Add #sim to the address for a simulated radio (no hardware; for trying the flow).

import { hex, sha256, toHex, fromHex, startsWith } from "./bytes.js";
import * as stockfw from "./stockfw.js";
import { makePlan, KEEP, MERGE } from "./plan.js";
import { verify, segments, forDevBuild } from "./manifest.js";
import { Dfu, WebUsbTransport, checkKey, usbSupported } from "./dfu.js";
import { Link, SerialStream, ODR_USB, ODR_NET, probe, serialSupported } from "./link.js";
import { readRadio, makeBundle, openBundle, bundleName, describeFirmware } from "./backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug } from "./restore.js";
import { Selection } from "./select.js";
import { decideCodec } from "./decide.js";
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

const S = {
	task: null, step: 0, data: null, radio: null, board: null, selection: null, sim: null, fakeRadio: null,
	firmware: null,		// {manifest, image, source}
	now: null,		// what runs on the radio: {kind: "odr", probe} | {kind: "stock", version} | {kind: "unknown"}
	stock: null,		// {name, data, info: identify()}
	key: null,
	decision: null,
	backupParts: [], backupDone: false,
	bundle: null, restoreTarget: null,
	link: null,
};

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
const flashable = (r) => r.flash.backend === "anyroad-dfu" && S.data.fb.boards[r.flash.board]?.releasable;

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

function savedKey(board) {
	try {
		const k = localStorage.getItem(KEY_STORE(board.id));
		return k ? fromHex(k) : null;
	} catch (e) {
		return null;
	}
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
	const view = { radio: radioView, build: buildView, now: nowView, codec: codecView, write: writeView, done: doneView,
		backup: backupView, bundle: bundleView, restore: restoreView }[steps[S.step]];
	app.append(view());
	app.append(h("div", { class: "actions" },
		S.step > 0 ? button("← " + t("flash.back"), () => { S.step--; render(); }) : null,
		button(t("flash.start_over"), () => reset())));
}

function next() {
	S.step++;
	render();
	app.scrollIntoView({ behavior: "smooth", block: "start" });
}

async function reset() {
	await dropSerial();
	const keep = { data: S.data, sim: S.sim, fakeRadio: S.fakeRadio };
	for (const k of Object.keys(S)) S[k] = null;
	Object.assign(S, keep, { step: 0, backupParts: [], backupDone: false });
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
			S.now = { kind: "odr", probe: p };
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
}

// ---------------------------------------------------------------------------------------------------------------
// install: the build (variant, a la carte)

function releaseFor(variant) {
	const list = S.data.releases.filter((x) => x.board === S.radio.id && x.variant === variant);
	return list[list.length - 1] || null;
}

function buildView() {
	const sel = S.selection;
	const wrap = h("div", { class: "flasher" });
	const draw = () => {
		const match = sel.matches();
		const variants = h("div", { class: "choice-grid", role: "radiogroup" }, S.data.variants.map((v) => {
			const rel = releaseFor(v.id);
			return h("button", { class: "choice", type: "button", role: "radio", "aria-checked": String(match === v.id),
				onclick: () => { sel.useVariant(v.id); draw(); } },
			h("strong", {}, t(`variant.${v.id}.title`) !== `variant.${v.id}.title` ? t(`variant.${v.id}.title`) : v.title),
			h("span", { class: "small" }, v.summary),
			h("span", { class: "small muted" }, t("flash.build.count", { n: S.radio.config[v.id].length })),
			h("span", { class: "small muted" }, rel ? t("flash.build.release", { version: rel.version }) : t("flash.build.no_release")));
		}));
		// a la carte: every switch this radio supports, grouped as on the features page
		const groups = S.data.groups.map((g) => {
			const items = S.data.features.filter((f) => f.group === g && sel.supported(f.name));
			if (!items.length) return null;
			return h("fieldset", {}, h("legend", {}, t(`group.${slug(g)}`) !== `group.${slug(g)}` ? t(`group.${slug(g)}`) : g), items.map((f) => {
				const base = S.radio.config[sel.base].includes(f.name);
				const on = sel.on.has(f.name);
				const off = S.data.variants.find((v) => v.id === sel.base)?.off.find((o) => o.name === f.name);
				return h("label", { class: `toggle${base !== on ? " changed" : ""}` },
					h("input", { type: "checkbox", checked: on, disabled: sel.required(f.name),
						onchange: (e) => { sel.set(f.name, e.target.checked); draw(); } }),
					h("span", {}, h("b", {}, f.title), h("span", { class: "why" }, off ? off.why : f.help)),
					h("code", { class: "small" }, f.name));
			}));
		});
		const d = sel.diff(match || sel.base);
		const summary = h("div", { class: "summary-bar" },
			h("div", {}, match ? h("b", {}, t("flash.build.prebuilt", { variant: S.data.variants.find((v) => v.id === match).title }))
				: h("span", {}, h("b", {}, t("flash.build.custom")), " ", t("flash.build.custom_diff", { added: d.added.length, removed: d.removed.length }))),
			button(t("flash.next"), () => { next(); }, { primary: true, disabled: !match && !S.firmware }));
		const custom = match ? null : h("div", {},
			notice(t("flash.build.custom_text"), "info"), h("pre", {}, h("code", {}, sel.makeCommand())),
			filePicker(t("flash.build.own_image"), ".bin", (f) => loadOwnImage(f, draw)));
		const image = firmwareChooser(match, draw);
		wrap.replaceChildren(
			panel(t("flash.build.title"), h("p", {}, t("flash.build.lead")), variants,
				h("details", { class: "alacarte-wrap", open: !match }, h("summary", {}, t("flash.build.customize")),
					h("div", { class: "alacarte" }, groups))),
			custom || image, summary);
	};
	draw();
	return wrap;
}

function firmwareChooser(match, redraw) {
	const rel = releaseFor(match);
	const status = h("div");
	if (S.firmware) status.append(notice(t("flash.build.image_ready", { what: S.firmware.source }), "ok"));
	else if (rel) status.append(h("p", { class: "small" }, t("flash.build.will_download", { version: rel.version, size: Math.round(rel.size / 1024) })));
	else if (S.sim) status.append(h("p", { class: "small" }, t("flash.sim.image")));
	else status.append(notice(t("flash.build.no_release_text"), "info"));
	return panel(t("flash.build.image_title"), status,
		h("details", { class: "small" }, h("summary", {}, t("flash.build.own_title")),
			h("p", {}, t("flash.build.own_text")),
			filePicker(t("flash.build.own_image"), ".bin", (f) => loadOwnImage(f, redraw)),
			filePicker(t("flash.build.own_manifest"), ".json", async (f) => { S.ownManifest = JSON.parse(new TextDecoder().decode(f.data)); })));
}

async function loadOwnImage(f, redraw) {
	try {
		if (startsWith(f.data, fb().magic)) throw new Error(t("flash.err.stock_as_image"));
		const m = S.ownManifest || await forDevBuild(S.board, f.data);
		const board = await verify(m, f.data, fb(), S.board.id);
		S.firmware = { manifest: m, image: f.data, source: f.name, board };
	} catch (e) {
		alert(errorText(e));
	}
	redraw();
}

async function ensureFirmware() {
	if (S.firmware) return S.firmware;
	const rel = releaseFor(S.selection.matches());
	if (rel) {
		const [m, img] = await Promise.all([fetch(ROOT + rel.manifest).then((r) => r.json()),
			fetch(ROOT + rel.image).then((r) => r.arrayBuffer()).then((b) => new Uint8Array(b))]);
		await verify(m, img, fb(), S.board.id);
		return (S.firmware = { manifest: m, image: img, source: `${rel.version} (${rel.variant})` });
	}
	if (S.sim) {
		const img = randomBytes(rng(99), 200 * 1024);
		return (S.firmware = { manifest: await forDevBuild(S.board, img), image: img, source: t("flash.sim.image_name") });
	}
	throw new Error(t("flash.build.no_release_text"));
}

function slug(text) {
	return text.toLowerCase().replace(/[^\w\- ]/g, "").replace(/ /g, "-");
}

// ---------------------------------------------------------------------------------------------------------------
// install: what runs on the radio now

function nowView() {
	const out = h("div", { class: "flasher" });
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
			backupNow());
		} else if (S.now?.kind === "stock") {
			const versions = S.radio.flash.stock_versions;
			detail.append(h("label", {}, t("flash.now.which_version"), " ",
				h("select", { onchange: (e) => { S.now.version = e.target.value || null; } },
					h("option", { value: "" }, t("flash.now.version_unknown")),
					versions.map((v) => h("option", { value: v, selected: S.now.version === v }, v)))),
			notice(t(S.radio.backup.stock === "none" ? "flash.now.stock_backup_later" : "flash.now.stock_backup_planned"), "info"));
		}
		out.replaceChildren(panel(t("flash.now.title"), opts, detail, status),
			h("div", { class: "actions" }, button(t("flash.next"), next, { primary: true, disabled: !S.now })));
	};
	const status = h("div");
	async function checkOdr() {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const p = await probe(await connectSerial());
			if (!p) throw new Error(t("flash.err.no_answer"));
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			S.now = { kind: "odr", probe: p };
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
function backupNow({ after = false, onDone = null } = {}) {
	const box = h("div", { class: "panel" });
	let what = "full";
	const prog = progressBar();
	const run = async () => {
		box.querySelectorAll("button").forEach((b) => { b.disabled = true; });
		box.append(prog.el);
		try {
			const link = await connectSerial();
			const p = S.now?.probe || await probe(link);
			S.backupParts = await readRadio(link, S.board, S.radio, {
				what: { spi: what }, progress: (part, d, total) => prog.set(d, total, t(`flash.backup.part.${part}`)) });
			if (S.stock) S.backupParts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: S.stock.data, complete: true });
			const meta = {
				radio: { board: S.radio.id, name: S.radio.name, chip: p.chip, version: p.version },
				firmware_now: { kind: "odr", git: p.git, codec: p.codec, features: p.features },
				firmware_before: after && S.now?.kind === "stock" ? { kind: "stock", version: S.now.version, stock_file_sha256: S.stock?.info.sha || null } : undefined,
				notes: after ? [t("flash.backup.note_after")] : [],
			};
			const bytes = await makeBundle(meta, S.backupParts, { git: S.data.site.git, url: location.origin });
			download(bundleName(meta), bytes, "application/zip");
			S.backupDone = true;
			const incomplete = S.backupParts.filter((x) => !x.complete).map((x) => x.file);
			box.append(notice(t("flash.backup.saved", { size: Math.round(bytes.length / 1024) }), "ok"),
				incomplete.length ? notice(t("flash.backup.incomplete", { files: incomplete.join(", ") })) : null);
			onDone?.();
		} catch (e) {
			box.append(notice(errorText(e), "bad"));
			box.querySelectorAll("button").forEach((b) => { b.disabled = false; });
		}
	};
	box.append(h("h3", {}, t(after ? "flash.backup.after_title" : "flash.backup.title")),
		h("p", {}, t(after ? "flash.backup.after_lead" : "flash.backup.lead")),
		h("label", { class: "toggle" }, h("input", { type: "radio", name: "spi", checked: true, onchange: () => { what = "full"; } }),
			h("span", {}, h("b", {}, t("flash.backup.full")), h("span", { class: "why" }, t("flash.backup.full_text"))), h("span")),
		h("label", { class: "toggle" }, h("input", { type: "radio", name: "spi", onchange: () => { what = "codeplug"; } }),
			h("span", {}, h("b", {}, t("flash.backup.quick")), h("span", { class: "why" }, t("flash.backup.quick_text"))), h("span")),
		h("div", { class: "actions" }, button(t("flash.backup.run"), run, { primary: true })));
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// install: codec and key

function codecView() {
	const out = h("div", { class: "flasher" });
	const draw = async () => {
		const parts = [];
		let fw;
		try {
			fw = await ensureFirmware();
		} catch (e) {
			out.replaceChildren(panel(t("flash.codec.title"), notice(errorText(e), "bad")));
			return;
		}
		parts.push(h("p", { class: "small muted" }, t("flash.codec.image", { what: fw.source, size: fw.image.length })));
		const haveStock = !!(S.stock && fb().profiles[S.stock.info.sha]);
		try {
			S.decision = decideCodec(S.board, fw.manifest, {
				haveStock, requested: S.requested || null,
				probe: S.now?.kind === "odr" ? S.now.probe : null,
				stockInstalled: S.now?.kind === "stock" ? S.now.version : null,
			});
			parts.push(notice(`${t(`flash.codec.mode.${S.decision.mode}`)}: ${S.decision.why}.`, "ok"));
			if (S.decision.risk) parts.push(h("p", { class: "small muted" }, S.decision.risk + "."));
		} catch (e) {
			S.decision = null;
			parts.push(notice(e.message, "bad"));
		}
		const needKey = !S.sim && !savedKey(S.board);
		const stockBox = h("div", {},
			h("h3", {}, t("flash.codec.stock_title")),
			h("p", {}, t(needKey || S.decision?.mode !== KEEP ? "flash.codec.stock_needed" : "flash.codec.stock_optional", { files: S.board.codec.sources.join(", ") })),
			S.stock ? notice(t("flash.codec.stock_loaded", { name: S.stock.name, what: S.stock.info.known || t("flash.codec.stock_unknown") }), S.stock.info.known ? "ok" : "bad") : null,
			S.sim ? button(t("flash.sim.use_stock"), async () => { await useStock({ name: "simulated-stock.bin", data: S.sim.stock }); })
				: filePicker(t("flash.codec.stock_pick"), ".bin", useStock));
		const canGo = S.decision && (S.sim || !needKey || S.stock);
		out.replaceChildren(panel(t("flash.codec.title"), h("p", {}, t("flash.codec.lead")), ...parts, stockBox,
			h("details", { class: "small" }, h("summary", {}, t("flash.codec.advanced")),
				h("label", {}, t("flash.codec.force"), " ", h("select", { onchange: (e) => { S.requested = e.target.value || null; draw(); } },
					h("option", { value: "" }, t("flash.codec.force_auto")),
					h("option", { value: KEEP, selected: S.requested === KEEP }, t("flash.codec.mode.keep")),
					h("option", { value: MERGE, selected: S.requested === MERGE }, t("flash.codec.mode.merge")))))),
		h("div", { class: "actions" }, button(t("flash.next"), next, { primary: true, disabled: !canGo })));
	};
	async function useStock(f) {
		S.stock = { ...f, info: await stockfw.identify(f.data, fb()) };
		S.key = null;
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
function bootloaderPanel(title, makePlanFn, onDone) {
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
				write.disabled = false;
				connect.disabled = true;
			} catch (e) {
				status.replaceChildren(notice(errorText(e), "bad"));
			}
		});
		const write = button(t("flash.write.go"), async () => {
			write.disabled = true;
			box.append(prog.el, log.el);
			try {
				await dropSerial();
				await new Dfu(transport, { log: log.add }).run(plan, S.key, (d, total) => prog.set(d, total, t("flash.write.writing")));
				log.add(t("flash.write.finished"));
				await transport.close?.();
				onDone();
			} catch (e) {
				status.replaceChildren(notice(t("flash.write.failed", { error: errorText(e) }), "bad"));
				write.disabled = false;
			}
		}, { primary: true, disabled: true });
		box.replaceChildren(h("h2", {}, title),
			h("details", { class: "small" }, h("summary", {}, t("flash.write.plan", { erase: plan.erase.length, blocks: plan.totalBlocks })), planTable(plan),
				h("pre", { class: "log" }, plan.describe().join("\n"))),
			h("h3", {}, t("flash.write.bootloader_title")),
			h("ol", { class: "steps" }, S.radio.flash.bootloader.map((s) => h("li", {}, s))),
			S.fakeRadio ? button(t("flash.sim.enter_bootloader"), () => { S.fakeRadio.enterBootloader(); log.add("(simulated) radio in its bootloader"); }) : null,
			h("p", { class: "small muted" }, t("flash.write.windows_hint")),
			h("div", { class: "actions" }, connect, write), fp, status);
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
	}, next);
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
				status.append(backupNow({ after: S.now?.kind !== "odr" }));
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
			backupNow());
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
	const choices = h("div", { class: "choice-grid" }, ["odr", "stock"].map((tg) =>
		h("button", { class: "choice", type: "button", disabled: !avail.includes(tg), "aria-pressed": String(S.restoreTarget === tg),
			onclick: () => { S.restoreTarget = tg; render(); } },
		h("strong", {}, t(`flash.restore.to_${tg}`)), h("span", { class: "small muted" }, t(`flash.restore.to_${tg}_text`)))));
	const parts = [panel(t("flash.restore.what"), summary, choices)];
	// the stock file: the application to write back, or (any target) where the bootloader key comes from
	if (S.restoreTarget && !S.stock && (S.restoreTarget === "stock" || (!S.sim && !savedKey(S.board))))
		parts.push(panel(t("flash.codec.stock_title"), h("p", {}, t("flash.restore.stock_needed")),
			filePicker(t("flash.codec.stock_pick"), ".bin", async (f) => { S.stock = { ...f, info: await stockfw.identify(f.data, fb()) }; render(); })));
	else if (S.restoreTarget) {
		parts.push(bootloaderPanel(t("flash.restore.write_title"), async (key) =>
			(S.restoreTarget === "odr" ? planInternal(b, S.board) : planStock(S.stock.data, key, S.board, fb())), () => {
			if (S.fakeRadio && S.restoreTarget === "stock") S.fakeRadio.running = "stock";
			box.append(afterRestore());
		}));
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
	try {
		S.data = await loadData();
	} catch (e) {
		app.replaceChildren(notice(t("flash.err.load"), "bad"));
		return;
	}
	if (location.hash.includes("sim")) {
		S.sim = await simulation(S.data.fb, "dm1701");
		S.fakeRadio = new FakeRadio(S.sim, { state: "stock", spiSize: 0x400000 });
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
