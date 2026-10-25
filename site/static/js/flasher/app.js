// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher: a step-by-step page over the modules beside it. Three tasks:
//
//   install   pick the radio and the build, find out what runs on the radio (backing it up if it runs RadioFoundry), decide
//             the codec, write through the bootloader (or, on a radio already running RadioFoundry with stub entry, through
//             the STM32F4 loader: f4stub.js), check the result and complete the backup
//   backup    read everything a radio running RadioFoundry can give into a bundle the user keeps (over the debug link, or
//             through the STM32F4 loader when RadioFoundry offers it: readers.js)
//   restore   put a radio back from a bundle: exactly as it was on RadioFoundry, or back on stock firmware
//
// How a radio is reached (which steps a task takes on it) is its backend's business: backends.js.
//
// Everything runs in the page: release images come from this site, custom images from the build API, stock files
// and backups from the user's disk, which never leave it. Add #sim=<board>-stock to the address for a simulated radio
// on its stock firmware (no hardware; for trying the flow), #sim=<board> for one that already runs RadioFoundry, #sim for the
// first radio on stock firmware. The fake is built from the board's data.

import { hex, sha256, toHex, fromHex, startsWith, sleep } from "./bytes.js";
import * as stockfw from "./stockfw.js";
import { openVendorFile, describe as describeVendorFile, analysisCandidate } from "./vendorfile.js";
import { makePlan, KEEP, MERGE } from "./plan.js";
import { validRef, refShort, defaultRef } from "./version.js";
import { verify, segments, forDevBuild } from "./manifest.js";
import { Dfu, WebUsbTransport, checkKey, usbSupported } from "./dfu.js";
import { Link, SerialStream, RFOUNDRY_USB, RFOUNDRY_NET, probe, serialSupported } from "./link.js";
import { readRadio, makeBundle, openBundle, bundleName, describeFirmware, CODEPLUG_SPAN } from "./backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug, codeplugLayout } from "./restore.js";
import { Selection } from "./select.js";
import { decideCodec } from "./decide.js";
import { readBootKey } from "./bootkey.js";
import { codecSegments, loadCodec, readCodec, saveCodec } from "./codecsave.js";
import { FakeRadio, simulation, randomBytes, rng } from "./fake.js";
import { t, ROOT, h, download, filePicker, progressBar, logBox, notice, button, panel, localTime, partNames } from "./dom.js";
import * as c7ui from "./c7000ui.js";
import { FakeC7000, c7000Simulation, fakeC7000Options } from "./fakec7000.js";
import { APP_ENTRY } from "./iap.js";
import { slotBytes, locateCodec } from "./c7000.js";
import { Stub } from "./stub.js";
import { F4Stub, f4Image } from "./f4stub.js";
import { f4LoaderReader } from "./readers.js";
import { register, backendOf, bundleBackend, allBackends } from "./backends.js";
import * as diag from "../diag.js";
import { initReporting, reportLine } from "../report.js";

// ---------------------------------------------------------------------------------------------------------------
// strings and DOM

const app = document.getElementById("flasher");

class NetworkModeError extends Error {}

// Every error shown goes through here: it is logged once (console, bug report) with the step it happened in
function errorText(e) {
	diag.error(`${S.task || "start"}/${stepsFor()?.[S.step] ?? S.step}`, e);
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
	now: null,		// what runs on the radio: {kind: "rfoundry", probe} | {kind: "stock", version} | {kind: "unknown"}
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
const API = "https://api.radiofoundry.org/v1/";
// minimal non-invasive reporting for helping the RadioFoundry project track programming failures; opt out with
// ?reporting=0
const FLASH_LOG = new URLSearchParams(location.search).get("reporting") !== "0";

// A version link (/flash/?version=<tag, full commit hash or branch name>) builds that version instead of the latest release. The tab keeps
// it for the session, so a reload or a visit to another page doesn't lose it; ?version= (empty) or the banner drops it.
const VERSION_STORE = "rfoundry-flash-version";
let VERSION_REF = (() => {
	const q = new URLSearchParams(location.search).get("version");
	const ref = q && /^[0-9A-Fa-f]+$/.test(q) ? q.toLowerCase() : q;
	try {
		if (ref) sessionStorage.setItem(VERSION_STORE, ref);
		else if (ref === "") sessionStorage.removeItem(VERSION_STORE);
		return sessionStorage.getItem(VERSION_STORE);
	} catch (e) { return ref || null; }
})();
const versionShort = () => refShort(VERSION_REF);

// redraw = false: the caller keeps its view (a build about to start), only the banner goes
function dropVersion(redraw = true) {
	VERSION_REF = null;
	try { sessionStorage.removeItem(VERSION_STORE); } catch (e) { /* nothing kept */ }
	const u = new URL(location.href);
	u.searchParams.delete("version");
	history.replaceState(null, "", u);
	if (S.firmware?.pinned) S.firmware = null;
	if (redraw) render();
	else document.querySelector(".version-banner")?.remove();
}

// Asked once per build, before a version build starts: true builds it, "release" drops the link and builds the
// release, false stays on the step
function confirmVersion() {
	return new Promise((done) => {
		const close = (v) => { dlg.close(); dlg.remove(); done(v); };
		const dlg = h("dialog", { class: "confirm", "aria-labelledby": "version-confirm-title", oncancel: (e) => { e.preventDefault(); close(false); } },
			h("h2", { id: "version-confirm-title" }, t("flash.version.confirm_title", { ref: versionShort() })),
			h("p", {}, t("flash.version.confirm_text", { ref: versionShort() })),
			h("p", { class: "small muted" }, t("flash.version.confirm_back")),
			h("div", { class: "confirm-actions" },
				button(t("flash.version.use_release"), () => close("release"), { quiet: true }),
				button(t("flash.version.confirm_go", { ref: versionShort() }), () => close(true), { primary: true })));
		document.body.append(dlg);
		dlg.showModal();
	});
}

// on every step while a version link is in use: what gets built, and the way back to the release
function versionBanner() {
	if (!VERSION_REF) return null;
	const ok = validRef(VERSION_REF);
	return h("div", { class: `notice ${ok ? "warn" : "bad"} version-banner` },
		h("p", {}, ok ? [h("b", {}, t("flash.version.title", { ref: versionShort() })), " ", t("flash.version.text")] : t("flash.version.bad", { ref: VERSION_REF })),
		h("p", {}, h("a", { href: "#", onclick: (e) => { e.preventDefault(); dropVersion(); } }, t("flash.version.use_release"))));
}

async function api(method, path, body) {
	const r = await fetch(new URL(path, API), body ? { method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : { method });
	const j = await r.json().catch(() => ({}));
	if (!r.ok) throw new Error(j.detail || `HTTP ${r.status}`);
	return j;
}

// "Send this file for further analysis": a vendor firmware file the page couldn't take the codec from goes to the
// backend (POST /v1/samples: vendor containers only, size and rate limited, never served back), so its build can be
// mapped. Only on the user's click, with what was found; nothing from the radio. Not offered in a simulation
function analysisOffer(cand, boardId) {
	if (!cand || S.sim) return null;
	const status = h("p", { class: "small" });
	const note = h("textarea", { class: "analysis-note", rows: 2, maxlength: 500, placeholder: t("flash.analysis.note") });
	const send = button(t("flash.analysis.send"), async () => {
		send.disabled = true;
		status.textContent = t("flash.analysis.sending");
		const q = new URLSearchParams({ board: boardId, name: cand.file.path.split("/").pop(), reason: cand.reason,
			version: S.data.site.version || "", note: note.value.trim() });
		try {
			const r = await fetch(new URL(`samples?${q}`, API), { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: cand.file.data });
			const j = await r.json().catch(() => ({}));
			if (!r.ok) throw new Error(j.detail || `HTTP ${r.status}`);
			status.textContent = t(j.status === "already-have" ? "flash.analysis.have" : "flash.analysis.sent");
		} catch (e) {
			status.textContent = t("flash.analysis.failed", { error: e.message });
			send.disabled = false;
		}
	});
	return h("div", { class: "notice" }, h("strong", {}, t("flash.analysis.title")),
		h("p", {}, t("flash.analysis.text", { name: cand.file.path.split("/").pop() })), note, send, status);
}

// One id per use: the custom build requests and the write that follows share it; each write ends the use
const useId = () => (FLASH_LOG && !S.sim ? (S.useId ||= crypto.randomUUID()) : undefined);

// Fire and forget: a flash never waits for these or fails because of them
function flashStarted(what) {
	diag.note("flash started", what);
	const id = useId();
	if (!id) return null;
	S.useId = null;
	const sent = fetch(new URL("flashes", API), { method: "POST", keepalive: true, headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ id, tool: "web", tool_version: S.data.site.version || "", ...what }) }).catch(() => {});
	return { id, sent };
}

async function flashFinished(started, error = null) {
	if (error) errorText(error);
	else diag.note("flash finished");
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
	help: ["radio", "symptom", "recover"],
};
// A radio running RadioFoundry that updates over the cable has no codec decision to make: the step goes
const STUB_INSTALL = ["radio", "build", "now", "write", "done"];
// Installing again from the help task: what runs on the radio is known to be broken, so that question goes
const RECOVER_INSTALL = ["build", "codec", "write", "done"];
// The radio's backend (backends.js) decides a task's steps; the shared ones otherwise
const stepsFor = () => (S.radio && backendOf(S.radio).steps(S.task, { recovering: !!S.recovering })) || STEPS[S.task];
const backend = () => backendOf(S.radio);

async function loadData() {
	const get = async (name) => (await fetch(`${ROOT}data/${name}`)).json();
	const [radios, features, variants, fb, releases, site] = await Promise.all(
		["radios.json", "features.json", "variants.json", "flash-boards.json", "releases.json", "site.json"].map(get));
	return { radios: radios.radios, features: features.features, groups: features.groups, variants: variants.variants, fb, releases: releases.releases, site };
}

const fb = () => (S.sim ? S.sim.fb : S.data.fb);
// Every image comes from the build service, so a radio needs only a backend the page has for it (backends.js)
const available = (r, task) => backendOf(r).available(r, fb(), task);

// The STM32 radios: stock firmware is reached through the AnyRoad bootloader (dfu.js), which takes
// the key from the stock file; a radio running RadioFoundry with stub entry also updates and backs up through the STM32F4
// loader (f4stub.js), and backs up over the debug link without it (backup.js). Stock firmware isn't backed up here.
register({
	id: "anyroad-dfu",
	available: (r, fbd, task) => (task === "backup" ? r.backup.rfoundry === "full" : !!fbd.boards[r.flash.board]?.key_sha256),
	steps: (task, { recovering }) => (task !== "install" ? null : recovering ? RECOVER_INSTALL : stubRoute() ? STUB_INSTALL : null),
	views: { now: nowView, codec: codecView, write: writeView, done: doneView, backup: backupView, restore: restoreView },
	recovery: { first: "updatemode", stock: "file", reach: null, stockText: "flash.help.fix.stock_text" },
});

// The STM32F4 loader route (f4stub.js): the radio runs RadioFoundry with stub entry and its codec, the image leaves the codec
// alone, and nobody asked for a bootloader mode. Everything else, a stock radio first of all, uses the bootloader.
const f4Board = () => fb().f4stub?.boards[S.board?.id];
const stubRoute = () => !!(S.task === "install" && f4Board() && S.now?.kind === "rfoundry" && S.now.stubOffer && !S.requested
	&& S.now.probe?.codec === S.board.codec.id && !S.firmware?.manifest.codec?.included);
let f4Json = null;
const f4StubJson = async () => (f4Json ||= await (await fetch(`${ROOT}data/f4-stub.json`)).json());

// Build options (radios.json flash.build_options, such as the MD-380's band): all chosen, and the value a radio
// running RadioFoundry reports by its debug link name
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

// What runs on the radio, once it answered as RadioFoundry: on the STM32 radios also whether it offers the loader hand-over
async function rfoundryNow(link, p) {
	return { kind: "rfoundry", probe: p, stubOffer: f4Board() ? await Stub.rfoundryOffer(link) : null };
}

const grantedRFoundryPorts = async () => (await navigator.serial.getPorts()).filter((p) => {
	const i = p.getInfo();
	return i.usbVendorId === RFOUNDRY_USB.usbVendorId && i.usbProductId === RFOUNDRY_USB.usbProductId;
});

async function connectSerial() {
	if (S.link) return S.link;
	let stream;
	if (S.fakeRadio) stream = S.fakeRadio.serialStream();
	else {
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		if (await networkModeSeen()) throw new NetworkModeError();
		const granted = await grantedRFoundryPorts();
		stream = granted.length ? await SerialStream.open(granted[0]) : await SerialStream.request();
	}
	S.link = new Link(stream);
	return S.link;
}

// The loader's port after it enumerated again: the same USB IDs and serial number, so the browser lists it as granted
// (no prompt). Throws until it is back.
async function reopenRFoundryPort() {
	if (S.fakeRadio) return S.fakeRadio.serialStream();
	const ports = await grantedRFoundryPorts();
	if (!ports.length) throw new Error("not back yet");
	return SerialStream.open(ports[ports.length - 1]);
}

// After the loader's restart: RadioFoundry answering on its port again (S.link set), within ms. false: it hasn't come back
async function waitForRFoundry(ms = 20000) {
	const end = Date.now() + ms;
	while (Date.now() < end) {
		await sleep(S.fakeRadio ? 10 : 1000);
		let stream = null;
		try {
			stream = await reopenRFoundryPort();
			const link = new Link(stream);
			if (await probe(link)) {
				S.link = link;
				return true;
			}
		} catch (e) { /* not back yet */ }
		try { await stream?.close?.(); } catch (e) { /* gone */ }
	}
	return false;
}

// A radio in USB network mode that this page was allowed to see before (no prompt)
async function networkModeSeen() {
	if (!usbSupported()) return false;
	try {
		return (await navigator.usb.getDevices()).some((d) => d.vendorId === RFOUNDRY_NET.vendorId && d.productId === RFOUNDRY_NET.productId);
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
			await navigator.usb.requestDevice({ filters: [RFOUNDRY_NET] });
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
		h("ol", { class: "steps" }, h("li", {}, t("flash.net.step1")), h("li", {}, t("flash.net.step2")), h("li", {}, t("flash.net.step3"))));
}

function connectError(e) {
	return e instanceof NetworkModeError ? networkModeHelp() : notice(errorText(e), "bad");
}

async function dropSerial() {
	try { await S.link?.s.close(); } catch (e) { /* gone */ }
	S.link = null;
}

async function connectBootloader() {
	if (S.dfuReady) {	// found by the help task's update mode check a moment ago
		const tr = S.dfuReady;
		S.dfuReady = null;
		return tr;
	}
	if (S.fakeRadio) {
		if (S.fakeRadio.mode !== "bootloader") throw new Error(t("flash.err.not_in_bootloader"));
		return S.fakeRadio.dfuTransport();
	}
	if (!usbSupported()) throw new Error(t("flash.err.no_usb"));
	return WebUsbTransport.request(S.board);
}

const KEY_STORE = (id) => `rfoundry-key-${id}`;

// Windows needs a driver for the bootloader; nobody else should read about it
const onWindows = () => /win/i.test(navigator.userAgentData?.platform || navigator.platform || "");
const WINUSB_STORE = "rfoundry-winusb-ok";

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

// A radio that runs RadioFoundry hands over its bootloader key (bootkey.js), so updating it needs no stock file. Quiet when it
// can't: the stock file still works.
async function keyFromRadio(link) {
	if (S.key || !S.board?.key_sha256) return;
	const key = await readBootKey(link, S.board, fb().key_len);
	if (!key) return;
	S.key = key;
	S.keyFromRadio = true;
	if (!S.sim) try { localStorage.setItem(KEY_STORE(S.board.id), toHex(key)); } catch (e) { /* not remembered */ }
}

// A radio running RadioFoundry with its codec hands it over (codecsave.js), and this browser keeps a copy: where an update
// erases the codec (keep_codec_supported false) before anything is erased, elsewhere once, for an install that can't
// keep it (a radio back on stock firmware, an interrupted install, a radio in its bootloader). Quiet when it can't:
// the stock file still works.
async function codecFromRadio(link, probe, status) {
	if (probe?.codec !== S.board.codec.id || S.savedCodec?.from === "radio") return;
	if (S.board.keep_codec_supported && !S.sim && (await loadCodec(S.board, fb()))) return;
	status?.replaceChildren(h("p", { class: "muted" }, t("flash.now.reading_codec")));
	const got = await readCodec(link, S.board, fb());
	if (got) S.savedCodec = { ...got, from: "radio", stored: S.sim ? false : await saveCodec(S.board, got.segments) };
}

// A stock file the user picked (a .bin, a readout, or the vendor's zip with updaters and several builds inside),
// opened by vendorfile.js: the best file for this radio, identified, and one line per file for the page (report)
async function pickedStock(f) {
	try {
		const opened = await openVendorFile(f.name, f.data, fb(), S.board.id);
		const report = describeVendorFile(opened, fb(), S.board.id);
		const b = opened.best;
		const analysis = analysisCandidate(opened, fb());
		if (!b) return { ...f, report, analysis, info: { sha: null, known: null, profile: null } };
		return { name: b.path.split("/").pop(), data: b.data, report, analysis, match: b.match, info: await stockfw.identify(b.data, fb()) };
	} catch (e) {
		console.warn(e.message);
		return { ...f, report: [e.message], info: { sha: null, known: null, profile: null } };
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
	document.querySelector(".page-head")?.classList.toggle("compact", !!S.task);
	if (!S.task) return app.append(...[versionBanner(), startView()].filter(Boolean));
	const steps = stepsFor();
	const label = (s) => (S.radio && backend().stepLabel?.(s, S.task)) || s;
	const stepper = () => h("ol", { class: "stepper" }, stepsFor().map((s, i) =>
		h("li", { class: i < S.step ? "done" : "", "aria-current": i === S.step ? "step" : false }, t(`flash.step.${label(s)}`))));
	let shown = stepper();
	app.append(shown);
	render.stepper = () => { const n = stepper(); shown.replaceWith(n); shown = n; };
	if (S.sim) app.append(h("p", { class: "small muted sim-line" }, t("flash.sim.banner")));
	if (VERSION_REF && S.task === "install") app.append(versionBanner());
	if (S.radio && S.radio.status !== "supported" && S.step > 0) app.append(prereleaseNotice());
	// One bar of buttons under every step: back, start over, and the step's own next action, which the view sets (and
	// changes when it redraws) through S.forward. The main action is always there, at the right; a step never puts a
	// second one in its panel. S.forward also takes [alternative, ..., main].
	const info = h("div", { class: "nav-info" }), fwd = h("div", { class: "nav-forward" });
	S.forward = (btn, text = null) => {
		fwd.replaceChildren(...[btn].flat().filter(Boolean));
		info.replaceChildren(...[text].filter(Boolean));
	};
	// the shared steps, then the ones the radio's backend draws; a bundle's own backend restores it
	const step = steps[S.step];
	diag.note(`step ${S.task || "start"}/${step}`);
	const view = (step === "restore" && bundleBackend(S.bundle)?.views.restore) || (S.radio && backend().views[step])
		|| { radio: radioView, build: buildView, bundle: bundleView, symptom: symptomView, recover: recoverView }[step];
	app.append(view());
	app.append(h("div", { class: "nav-bar" },
		h("div", { class: "nav-back" },
			S.step > 0 ? button("← " + t("flash.back"), () => { S.step--; render(); }) : null,
			h("button", { class: "btn btn-quiet", type: "button", onclick: () => reset() }, t("flash.start_over"))),
		info, fwd));
}

// A radio still in development (board.toml [status] below supported): on every step after it is picked
function prereleaseNotice() {
	const repo = S.data.site.repo;
	return h("div", { class: "notice prerelease", role: "note" },
		h("p", {}, h("strong", {}, t("flash.prerelease.title")), " ", t("flash.prerelease.text", { radio: S.radio.name })),
		S.radio.flash.notice ? h("p", {}, h("strong", {}, S.radio.flash.notice)) : null,
		repo ? h("p", {}, h("a", { href: `${repo}/issues/new/choose`, target: "_blank", rel: "noopener" }, t("flash.prerelease.link"))) : null);
}

function next() {
	S.step++;
	render();
	app.scrollIntoView({ behavior: "smooth", block: "start" });
}

async function reset() {
	await dropSerial();
	for (const b of allBackends()) await b.release?.();
	const keep = { data: S.data, sim: S.sim, fakeRadio: S.fakeRadio, fakeC7: S.fakeC7 };
	// exactly a fresh load: fields set along the way (buildOptions, keyFromRadio, ...) go, the rest start over
	for (const k of Object.keys(S)) delete S[k];
	Object.assign(S, initialState(), keep);
	render();
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
			h("div", { class: "choice-grid" }, choice("install", "⤓"), choice("backup", "⎘"), choice("restore", "↺"), choice("help", "?"))),
		h("details", { class: "small" }, h("summary", {}, t("flash.start.privacy_title")), h("p", {}, t("flash.start.privacy"))),
		h("p", { class: "small muted" }, S.sim ? h("a", { href: "#", onclick: (e) => { e.preventDefault(); location.hash = ""; location.reload(); } }, t("flash.sim.leave"))
			: simLinks()));
}

// The practice links: the first radio, and the first one flashed through the C7 stub on stock firmware and on RadioFoundry
function simLinks() {
	const link = (hash, text) => h("a", { href: hash, onclick: () => setTimeout(() => location.reload(), 0) }, text);
	const c7 = S.data.radios.find((r) => r.flash.backend === "c7000-stub");
	return [link("#sim", t("flash.sim.try")), ...(c7 ? [" · ", link(`#sim=${c7.id}-stock`, t("flash.sim.try_c7000", { name: c7.model })),
		" · ", link(`#sim=${c7.id}`, t("flash.sim.try_c7000_rfoundry"))] : [])];
}

// ---------------------------------------------------------------------------------------------------------------
// install and backup: the radio

function radioView() {
	const list = h("div", { class: "choice-grid" }, S.data.radios.map((r) => {
		const ok = available(r, S.task === "backup" ? "backup" : "install");
		return h("button", { class: "choice", type: "button", "aria-pressed": String(S.radio?.id === r.id), disabled: !ok,
			onclick: () => { selectRadio(r); next(); } },
		h("img", { src: ROOT + r.image, alt: "" }), h("strong", {}, r.name),
		h("span", { class: "small muted" }, r.also.join(", ")),
		h("span", { class: "small" }, ok ? t(`status.${r.status}`) : t("flash.radio.not_yet")));
	}));
	const detect = button(t("flash.radio.detect"), async () => {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.err.no_answer"));
			const r = S.data.radios.find((x) => S.data.fb.boards[x.flash.board]?.debuglink_names.includes(p.board));
			if (!r) throw new Error(t("flash.err.unknown_board", { board: p.board }));
			selectRadio(r);
			optionsFromProbe(p);
			S.now = await rfoundryNow(link, p);
			await keyFromRadio(link);
			await codecFromRadio(link, p, status);
			next();
		} catch (e) {
			await dropSerial();
			status.replaceChildren(connectError(e), serialHelp());
		}
	}, { quiet: true });
	const status = h("div");
	S.forward(null);	// picking a radio is the step's action
	return panel(t("flash.radio.title"), list,
		S.task === "help" ? null : h("p", { class: "small muted detect" }, t("flash.radio.detect_hint"), " ", detect), status);
}

function selectRadio(r) {
	if (S.radio && S.radio.id !== r.id) Promise.resolve(backend().release?.()).then(() => { S.c7 = null; });
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
		if (!S.firmware && VERSION_REF && validRef(VERSION_REF)) {
			const yes = await confirmVersion();
			if (!yes) return;
			if (yes === "release") dropVersion(false);	// and go on with the release
		}
		if (!S.firmware && S.sim) await ensureFirmware();
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
			// what this radio can't run is listed too, grayed out, so nobody wonders where it went
			const all = S.data.features.filter((f) => f.group === g && f.name !== "AUDIO_PROBE");
			const items = all.filter((f) => sel.supported(f.name));
			if (!all.length) return null;
			const on = items.filter((f) => sel.on.has(f.name)).length;
			const base = S.radio.config[sel.base];
			const changed = items.filter((f) => base.includes(f.name) !== sel.on.has(f.name)).length;
			const details = h("details", { class: "feature-group", open: S.openGroups.has(g),
				ontoggle: (e) => { if (e.target.open) S.openGroups.add(g); else S.openGroups.delete(g); } },
			h("summary", {}, h("span", { class: "group-name" }, gtitle(g)),
				h("span", { class: "group-count" }, t("flash.build.group_count", { on, total: items.length }),
					changed ? h("span", { class: "muted" }, " · ", t("flash.build.group_changed", { n: changed })) : null)),
			nest(all).map(({ f, depth }) => {
				const na = !sel.supported(f.name);
				const isOn = sel.on.has(f.name);
				const off = S.data.variants.find((v) => v.id === sel.base)?.off.find((o) => o.name === f.name);
				// a needed feature outside this group that is off: turning this on turns it on too
				const away = (f.needs?.all || []).filter((n) => !items.some((i) => i.name === n) && sel.supported(n) && !sel.on.has(n));
				// one line per feature; the help (written for developers) is the tooltip, the build's reason for leaving
				// a feature out and a needed feature that's off show inline
				const note = na ? [naNote(f.name)] : [off && !isOn ? off.why : null,
					away.length ? t("flash.build.needs", { names: away.map((n) => sel.byName[n]?.title || n).join(", ") }) : null].filter(Boolean);
				const tipId = `tip-${f.name}`;
				const tipOpen = S.openTip === f.name;
				return h("div", { class: `toggle${na ? " unsupported" : base.includes(f.name) !== isOn ? " changed" : ""}`, style: `--depth: ${depth}` },
					h("input", { type: "checkbox", id: `f-${f.name}`, checked: isOn && !na, disabled: na || sel.required(f.name),
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
		forward(button(t(busy ? "flash.nav.building" : missing ? "flash.nav.choose" : S.firmware || S.sim ? `flash.nav.to_${stepsFor()[S.step + 1]}`
			: VERSION_REF ? "flash.nav.build_version" : "flash.nav.build", { what: missing?.title.toLowerCase(), ref: VERSION_REF && versionShort() }), go, { primary: true, disabled: busy || !!missing }), summary);
		// every image, prebuilt choice or not, comes from the build service (or is the user's own)
		// the image's state, then building it yourself: one line and a fold under the features, not a card of its own
		const image = h("div", { class: "build-image" },
			S.firmware ? notice(t("flash.build.image_ready", { what: S.firmware.source }), "ok")
				: building && S.build.error ? notice(S.build.error, "bad")
				: building ? h("p", { class: "muted" }, S.build.text)
				: h("p", { class: "small muted" }, t(S.sim ? "flash.sim.image" : VERSION_REF ? "flash.build.version_on_request" : "flash.build.on_request", { ref: versionShort() })),
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
				h("div", { class: "feature-groups" }, groups),
				image));
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

// Why a feature can't be chosen: the radio's code isn't written for it yet, or its hardware can't
const naNote = (name) => t(S.selection.notWritten(name) ? "flash.build.not_written" : "flash.build.unsupported",
	{ radio: S.radio.name });

// The ⓘ bubble: what a feature does, what it needs, its flash cost on this radio
function featureTip(f, id) {
	const titles = (names) => names.map((n) => S.selection.byName[n]?.title || n).join(", ");
	const { all = [], any = [] } = f.needs || {};
	const needs = [all.length ? titles(all) : null, any.length ? t("flash.build.tip_any", { names: titles(any) }) : null].filter(Boolean);
	const bytes = f.bytes?.[S.radio.model];
	return h("div", { class: "tip", id, role: "note", onclick: (e) => e.stopPropagation() },
		S.selection.supported(f.name) ? null : h("p", { class: "tip-na" }, naNote(f.name)),
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

const selectionKey = (sel) => `${VERSION_REF || ""}:${sel.radio.id}:${JSON.stringify(S.buildOptions || {})}:${[...sel.on].sort().join(",")}`;

// Ask the build API for the current selection, wait for it, and take the image like a release's
async function requestBuild(redraw) {
	const sel = S.selection, key = selectionKey(sel);
	const current = () => S.build?.selection === key && selectionKey(S.selection) === key;
	S.build = { selection: key, text: t("flash.build.sending") };
	redraw();
	try {
		const rels = S.data.releases.filter((x) => x.board === S.radio.id);
		if (VERSION_REF && !validRef(VERSION_REF)) throw new Error(t("flash.version.bad", { ref: VERSION_REF }));
		let b = await api("POST", "builds", { board: S.radio.id, ref: VERSION_REF || (rels.length ? rels[rels.length - 1].version : defaultRef(S.data.site.branch)), features: [...sel.on].sort(),
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
			variant: sel.matches() || "custom", build: b.id, selection: key, pinned: !!VERSION_REF };
		S.build = null;
	} catch (e) {
		if (S.build?.selection === key) S.build.error = t("flash.build.unavailable", { error: errorText(e) });
	}
	redraw();
}

async function loadOwnImage(f, redraw) {
	try {
		if (startsWith(f.data, fb().magic)) throw new Error(t("flash.err.stock_as_image"));
		// an image merged with its codec (tools/hrc7000_codec.py): the backend keeps the codec, the rest is the image
		if (backend().ownImage) f = { ...f, data: await backend().ownImage(fb(), S.radio.flash.board, f.data) };
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
		img.set(new TextEncoder().encode("RadioFoundry"), 0x1000);	// what a C7000 read recognizes RadioFoundry by
		img.set([0xFC, 0xFF, 0x01, 0x20], 0);	// a RAM stack pointer, as the STM32 loader checks
		const fs = S.radio?.flash.first_stage;
		if (fs) {	// an install image with a first stage: its signature, then RadioFoundry's reset handler after it
			img.set(fromHex(fs.head), 0);
			img.set(APP_ENTRY, fs.size);
			img.set(new TextEncoder().encode("RadioFoundry"), fs.size + 0x100);
		}
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
	let runBackup = null;
	const draw = () => {
		runBackup = null;
		const opts = h("div", { class: "choice-grid" },
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "rfoundry"), onclick: checkRFoundry },
				h("strong", {}, t("flash.now.rfoundry")), h("span", { class: "small muted" }, t("flash.now.rfoundry_text"))),
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "stock"), onclick: () => { S.now = { kind: "stock", version: null }; draw(); } },
				h("strong", {}, t("flash.now.stock")), h("span", { class: "small muted" }, t("flash.now.stock_text"))),
			h("button", { class: "choice", type: "button", "aria-pressed": String(S.now?.kind === "unknown"), onclick: () => { S.now = { kind: "unknown" }; draw(); } },
				h("strong", {}, t("flash.now.unknown")), h("span", { class: "small muted" }, t("flash.now.unknown_text"))));
		const detail = h("div");
		if (S.now?.kind === "rfoundry" && S.now.probe) {
			const p = S.now.probe;
			detail.append(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.board")), h("dd", {}, p.board),
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, codecText(p.codec))),
			S.backupDone ? notice(t("flash.backup.done_before"), "ok") : backupNow({ when: "before", onDone: draw, actions: (b) => { runBackup = b; } }));
		} else if (S.now?.kind === "stock") {
			const versions = S.radio.flash.stock_versions;
			detail.append(h("label", {}, t("flash.now.which_version"), " ",
				h("select", { onchange: (e) => { S.now.version = e.target.value || null; } },
					h("option", { value: "" }, t("flash.now.version_unknown")),
					versions.map((v) => h("option", { value: v, selected: S.now.version === v }, v)))),
			notice(t(S.radio.backup.stock === "none" ? "flash.now.stock_backup_later" : "flash.now.stock_backup_planned"), "info"));
		}
		out.replaceChildren(panel(t("flash.now.title"), opts, detail, status));
		// on RadioFoundry the backup comes first: saving it is the main action, skipping it the quiet one beside it
		const skip = S.now?.kind === "rfoundry" && !S.backupDone && runBackup;
		if (S.now) render.stepper?.();	// the steps change when the radio turns out to update over the cable
		forward(skip ? [button(t("flash.nav.skip_backup"), next, { quiet: true }), runBackup]
			: button(t("flash.nav.to_codec"), next, { primary: true, disabled: !S.now || (S.now.kind === "rfoundry" && !S.now.probe) }));
	};
	const status = h("div");
	async function checkRFoundry() {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.err.no_answer"));
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			const odd = optionMismatch(p);
			if (odd) throw new Error(t("flash.err.wrong_option", { board: p.board, option: odd[0].title.toLowerCase(), value: odd[1].title }));
			S.now = await rfoundryNow(link, p);
			await keyFromRadio(link);
			await codecFromRadio(link, p, status);
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

// What the radio's codec report means to a user
const codecText = (c) => (c === S.board.codec.id ? t("flash.now.codec_ok") : !c || c === "absent" ? t("flash.now.codec_none") : c);

// The backup panel used on a RadioFoundry radio (before an update, the backup task, after a first install)
// actions(run): where the "save a backup" button goes (the nav bar); without it, in the panel
function backupNow({ when = "task", onDone = null, actions = null } = {}) {
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
	// the loader reads the internal flash at about 500 KB/s and the SPI flash at about 280 KB/s (stub-stm32f4.md 11),
	// plus the hand-over and the restart; the codeplug alone is no faster that way, so the debug link reads it
	const viaLoader = () => !!(f4Board() && S.now?.kind === "rfoundry" && S.now.stubOffer && pick !== "codeplug");
	const seconds = (id) => (id !== "codeplug" && f4Board() && S.now?.stubOffer
		? 12 + internalSize / 500e3 + (choices[id].what.spi === "full" ? S.radio.backup.spi_size / 280e3 : 0)
		: S.readRate ? choices[id].bytes / S.readRate : null);
	const run = async () => {
		box.querySelectorAll("button, input").forEach((b) => { b.disabled = true; });
		runBtn.disabled = true;
		box.append(prog.el);
		let started = null, bulk = null;
		try {
			const link = await connectSerial();
			const p = S.now?.probe || await probe(link);
			// the backup task is a use of its own; the backups around an install belong to the install
			if (S.task === "backup") started = flashStarted({ task: "backup", source: pick, radio: S.board.id, firmware: p.git || "", features: p.features || "" });
			const t0 = performance.now();
			bulk = viaLoader() ? f4LoaderReader(link, { stubJson: f4StubJson, reopen: reopenRFoundryPort, board: f4Board(), onHandOver: () => { S.link = null; } }) : null;
			S.backupParts = await readRadio(link, S.board, S.radio, {
				what: choices[pick].what, readers: bulk ? [bulk] : [], progress: (part, d, total) => prog.set(d, total, t(`flash.backup.part.${part}`)) });
			if (!bulk?.used) S.readRate = choices[pick].bytes / Math.max(0.5, (performance.now() - t0) / 1000);
			if (S.stock) S.backupParts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: S.stock.data, complete: true });
			const after = when === "after";
			const meta = {
				radio: { board: S.radio.id, name: S.radio.name, model: S.radio.model, chip: p.chip, version: p.version },
				firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec, features: p.features },
				firmware_before: after && S.now?.kind === "stock" ? { kind: "stock", version: S.now.version, stock_file_sha256: S.stock?.info.sha || null } : undefined,
				contents: pick,
				notes: after && S.now?.kind !== "rfoundry" ? [t("flash.backup.note_after")] : [],
			};
			const bytes = await makeBundle(meta, S.backupParts, { git: S.data.site.git, url: location.origin });
			const name = bundleName(meta);
			download(name, bytes, "application/zip");
			S.backupDone = true;
			flashFinished(started);
			const incomplete = S.backupParts.filter((x) => !x.complete && x.what !== "spi-flash").map((x) => x.file);
			box.append(notice(t("flash.backup.saved", { name, size: Math.round(bytes.length / 1024) }), "ok"));
			if (incomplete.length) box.append(notice(t("flash.backup.incomplete", { files: incomplete.join(", ") })));
			if (bulk?.used) {
				// the next step talks to RadioFoundry again: it has to be back first
				prog.set(0, 1, t("flash.backup.restarting"));
				if (!await waitForRFoundry()) {
					box.append(notice(t("flash.backup.not_back"), "action"));
					return;
				}
				prog.el.remove();
			}
			// the backup holds the whole memory chip now, so the original firmware's data in RadioFoundry's settings area can go
			if (pick === "all") await prepareStorage(box);
			onDone?.();
		} catch (e) {
			flashFinished(started, e);
			if (bulk?.used) await waitForRFoundry();	// readRadio restarted the radio
			box.append(notice(errorText(e), "bad"));
			box.querySelectorAll("button, input").forEach((b) => { b.disabled = false; });
			runBtn.disabled = false;
		}
	};
	const option = (id) => h("label", { class: "toggle" },
		h("input", { type: "radio", name: "backup-what", checked: id === pick, onchange: () => { pick = id; } }),
		h("span", {}, h("b", {}, t(`flash.backup.opt.${id}`)),
			h("span", { class: "why" }, t(`flash.backup.opt.${id}_text`), " ", times[id] = h("span", {}, estimate(choices[id].bytes, seconds(id))))),
		h("span"));
	box.append(h("h3", {}, t(`flash.backup.title_${when}`)),
		h("p", {}, t(when === "after" && S.now?.kind !== "rfoundry" ? "flash.backup.after_lead" : "flash.backup.lead")),
		option("all"), option("firmware"), option("codeplug"));
	const runBtn = button(t("flash.backup.run"), run, { primary: true });
	if (actions) actions(runBtn);
	else box.append(h("div", { class: "actions" }, runBtn));
	// the estimates come from this radio: a short timed read, unless a backup already measured it
	if (!S.readRate) {
		measureRate().then(() => { for (const id of Object.keys(choices)) times[id]?.replaceChildren(estimate(choices[id].bytes, seconds(id))); });
	}
	return box;
}

// A radio fresh from its original firmware still holds that firmware's data where RadioFoundry keeps its settings and journal:
// until that area is claimed the radio keeps no settings and refuses every codeplug write (settings-store.md 5)
async function prepareStorage(box) {
	try {
		const link = await connectSerial();
		const st = await link.storageInfo();
		diag.note("storage region", st);
		if (st?.region !== "foreign") return;
		const line = notice(t("flash.storage.preparing"));
		box.append(line);
		const ok = await link.storageClaim();
		line.replaceWith(notice(t(ok ? "flash.storage.done" : "flash.storage.failed"), ok ? "ok" : "bad"));
	} catch (e) {
		diag.error("flash/prepare storage", e);
		box.append(notice(t("flash.storage.failed"), "bad"));
	}
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

function estimate(bytes, s) {
	const mb = bytes / 1048576;
	const size = mb >= 1 ? `${mb.toFixed(mb >= 10 ? 0 : 1)} MB` : `${Math.round(bytes / 1024)} KB`;
	if (s == null) return t("flash.backup.time_unknown", { size });
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
		// the copy an earlier visit read from this radio: an update that stopped halfway leaves none in the radio, and a
		// radio back on stock firmware or in its bootloader can't say what it holds
		if (!S.savedCodec && !S.sim) {
			const got = await loadCodec(S.board, fb());
			if (got) S.savedCodec = { ...got, from: "browser", stored: true };
		}
		let err = null;
		try {
			S.decision = decideCodec(S.board, fw.manifest, {
				haveStock, haveSaved: !!S.savedCodec, requested: S.requested || null,
				probe: S.now?.kind === "rfoundry" ? S.now.probe : null,
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
		const fromSaved = S.decision?.source === "saved";
		const codecLine = !fromSaved ? null : S.savedCodec.from === "browser" ? t("flash.codec.codec_saved")
			: t(S.savedCodec.stored ? "flash.codec.codec_from_radio" : "flash.codec.codec_from_radio_unsaved");
		const picker = S.sim ? button(t("flash.sim.use_stock"), async () => { await useStock({ name: "simulated-stock.bin", data: S.sim.stock }); })
			: filePicker(t("flash.codec.stock_pick"), ".bin,.zip", useStock);
		const loadedText = S.stock && t("flash.codec.stock_loaded", { name: S.stock.name, what: S.stock.info.known
			|| (S.stock.match?.board === S.board.id ? t("flash.codec.stock_build", { radio: S.board.name }) : t("flash.codec.stock_unknown")) });
		// a known file without a codec profile (MD-380 D2.30) gives the bootloader key, not the codec: say what it's good for
		const keyOnly = S.stock?.info.known && !fb().profiles[S.stock.info.sha];
		// a build with the codec but no map yet, or a file for another radio: what each file in the pick is
		const unmapped = S.stock?.match?.kind === "codec-unmapped" && t("flash.codec.stock_unmapped", { files: S.board.codec.sources.join(", ") });
		const report = S.stock?.report?.length > 1 || (S.stock && !S.stock.info.profile)
			? h("details", { class: "small", open: !S.stock.info.known }, h("summary", {}, t("flash.codec.stock_report")), h("ul", {}, S.stock.report.map((l) => h("li", {}, l)))) : null;
		const loaded = !S.stock ? null : [keyOnly ? notice(`${loadedText}. ${t("flash.codec.stock_key_only", { files: S.board.codec.sources.join(", ") })}`, "action")
			: unmapped ? notice(`${loadedText}. ${unmapped}`, "action")
			: S.stock.info.known ? h("p", {}, "✓ ", loadedText) : notice(loadedText, "bad"), report, analysisOffer(S.stock.analysis, S.board.id)];
		// a download link only where there is a known one; no vague pointers
		const where = h("p", {}, t("flash.codec.stock_files", { files: S.board.codec.sources.join(", ") }),
			S.radio.flash.stock_download ? [" ", h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.codec.stock_download"))] : null);
		const parts = [h("p", {}, t("flash.codec.lead"))];
		const viaStub = stubRoute();
		if (viaStub) {
			parts.push(h("div", { class: "notice ok" }, h("strong", {}, t("flash.f4.codec_title")),
				h("p", {}, t("flash.f4.codec_text"))));
		} else if (needs.length) {
			parts.push(h("div", { class: "notice action" }, h("strong", {}, t("flash.codec.action_title")),
				needs.length > 1 ? h("ul", {}, needs.map((n) => h("li", {}, n))) : h("p", {}, needs[0]), where, picker, loaded));
			if (!S.decision && S.now?.kind !== "rfoundry") parts.push(h("p", { class: "small muted" }, t("flash.codec.or_connect")));
		} else if (!S.decision) {
			parts.push(notice(err, "bad"));
		} else {
			parts.push(h("div", { class: "notice ok" }, h("strong", {}, t(haveStock && S.stock ? "flash.codec.ready_title" : "flash.codec.none_title")),
				h("p", {}, plan), codecLine ? h("p", {}, codecLine) : null, keyLine ? h("p", {}, keyLine) : null, loaded));
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
		forward(button(t("flash.nav.to_write"), next, { primary: true, disabled: !(ready || viaStub) }));
	};
	async function useStock(f) {
		S.stock = await pickedStock(f);
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

// The bootloader panel: instructions, then one button that connects and, once the radio is found, installs.
// makePlanFn returns the plan once the key is known. what: the flash log's description of this write (task, source,
// radio, firmware, variant, features, build). inline: the button sits in the panel (it is the folded alternative
// under another route), not in the nav bar.
function bootloaderPanel(title, makePlanFn, onDone, what, { inline = false } = {}) {
	const box = h("section", { class: "panel" });
	const log = logBox();
	const prog = progressBar();
	const status = h("div");
	let plan = null, transport = null;
	const place = (btn) => (inline ? slot.replaceChildren(btn) : S.forward(btn));
	const slot = h("div", { class: "actions" });
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
				place(write);
			} catch (e) {
				status.replaceChildren(...h("div", {}, notice(errorText(e), "bad"),
					onWindows() && usbSupported() ? windowsDriverHelp({ failed: true }) : null).childNodes);
				winHelp?.replaceChildren();
			}
		}, { primary: true });
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
				status.replaceChildren(notice(t("flash.write.failed", { error: errorText(e) }), "bad"), helpLink());
				write.disabled = false;
				place(connect);
				connect.disabled = false;
			}
		}, { primary: true });
		place(connect);
		// through h(): the DOM's own replaceChildren writes a null as the text "null"
		box.replaceChildren(...h("div", {}, inline ? null : h("h2", {}, title),
			h("h3", {}, t("flash.write.bootloader_title")),
			h("ol", { class: "steps" }, S.radio.flash.bootloader.map((s) => h("li", {}, s)),
				h("li", {}, t("flash.write.then_connect"))),
			S.fakeRadio ? h("p", {}, button(t("flash.sim.enter_bootloader"), () => { S.fakeRadio.enterBootloader(); log.add("(simulated) radio in update mode"); }, { quiet: true })) : null,
			winHelp, inline ? slot : null, fp, status,
			h("details", { class: "small" }, h("summary", {}, t("flash.write.plan", { erase: plan.erase.length, blocks: plan.totalBlocks })), planTable(plan),
				h("pre", { class: "log" }, plan.describe().join("\n")))).childNodes);
	};
	box.append(h("p", { class: "muted" }, t("flash.loading")));
	draw();
	return box;
}

function writeView() {
	if (!S.firmware) {	// the cable route skips the codec step, which otherwise loads it
		ensureFirmware().then(render, (e) => out.replaceChildren(notice(errorText(e), "bad")));
		const out = h("p", { class: "muted" }, t("flash.loading"));
		return out;
	}
	const fw = S.firmware;
	const what = { task: "install", source: fw.origin || "", radio: S.board.id, firmware: String(fw.manifest.version || ""),
		variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null };
	const dfu = (opts) => bootloaderPanel(t("flash.write.title"), async () => {
		// from the cable route the codec step was skipped: decide now, from what the radio reported
		S.decision ||= decideCodec(S.board, fw.manifest, { haveStock: false, haveSaved: !!S.savedCodec, probe: S.now?.probe || null });
		let codec = [], prof = null;
		if (S.decision.mode === MERGE && S.decision.source === "saved") {
			prof = S.savedCodec.profile;
			codec = await codecSegments(prof, S.savedCodec.segments, fb());
		} else if (S.decision.mode === MERGE) ({ profile: prof, segments: codec } = await stockfw.extract(S.stock.data, S.key, fb()));
		return makePlan(S.board, segments(fw.manifest, fw.image), S.decision.mode, codec,
			prof ? stockfw.layoutWindows(prof, fb()) : S.board.codec.windows);
	}, next, what, opts);
	if (!stubRoute()) return dfu();
	// the bootloader route stays one click away: it finishes an install the loader couldn't
	const fallback = h("details", { class: "panel" }, h("summary", {}, t("flash.f4.bootloader")));
	fallback.addEventListener("toggle", () => { if (fallback.open && fallback.children.length < 2) fallback.append(dfu({ inline: true })); });
	return h("div", { class: "flasher" }, stubPanel(fallback, next, what), fallback);
}

// Install through the STM32F4 loader on a radio running RadioFoundry (f4stub.js). fallback: the bootloader route's details,
// opened when the loader can't finish
function stubPanel(fallback, onDone, what) {
	const box = h("section", { class: "panel" });
	const log = logBox();
	const prog = progressBar();
	const status = h("div");
	let stub = null;
	const openFallback = () => { fallback.open = true; };
	const install = async () => {
		const fw = S.firmware;
		let started = null;
		try {
			started = flashStarted(what);
			await stub.configure();	// the radio's board record, its echo checked before any write
			prog.set(0, 1, t("flash.f4.reading"));
			const r = await stub.writeImage(f4Board().owned, segments(fw.manifest, fw.image), {
				// bytes in a rewritten sector that aren't the firmware's (no radio has them since the codec moved): a copy first
				onKept: async (sec) => {
					const file = `${S.board.id}_sector${sec.index}_${hex(sec.addr)}.bin`;
					if (!S.sim) download(file, sec.kept);
					log.add(t("flash.f4.kept", { sector: sec.index, file }));
				},
				progress: (done, todo) => prog.set(done, todo, t("flash.f4.writing", { done, todo })) });
			prog.set(1, 1, t("flash.c7.write.verified"));
			S.stubResult = t(r.written ? "flash.f4.ok" : "flash.f4.same", r);
			log.add(S.stubResult);
			await stub.resetChip();
			try { await stub.s.close?.(); } catch (e) { /* gone */ }
			flashFinished(started);
			onDone();
		} catch (e) {
			flashFinished(started, e);
			const again = button(t("flash.f4.again"), async () => {
				again.disabled = true;
				if (await stub.attach()) {
					status.replaceChildren();
					return install();
				}
				status.replaceChildren(notice(t("flash.f4.failed", { error: t("flash.c7.write.lost") }), "bad"), h("p", {}, t("flash.f4.after_fail")));
				S.forward(null);
			}, { primary: true });
			status.replaceChildren(...h("div", {}, notice(t("flash.f4.failed", { error: errorText(e) }), "bad"),
				h("p", {}, t("flash.f4.after_fail")), helpLink()).childNodes);
			S.forward(again);
			openFallback();
		}
	};
	// the loader came up on a port this page can't open without asking: one click picks it
	const repick = () => {
		const pick = button(t("flash.f4.repick_button"), async () => {
			try {
				stub.s = await SerialStream.request();
				await stub.helloWithin(2000, t("flash.c7.write.lost"));
				stub.attached();
				status.replaceChildren();
				await install();
			} catch (err) {
				status.replaceChildren(notice(t("flash.f4.enter_failed", { error: errorText(err) }), "bad"));
				openFallback();
			}
		}, { primary: true });
		status.replaceChildren(notice(t("flash.f4.repick"), "action"));
		S.forward(pick);
	};
	const go = button(t("flash.f4.go"), async () => {
		go.disabled = true;
		box.append(prog.el, log.el);
		status.replaceChildren();
		try {
			const link = await connectSerial();
			const json = await f4StubJson();
			stub = new F4Stub(link.s, { log: log.add, reopen: reopenRFoundryPort, board: f4Board() });
			S.link = null;		// the port belongs to the loader from here
			await stub.enterFromRFoundry(f4Image(json), (d, n) => prog.set(d, n, t("flash.f4.sending")), { entry: json.run });
		} catch (e) {
			if (stub?.jumped && !S.fakeRadio) return repick();
			await dropSerial();
			status.replaceChildren(notice(t("flash.f4.enter_failed", { error: errorText(e) }), "bad"));
			go.disabled = false;
			S.forward(go);
			openFallback();
			return;
		}
		await install();
	}, { primary: true });
	box.append(h("h2", {}, t("flash.f4.title")), h("p", {}, t("flash.f4.codec_text")), h("p", {}, h("b", {}, t("flash.f4.lead"))),
		h("p", { class: "small muted" }, t("flash.f4.safety")), status);
	S.forward(go);
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// install: after the write

function doneView() {
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const finish = () => button(t("flash.nav.finish"), () => reset(), { primary: true });
	const check = button(t("flash.done.check"), async () => {
		check.disabled = true;
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		await dropSerial();
		try {
			const p = await probe(await connectSerial());
			if (!p) throw new Error(t("flash.err.no_answer"));
			// the loader route never touches the codec, so it is there afterwards as it was before
			const good = !(S.decision ? S.decision.expectCodec : true) || p.codec === S.board.codec.id;
			intro.replaceChildren();	// "the radio restarts" is over: what it says now replaces it
			status.replaceChildren(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, codecText(p.codec))),
			notice(good ? t("flash.done.ready") : t("flash.done.no_codec"), good ? "ok" : "bad"), good ? null : helpLink());
			if (!S.backupDone) {
				S.now = { ...S.now, probe: S.now?.kind === "rfoundry" ? p : S.now?.probe, after: p };
				status.append(backupNow({ when: "after", onDone: () => S.forward(finish()),
					actions: (b) => S.forward([button(t("flash.nav.finish_no_backup"), () => reset(), { quiet: true }), b]) }));
			} else S.forward(finish());
		} catch (e) {
			await dropSerial();
			check.disabled = false;
			status.replaceChildren(connectError(e), serialHelp(), helpLink());
		}
	}, { primary: true });
	S.forward(check);
	const intro = h("div", {}, S.stubResult ? notice(S.stubResult, "ok") : null, h("p", {}, t("flash.done.lead")));
	box.append(panel(t("flash.done.title"), intro, status));
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// backup task

function backupView() {
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const connect = button(t("flash.backup.connect"), async () => {
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.err.no_answer"));
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			S.now = await rfoundryNow(link, p);
			status.replaceChildren(h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, p.codec || "?")),
			h("details", { class: "small" }, h("summary", {}, t("flash.backup.add_stock")),
				filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); })),
			backupNow({ when: "task", actions: (b) => S.forward(b),
				onDone: () => S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true })) }));
		} catch (e) {
			await dropSerial();
			status.replaceChildren(connectError(e), serialHelp());
		}
	}, { primary: true });
	// Connect is the step's action until the radio answers; then the backup's own run button takes its place
	S.forward(connect);
	box.append(panel(t("flash.backup.task_title", { name: S.radio.name }), h("p", {}, t("flash.backup.task_lead")),
		S.radio.backup.stock === "none" ? notice(t("flash.backup.stock_none"), "info") : null, status));
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
	if (!S.restoreTarget) S.restoreTarget = avail.includes(S.restoreHint) ? S.restoreHint : avail[0] || null;
	const summary = h("dl", { class: "kv-list" },
		h("dt", {}, t("flash.now.board")), h("dd", {}, meta.radio.name),
		h("dt", {}, t("flash.restore.taken")), h("dd", {}, localTime(meta.created)),
		meta.radio.chip ? [h("dt", {}, t("flash.now.uid")), h("dd", {}, meta.radio.chip.uid)] : null,
		h("dt", {}, t("flash.restore.fw_now")), h("dd", {}, describeFirmware(meta.firmware_now)),
		meta.firmware_before ? [h("dt", {}, t("flash.restore.fw_before")), h("dd", {}, describeFirmware(meta.firmware_before))] : null,
		h("dt", {}, t("flash.restore.parts")), h("dd", {}, partNames(b.parts)));
	const choices = h("div", { class: "choice-grid" }, ["rfoundry", "codeplug", "stock"].map((tg) =>
		h("button", { class: "choice", type: "button", disabled: !avail.includes(tg), "aria-pressed": String(S.restoreTarget === tg),
			onclick: () => { S.restoreTarget = tg; render(); } },
		h("strong", {}, t(`flash.restore.to_${tg}`)), h("span", { class: "small muted" }, t(`flash.restore.to_${tg}_text`)))));
	const parts = [panel(t("flash.restore.what"), summary, choices)];
	// the stock file: the application to write back, or (any target) where the bootloader key comes from
	if (S.restoreTarget === "codeplug") parts.push(afterRestore());
	else if (S.restoreTarget && !S.stock && (S.restoreTarget === "stock" || (!S.sim && !S.key && !savedKey(S.board))))
		parts.push(panel(t("flash.codec.stock_title"), h("p", {}, t("flash.restore.stock_needed")),
			filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); render(); })));
	else if (S.restoreTarget) {
		parts.push(bootloaderPanel(t("flash.restore.write_title"), async (key) =>
			(S.restoreTarget === "rfoundry" ? planInternal(b, S.board) : planStock(S.stock.data, key, S.board, fb())), () => {
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
		S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		return panel(t("flash.restore.done_stock_title"), h("p", {}, t("flash.restore.done_stock")),
			notice(t("flash.restore.spi_note"), "info"));
	}
	// a backup without the memory chip has no channels to put back: the firmware was the whole restore
	if (!S.bundle.parts.get("spi-flash")) {
		S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		return panel(t("flash.restore.done_fw_title"), h("p", {}, t("flash.restore.done_fw")));
	}
	if (codeplugLayout(S.bundle) !== "openfw") {
		S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		return panel(t("flash.restore.done_fw_title"), h("p", {}, t("flash.restore.done_fw_only")),
			notice(t("flash.restore.codeplug_not_rfoundry"), "info"));
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
			await prepareStorage(status);	// the radio refuses every codeplug write until its settings area is RadioFoundry's
			const refused = await writeCodeplug(link, sectors, (d, n) => prog.set(d, n, t("flash.restore.writing_codeplug")));
			status.append(notice(refused.length ? t("flash.restore.codeplug_refused", { n: refused.length, total: sectors.length })
				: t("flash.restore.codeplug_done", { n: sectors.length }), refused.length ? "" : "ok"));
			S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		} catch (e) {
			status.append(connectError(e), serialHelp());
			go.disabled = false;
		}
	}, { primary: true });
	S.forward(go);
	return panel(t("flash.restore.codeplug_title"), h("p", {}, t("flash.restore.codeplug_lead")), status);
}

// ---------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------
// help: a radio that isn't right after an install. One question at a time, every answer a click, ending in a way
// back: install again, the user's backup, or the original firmware. The page never writes the part of the radio that
// starts update mode (the STM32 radios' bootloader sectors, the HR_C7000's boot ROM and bootloader), so a radio can
// always be put back, and every step says so: someone whose radio just went dark is scared it's ruined.

const SYMPTOMS = ["dark", "stuck", "stopped", "novoice", "channels", "revert"];
// "iap": a white, backlit screen and a solid red LED at every power-on. Only for a radio whose backend has a way back
// through the vendor update mode (recovery.extra); it is listed after the dark screen it is mistaken for
const symptoms = () => (backend().recovery.extra?.(S.radio, fb()).length ? ["dark", "iap", ...SYMPTOMS.slice(1)] : SYMPTOMS);

// From a failure: the radio is known, so the page asks what's wrong right away
function startHelp() {
	dropSerial();
	S.task = "help";
	S.step = S.radio ? 1 : 0;
	S.help = null;
	S.recovering = false;
	render();
	app.scrollIntoView({ behavior: "smooth", block: "start" });
}

// Under every failure: troubleshooting, and the bug report
function helpLink() {
	return h("div", {}, h("p", { class: "help-link" }, t("flash.help.link_lead"), " ",
		h("a", { href: "#", onclick: (e) => { e.preventDefault(); startHelp(); } }, t("flash.help.link"))), reportLine());
}

// What a bug report says about the flasher: no names, callsigns or IDs (automatic error reports carry it too)
function reportContext() {
	const p = S.now?.probe;
	return {
		task: S.task, step: stepsFor()?.[S.step] ?? S.step, radio: S.radio?.id || null, board: S.board?.id || null, sim: !!S.sim,
		version: VERSION_REF || null,
		firmware: S.firmware ? { source: S.firmware.source, version: S.firmware.manifest?.version, git: S.firmware.manifest?.git } : null,
		now: S.now ? { kind: S.now.kind, version: S.now.version || null, board: p?.board, git: p?.git, features: p?.features, codec: p?.codec } : null,
	};
}

function reassure() {
	return h("div", { class: "notice ok calm" }, h("p", {}, h("b", {}, t("flash.help.calm_title"))), h("p", {}, t("flash.help.calm_text")));
}

// One card per answer; picking one moves on
function answers(items, pick, chosen = null) {
	return h("div", { class: `choice-grid${items.length % 3 === 0 ? " three" : ""}` }, items.map(([id, title, text]) =>
		h("button", { class: "choice", type: "button", "aria-pressed": String(chosen === id), onclick: () => pick(id) },
			h("strong", {}, title), text ? h("span", { class: "small muted" }, text) : null)));
}

function symptomView() {
	S.forward(null);
	return h("div", { class: "flasher" }, reassure(),
		panel(t("flash.help.symptom_title"), answers(symptoms().map((id) => [id, t(`flash.help.s.${id}`), t(`flash.help.s.${id}_text`)]),
			(id) => { S.help = { symptom: id, stage: id === "dark" ? "battery" : id === "iap" ? "fix" : ["stuck", "stopped"].includes(id) ? backend().recovery.first : "fix" }; next(); })));
}

function recoverView() {
	const st = S.help;
	const go = (stage, more = {}) => { S.help = { ...st, ...more, stage }; render(); };
	S.forward(null);
	let body;
	if (st.stage === "battery") {
		body = panel(t("flash.help.battery_title"), h("p", {}, t("flash.help.battery_text")),
			answers([["ok", t("flash.help.works_now")], ["no", t("flash.help.still_dark")]], (a) => go(a === "ok" ? "fixed" : backend().recovery.first)));
	} else if (st.stage === "updatemode") body = updateModeCheck(go);
	else if (st.stage === "nohope") body = noResponse(go);
	else if (st.stage === "fix") body = fixChooser(st, go);
	else if (st.stage === "stock") body = stockRecovery(go);
	else if (st.stage === "fixed") {
		S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		return h("div", { class: "flasher" }, panel(t(st.how === "stock" ? "flash.help.stock_done_title" : "flash.help.fixed_title"),
			h("p", {}, t(st.how === "stock" ? "flash.help.stock_done_text" : "flash.help.fixed_text"))));
	}
	// Once the radio answered, the fix chooser says so itself
	return h("div", { class: "flasher" }, st.fine && st.stage !== "stock" ? null : reassure(), body);
}

// STM32 radios: put it in update mode and let the page find it there. A radio that answers is fine.
function updateModeCheck(go) {
	const status = h("div");
	const giveUp = button(t("flash.help.no_response"), () => go("nohope"), { quiet: true });
	const check = button(t("flash.help.um_check"), async () => {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			S.dfuReady = await connectBootloader();
			go("fix", { fine: true });
		} catch (e) {
			status.replaceChildren(notice(t("flash.help.um_not_found"), "bad"),
				h("ul", {}, h("li", {}, t("flash.help.hint_battery")), h("li", {}, t("flash.help.hint_cable")), h("li", {}, t("flash.help.hint_keys"))),
				onWindows() && usbSupported() ? windowsDriverHelp({ failed: true }) : null);
		}
	}, { primary: true });
	S.forward([giveUp, check]);
	return panel(t("flash.help.um_title"), h("p", {}, t("flash.help.um_lead")),
		h("ol", { class: "steps" }, S.radio.flash.bootloader.map((x) => h("li", {}, x)), h("li", {}, t("flash.help.um_then"))),
		S.fakeRadio ? h("p", {}, button(t("flash.sim.enter_bootloader"), () => S.fakeRadio.enterBootloader(), { quiet: true })) : null, status);
}

function noResponse(go) {
	const repo = S.data.site.repo;
	S.forward(button(t("flash.help.try_again"), () => go(backend().recovery.first), { primary: true }));
	return panel(t("flash.help.nohope_title"), h("p", {}, t("flash.help.nohope_lead")),
		h("ol", { class: "steps" }, h("li", {}, t("flash.help.nohope_battery")), h("li", {}, t("flash.help.hint_cable")),
			h("li", {}, t("flash.help.nohope_port")), h("li", {}, t("flash.help.nohope_again"))),
		repo ? h("p", {}, t("flash.help.ask"), " ", h("a", { href: `${repo}/issues`, target: "_blank", rel: "noopener" }, t("flash.help.ask_link"))) : null);
}

// The ways back, each saying what it needs. Picking one and pressing Continue starts it.
function fixChooser(st, go) {
	const rec = backend().recovery;
	const files = S.board?.codec?.sources?.join(", ") || "";
	const opts = {
		again: [t("flash.help.fix.again"), t("flash.help.fix.again_text")],
		codec: [t("flash.help.fix.codec"), t("flash.help.fix.codec_text", { files })],
		backup: [t("flash.help.fix.backup"), t(st.symptom === "channels" ? "flash.help.fix.backup_channels" : "flash.help.fix.backup_text")],
		stock: [t("flash.help.fix.stock"), t(rec.stockText, { files })],
	};
	// a backend's own ways back (the DB25-D's update mode) come after the reinstall
	const extra = rec.extra?.(S.radio, fb()) || [];
	for (const x of extra) opts[x.id] = [t(x.title), t(x.text)];
	const order = { novoice: ["codec", "backup", "stock"], channels: ["backup"], revert: ["backup", "stock"],
		iap: ["updatemode", "backup"] }[st.symptom]
		|| ["again", ...extra.map((x) => x.id), "backup", "stock"];
	const start = () => {
		const pick = st.pick;
		const own = extra.find((x) => x.id === pick);
		if (pick === "stock" && rec.stock === "file") return go("stock");
		if (pick === "again" || pick === "codec" || own) {
			S.task = "install";
			S.recovering = true;
			S.step = 0;
			S.now = { kind: "unknown" };
			S.requested = pick === "codec" ? MERGE : S.board.keep_codec_supported ? KEEP : null;
			own?.prepare();
		} else {
			S.task = "restore";
			S.step = 0;
			S.restoreHint = pick === "backup" ? (st.symptom === "channels" ? "codeplug" : "rfoundry") : "stock";
		}
		render();
	};
	S.forward(button(t("flash.help.continue"), start, { primary: true, disabled: !st.pick }));
	return panel(t(st.fine ? "flash.help.fine_title" : "flash.help.fix_title"),
		st.symptom === "iap" ? stuckInUpdateMode() : st.fine ? h("p", {}, t("flash.help.fine_text")) : rec.reach ? h("p", {}, t(rec.reach)) : null,
		answers(order.map((id) => [id, ...opts[id]]), (id) => go("fix", { pick: id }), st.pick));
}

// A radio that shows a white screen and a red LED at every power-on is in the vendor update mode because its application
// is missing or incomplete: say so, and how to get into update mode if the radio is not showing it
function stuckInUpdateMode() {
	return h("div", {}, h("p", {}, t("flash.help.iap_cause")), h("p", {}, t("flash.help.iap_nokeys")),
		h("p", { class: "small" }, t("flash.help.iap_enter")),
		h("ol", { class: "steps small" }, S.radio.flash.bootloader.map((x) => h("li", {}, x))),
		h("p", { class: "small muted" }, t("flash.help.iap_original")));
}

// STM32 radios back on their original firmware, from the stock file, through update mode (no backup needed)
function stockRecovery(go) {
	if (!S.stock?.info?.known) {
		const picker = S.sim ? button(t("flash.sim.use_stock"), async () => { S.stock = { name: "simulated-stock.bin", data: S.sim.stock, info: await stockfw.identify(S.sim.stock, fb()) }; render(); })
			: filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); render(); });
		const where = S.radio.flash.stock_download ? h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.codec.stock_download")) : null;
		return panel(t("flash.help.stock_title"), h("p", {}, t("flash.help.stock_need", { files: S.board.codec.sources.join(", ") }), " ", where),
			S.stock ? notice(t("flash.codec.stock_loaded", { name: S.stock.name, what: t("flash.codec.stock_unknown") }), "bad") : null, picker);
	}
	return bootloaderPanel(t("flash.help.stock_title"), async (key) => planStock(S.stock.data, key, S.board, fb()), () => {
		if (S.fakeRadio) S.fakeRadio.running = "stock";
		go("fixed", { how: "stock" });
	}, { task: "restore", source: "stock", radio: S.board.id, firmware: "stock" });
}

async function main() {
	initReporting({ tool: "flasher", context: reportContext });
	if (!FLASH_LOG) document.getElementById("flash-log-off")?.replaceWith(t("flash.log.is_off"));
	// the privacy contact is put together here, so address scrapers don't find it in the page or the repository
	const mail = ["tseuqer-ycavirp-rdo", "moc.relwofrecneps"].map((x) => [...x].reverse().join("")).join("@");
	const a = document.getElementById("privacy-mail");
	if (a) { a.href = `mailto:${mail}`; a.textContent = mail; }
	try {
		S.data = await loadData();
	} catch (e) {
		diag.error("load data", e);
		app.replaceChildren(notice(t("flash.err.load"), "bad"), reportLine());
		return;
	}
	c7ui.init({ S, next, render, reset, helpLink, fb, flashStarted, flashFinished, errorText, analysisOffer,
		stubJson: () => fetch(`${ROOT}data/c7000-stub.json`).then((r) => r.json()) });
	// #sim=<board>[-stock]: that board, running RadioFoundry or its stock firmware; #sim: the first radio on stock firmware
	const simAsk = location.hash.match(/(?:^#|&)sim(?:=(\w+?)(-stock)?)?(?:&|$)/);
	const simRadio = simAsk && (simAsk[1] ? S.data.radios.find((r) => r.id === simAsk[1]) : S.data.radios[0]);
	const simStock = !!simAsk && (!simAsk[1] || !!simAsk[2]);
	if (simRadio && c7ui.isC7000(simRadio, S.data.fb)) {
		// on its cable: stock firmware (a stock console answers if the board has one), or RadioFoundry with the stub hand-over
		const board = simRadio.id;
		const sim = await c7000Simulation(S.data.fb.c7000, board, 7000, simRadio);
		S.sim = { fb: { ...S.data.fb, c7000: sim.c } };
		S.fakeC7 = new FakeC7000({ progmem: sim.progmem, speed: 8, stubEntry: true, romWindow: 600, faults: { silentAfterReset: true },
			...fakeC7000Options(simRadio, S.data.fb) });
		if (!simStock) {
			const img = randomBytes(rng(98), 180 * 1024);
			img.set(new TextEncoder().encode("RadioFoundry"), 0x1000);
			S.fakeC7.progmem.set(img, sim.c.app_start);
			S.fakeC7.progmem.set(slotBytes(sim.c, board, await locateCodec(sim.progmem, sim.c, board)), sim.c.boards[board].slot[0]);
		}
		S.fakeC7.powerOn();
	} else if (simRadio) {
		S.sim = await simulation(S.data.fb, simRadio.id);
		S.fakeRadio = new FakeRadio(S.sim, { state: simStock ? "stock" : "rfoundry", spiSize: 0x400000 });
		// a smaller SPI flash keeps the simulated backup quick
		simRadio.backup = { ...simRadio.backup, spi_size: 0x400000 };
	}
	S.backupParts = [];
	const m = location.hash.match(/radio=(\w+)/);
	const r = m && S.data.radios.find((x) => x.id === m[1]);
	if (r && available(r, "install")) {
		S.task = "install";
		selectRadio(r);
		S.step = 1;
	}
	render();
}

main();
