// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher: a step-by-step page over the modules beside it. Three tasks:
//
//   update    a radio that runs RadioFoundry: connect once, keep its build (or edit it), write the current release
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
import { unzip } from "./zip.js";
import * as stockfw from "./stockfw.js";
import { openVendorFile, describe as describeVendorFile, analysisCandidate, offerFile, logId, whyNot } from "./vendorfile.js";
import { pickProblem, errorProblem, vendorContext, supportedFiles, foldSupported, keepNote, nowFor, nowChoice, OFFER } from "./explain.js";
import { makePlan, KEEP, MERGE } from "./plan.js";
import { radioPicture, holdKeys } from "./radiotop.js";
import { validRef, refShort, defaultRef, firmwareName, betaLink } from "./version.js";
import { verify, segments, forDevBuild } from "./manifest.js";
import { Dfu, DfuError, WebUsbTransport, checkKey, usbSupported } from "./dfu.js";
import { Link, LinkError, SerialStream, RFOUNDRY_USB, RFOUNDRY_NET, probe, whyNoAnswer, noAnswerKey, serialSupported, browserErrorKey, knownRadioPort } from "./link.js";
import { readRadio, makeBundle, openBundle, bundleFolder, bundleName, describeFirmware, CODEPLUG_SPAN } from "./backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug, codeplugLayout } from "./restore.js";
import { Selection } from "./select.js";
import { decideCodec, DecisionError } from "./decide.js";
import { tooBig } from "./buildfit.js";
import { suggestPack, packCode, startCode, startLanguages, startLanguage, setStartLanguage } from "./langsuggest.js";
import { readBootKey } from "./bootkey.js";
import { codecSegments, loadCodec, readCodec, saveCodec } from "./codecsave.js";
import { FakeRadio, simulation, randomBytes, rng } from "./fake.js";
import { t, tOr, num, LANG, ROOT, h, download, filePicker, progressBar, stopBars, logBox, notice, button, spinner, panel, localTime, partNames } from "./dom.js";
import * as c7ui from "./c7000ui.js";
import { variantWarning, variantVerdict } from "./iapui.js";
import { groups as variantGroups } from "./variant.js";
import { FakeC7000, c7000Simulation, fakeC7000Options } from "./fakec7000.js";
import { APP_ENTRY } from "./iap.js";
import { slotBytes, locateCodec } from "./c7000.js";
import { Stub, StubError } from "./stub.js";
import { F4Stub, f4Image } from "./f4stub.js";
import { f4LoaderReader } from "./readers.js";
import * as openfw from "./openfw.js";
import { register, backendOf, bundleBackend, allBackends } from "./backends.js";
import * as diag from "../diag.js";
import { initReporting, reportLine, reportingActive, errorNotice, reportButton, setAdapter } from "../report.js";
import * as adapter from "./adapter.js";
import * as crumbs from "./trail.js";
import { missingApi, kitPanel, kitLine } from "./manual.js";
import { showBetaNotice } from "./beta.js";
import { currentBuild, planUpdate } from "./update.js";

// ---------------------------------------------------------------------------------------------------------------
// strings and DOM

const app = document.getElementById("flasher");

class NetworkModeError extends Error {}
// A refusal from the backend (its "detail", or a build's error): a feature or option the build doesn't know means
// the page and the version it builds are out of step
class ApiError extends Error {}
// a build the service couldn't fit in the radio (buildfit.js): told as what to change, not as a fault to report
class FitError extends Error {
	constructor(fit) { super(`too big: ${fit.region}`); this.fit = fit; }
}

// Every error shown goes through here: it is logged once (console, bug report) with the step it happened in. The
// browser's own USB and serial errors say little ("Failed to open serial port."): they get what to do, with the
// browser's words after it for a bug report
function errorText(e) {
	diag.error(`${S.task || "start"}/${stepsFor()?.[S.step] ?? S.step}`, e);
	const raw = e?.message || String(e);
	if (e instanceof DecisionError && e.key) return said(e.key, e.args) + (e.hint ? " " + t("flash.codec.why_not.hint") : "");
	const withRaw = (key) => `${t(key)} (${raw})`;
	if (e instanceof NetworkModeError) return t("flash.net.found");
	const browser = browserErrorKey(e);
	if (browser) return ["flash.err.no_device", "flash.err.security", "flash.err.gesture"].includes(browser) ? t(browser) : withRaw(browser);
	if (e instanceof DfuError && /^USB transfer/.test(raw)) return withRaw("flash.err.connection_lost");
	if (e instanceof LinkError && /^no answer/.test(raw)) return withRaw("flash.err.no_answer");
	if (e instanceof StubError && /not acknowledged/.test(raw)) return withRaw("flash.err.rom_no_ack");
	if (e instanceof ApiError && /^unknown (feature|option)/i.test(raw)) return withRaw("flash.err.build_mismatch");
	// fetch's own failure: offline, blocked, or the server unreachable
	if (e instanceof TypeError && /fetch/i.test(raw)) return withRaw("flash.err.download");
	return raw;
}

// A catalog string whose args may themselves be catalog strings ({key, args}: decide.js)
function said(key, args = {}) {
	return t(key, Object.fromEntries(Object.entries(args).map(([k, v]) => [k, v?.key ? said(v.key, v.args) : v])));
}

// A language's name in the page's language ("es": "Spanish" on an English page), else its code
function languageName(code) {
	try {
		return new Intl.DisplayNames([LANG], { type: "language" }).of(code) || code;
	} catch (e) {
		return code;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// state

// A fresh start; reset() puts these back (a Set, an array) rather than nulling them
const initialState = () => ({
	task: null, step: 0, data: null, radio: null, board: null, selection: null, sim: null, fakeRadio: null,
	firmware: null,		// {manifest, image, source}
	now: null,		// what runs on the radio: {kind: "rfoundry", probe} | {kind: "stock", version} | {kind: "unknown"}, with
			// openfw: true when the user chose "Another open firmware" (explain.js nowFor), plus radioType and git
			// once a probe recognized it (openfwRecognized): then the step backs it up (openfw.js)
	openfwRead: null,	// another open firmware's backup (openfw.readBackup), kept so a stopped read continues
	openfwFound: null,	// what it found: {key, codec} (booleans; S.key and S.savedCodec hold them)
	openfwSaved: null,	// the bundle it saved: {name, size}
	openfwAbort: null,	// stops a backup that is reading
	stock: null,		// {name, data, info: identify()}
	readout: null,		// the internal flash read from the radio, for the flash log: {len, sha256, id} (noteReadout)
	key: null,
	keySource: null,	// where the key came from (getKey), for error reports
	vendorPick: null,	// the last vendor file picked and what was found in it (pickedStock), for error reports
	decision: null,
	backupParts: [], backupDone: false,
	bundle: null, restoreTarget: null,
	link: null,
	packsDismissed: new Set(),	// language packs the user said no to on the build step (langsuggest.js)
	startLang: null,	// the Radio language picked on the build step; null: on an installation the page's (langsuggest.js)
	langAdded: null,	// the language pack that pick put in the build, taken out again when the pick changes
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
	if (!r.ok) throw new ApiError(typeof j.detail === "string" ? j.detail : `HTTP ${r.status}`);
	return j;
}

// "Send this file for further analysis": a vendor firmware file the page couldn't take the codec from, or failed on after
// identifying it, goes to the backend (POST /v1/samples: vendor containers only, size and rate limited, never served
// back), so its build can be mapped or the fault reproduced. Only on the user's click, with what was found; nothing
// from the radio. problem (explain.js): what went wrong; for a fault in the page its message goes with the user's note.
// Not offered in a simulation
function analysisOffer(cand, boardId, problem = null) {
	if (!cand || S.sim) return null;
	const internal = problem?.problem === "internal";
	const dump = problem?.problem === "dump";
	const status = h("p", { class: "small" });
	const note = h("textarea", { class: "analysis-note", rows: 2, maxlength: 400, placeholder: t("flash.analysis.note") });
	const send = button(t("flash.analysis.send"), async () => {
		send.disabled = true;
		status.textContent = t("flash.analysis.sending");
		const page = internal && problem.raw ? ` [page: ${problem.raw}]` : "";
		const q = new URLSearchParams({ board: boardId, name: cand.file.path?.split("/").pop() || cand.file.name || "", reason: cand.reason,
			version: S.data.site.version || "", note: (note.value.trim() + page).slice(0, 500) });
		try {
			const r = await fetch(new URL(`samples?${q}`, API), { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: cand.file.data });
			const j = await r.json().catch(() => ({}));
			// FastAPI's own validation errors carry a list, the backend's refusals a string
			if (!r.ok) throw new Error(typeof j.detail === "string" ? j.detail : `HTTP ${r.status}`);
			status.textContent = t(j.status === "already-have" ? "flash.analysis.have" : "flash.analysis.sent");
		} catch (e) {
			status.textContent = t("flash.analysis.failed", { error: e.message });
			send.disabled = false;
		}
	});
	const name = cand.file.path?.split("/").pop() || cand.file.name || "";
	return h("div", { class: "notice" }, h("strong", {}, t(dump ? "flash.analysis.title_dump" : "flash.analysis.title")),
		h("p", {}, t(internal ? "flash.analysis.text_internal" : dump ? "flash.analysis.text_dump" : "flash.analysis.text", { name })), note, send, status);
}

const radioName = (id) => fb().boards[id]?.name || S.data?.radios?.find((r) => r.id === id || r.flash?.board === id)?.name || id || "";

// Why a picked vendor file can't be used (explain.js): what happened, why, what to do, the exact message folded away,
// and the send offer where the file can help. offer: the analysis candidate for this problem. A build without a codec
// map also lists every supported file to select instead (in full: the user is choosing one), folded when there are many
// (explain.js foldSupported), and in a sentence of its own what else the codec can come from (a program flash read)
function problemNotice(p, { offer = null, boardId = S.board?.id } = {}) {
	S.vendorPick = { ...(S.vendorPick || {}), problem: p };
	const vars = { file: p.file || t("flash.problem.this_file"), radio: radioName(boardId), other: radioName(p.board),
		what: p.known || t("flash.codec.stock_build", { radio: radioName(boardId) }) };
	const internal = p.problem === "internal";
	const unmapped = p.problem === "unmapped";
	// a dump of this radio's firmware that has no map yet: it restores, and the same supported files give the codec
	const dump = p.problem === "dump";
	const { files, also } = unmapped || dump ? supportedFiles(fb(), boardId) : { files: [], also: [] };
	const download = dump && S.radio?.flash.stock_download
		? h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.problem.dump.download")) : null;
	const list = h("ul", { class: "supported-files" }, files.map((n) => h("li", {}, n)));
	return h("div", { class: "notice bad problem" },
		h("strong", {}, t(`flash.problem.${p.problem}.title`, vars)),
		h("p", {}, t(`flash.problem.${p.problem}.text`, vars), internal ? [" ", t(reportingActive() ? "flash.problem.internal.reported" : "flash.problem.internal.report")] : null,
			unmapped && p.key ? [" ", t("flash.problem.unmapped.key")] : null),
		p.raw ? h("details", { class: "small" }, h("summary", {}, t("flash.problem.details")), h("pre", { class: "log" }, p.raw)) : null,
		OFFER.has(p.problem) ? analysisOffer(offer, boardId, p) : null,
		files.length ? h("p", {}, t(dump ? "flash.problem.dump.instead" : "flash.problem.unmapped.instead", vars), download ? [" ", download] : null) : null,
		!files.length ? null : dump || foldSupported(files)
			? h("details", {}, h("summary", {}, t("flash.problem.unmapped.all", { n: files.length })), list) : list,
		also.length && !dump ? h("p", {}, t(files.length ? "flash.problem.unmapped.also" : "flash.problem.unmapped.only", { what: also.join("; ") })) : null,
		reportLine());	// a file the page can't use may be the page's fault: the user can send it with the report
}

// An error from a step that uses a vendor file: its problem explained when it is one (logged and reported like every
// error), else the plain message. file: the vendor file in use ({name, data}), offered when the fault is the page's
function failureNotice(e, file = null) {
	const text = errorText(e);
	const p = errorProblem(e);
	if (!p) return errorNotice(text);
	return problemNotice({ ...p, file: p.file || file?.name || null },
		{ offer: file ? offerFile({ path: file.name, data: file.data }, fb(), p.problem) : null });
}

// A radio that didn't answer the probe: the message, and what the port gave meanwhile for the report (link.js
// whyNoAnswer(): nothing, another open firmware that gave its radio info, or another firmware's bytes). written: the
// manifest of the build just written, so that a RadioFoundry build without the debug link is named as that
async function noAnswer(link, { written = null } = {}) {
	const why = await whyNoAnswer(link, { written });
	const key = noAnswerKey(why);
	const e = new Error(t(key));
	e.key = key;	// the report groups by it, not by the translated message (report.js captureOptions)
	e.params = why;
	return e;
}

// One id per use: the custom build requests and the write that follows share it; each write ends the use
const useId = () => (FLASH_LOG && !S.sim ? (S.useId ||= crypto.randomUUID()) : undefined);
// The browser's language preferences, for the flash log only (the backend keeps at most 10 tags of 35 characters)
const browserLanguages = () => (navigator.languages?.length ? [...navigator.languages].slice(0, 5) : (navigator.language ? [navigator.language] : []));

// The vendor firmware involved in this use, as the flash log takes it: lengths, SHA-256 and a short id, never bytes.
// stock: the file used (the inner one for a zip); readout: the internal flash read from the radio
function stockLog() {
	const o = {};
	if (S.stock?.data && S.stock.info?.sha) Object.assign(o, { stock_len: S.stock.data.length, stock_sha256: S.stock.info.sha, stock_id: logId(S.stock) });
	if (S.readout) Object.assign(o, { readout_len: S.readout.len, readout_sha256: S.readout.sha256, readout_id: S.readout.id });
	return o;
}

// Remember an internal-flash readout for the flash log. A failure to hash never stops a backup
async function noteReadout(data) {
	try {
		const info = await stockfw.identify(data, fb());
		S.readout = { len: data.length, sha256: info.sha, id: (info.known ? `known:${info.known}` : S.now?.kind || "unidentified").slice(0, 64) };
		S.readoutBytes = data;
	} catch (e) { diag.error("flash/readout hash", e); }
}

// Fire and forget: a flash never waits for these or fails because of them. The vendor firmware known by now goes along
function flashStarted(what) {
	diag.note("flash started", what);
	const id = useId();
	if (!id) return null;
	S.useId = null;
	const sent = fetch(new URL("flashes", API), { method: "POST", keepalive: true, headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ id, tool: "web", tool_version: S.data.site.version || "", languages: browserLanguages(), ...stockLog(), ...(S.adapter ? { adapter: S.adapter } : {}), ...what }) }).catch(() => {});
	S.lastFlashId = id;
	return (S.lastStarted = { id, sent, marked: new Set() });
}

// A milestone of the use: first_byte, last_byte (data on the radio) or verify (the check after, with its outcome). Each is
// sent once; fire and forget like the rest
async function flashMark(started, event, outcome = null) {
	if (!started || started.marked.has(event)) return;
	started.marked.add(event);
	diag.note("flash " + event, outcome);
	await started.sent;
	fetch(new URL(`flashes/${started.id}/events`, API), { method: "POST", keepalive: true, headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ event, ...(outcome ? { result: outcome.ok ? "success" : "fail", reason: (outcome.reason || "").slice(0, 300) } : {}) }) }).catch(() => {});
}

async function flashFinished(started, error = null) {
	stopBars();	// a bar left short of 100% by a failure or a cancel stops saying it waits
	if (error) errorText(error);
	else diag.note("flash finished");
	if (!started) return;
	await started.sent;
	// the readout may have come after the start (the backup task), so the end carries it too
	const body = { ...stockLog(), ...(error ? { result: "fail", reason: errorText(error).slice(0, 300) } : { result: "success" }),
		...(S.adapter ? { adapter: S.adapter } : {}), trail: diag.trail(200).map((l) => diag.scrub(l).slice(0, 300)) };
	// a keepalive request carries 64 KB at most: the oldest steps go first
	let json = JSON.stringify(body);
	while (new TextEncoder().encode(json).length > 60000 && body.trail.length) {
		body.trail.splice(0, 20);
		json = JSON.stringify(body);
	}
	fetch(new URL(`flashes/${started.id}`, API), { method: "PATCH", keepalive: true, headers: { "Content-Type": "application/json" },
		body: json }).catch(() => {});
}

const STEPS = {
	install: ["radio", "build", "now", "codec", "write", "done"],
	backup: ["radio", "backup"],
	restore: ["bundle", "restore"],
	help: ["radio", "symptom", "recover"],
};
// An update before the radio is known; its backend then says how it is reached (backends.js)
STEPS.update = ["update", "write", "done"];
// A radio running RadioFoundry that updates over the cable has no codec decision to make: the step goes
const STUB_INSTALL = ["radio", "build", "now", "write", "done"];
// Installing again from the help task: what runs on the radio is known to be broken, so that question goes
const RECOVER_INSTALL = ["build", "codec", "write", "done"];
// The radio's backend (backends.js) decides a task's steps; the shared ones otherwise. An update whose build the user
// edits (or whose build the radio doesn't say) takes the build step after its first
function stepsFor() {
	const s = (S.radio && backendOf(S.radio).steps(S.task, { recovering: !!S.recovering })) || STEPS[S.task];
	return S.task === "update" && S.editBuild ? [s[0], "build", ...s.slice(1)] : s;
}
const backend = () => backendOf(S.radio);

async function loadData() {
	const get = async (name) => (await fetch(`${ROOT}data/${name}`)).json();
	const [radios, features, variants, fb, releases, site] = await Promise.all(
		["radios.json", "features.json", "variants.json", "flash-boards.json", "releases.json", "site.json"].map(get));
	// repository texts in the page's language where the catalog overrides them (feature.<SWITCH>.*, variant.<id>.*)
	const feats = features.features.map((f) => ({ ...f, title: tOr(`feature.${f.name}.title`, f.title), help: tOr(`feature.${f.name}.help`, f.help) }));
	const vars = variants.variants.map((v) => ({ ...v, title: tOr(`variant.${v.id}.title`, v.title), summary: tOr(`variant.${v.id}.summary`, v.summary) }));
	return { radios: radios.radios, features: feats, groups: features.groups, reported: features.reported || [],
		variants: vars, fb, releases: releases.releases, site };
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
	// an update writes over the cable when it can, through the bootloader otherwise (its codec decided from the radio)
	steps: (task, { recovering }) => (task === "update" ? STEPS.update : task !== "install" ? null : recovering ? RECOVER_INSTALL
		: stubRoute() ? STUB_INSTALL : null),
	views: { now: nowView, codec: codecView, write: writeView, done: doneView, backup: backupView, restore: restoreView },
	recovery: { first: "updatemode", stock: "file", reach: null, stockText: "flash.help.fix.stock_text" },
});

// The STM32F4 loader route (f4stub.js): the radio runs RadioFoundry with stub entry and its codec, the image leaves the codec
// alone, and nobody asked for a bootloader mode. Everything else, a stock radio first of all, uses the bootloader.
const f4Board = () => fb().f4stub?.boards[S.board?.id];
const stubRoute = () => !!((S.task === "install" || S.task === "update") && f4Board() && S.now?.kind === "rfoundry" && S.now.stubOffer && !S.requested
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
	// a radio switched off, restarted or put in update mode since: its old port is gone, the browser lists it afresh
	if (S.link?.s.lost) await dropSerial();
	if (S.link) return S.link;
	let stream;
	if (S.fakeRadio) stream = S.fakeRadio.serialStream();
	else {
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		if (await networkModeSeen()) throw new NetworkModeError();
		const granted = await grantedRFoundryPorts();
		stream = granted.length ? await SerialStream.open(granted[0]) : await SerialStream.request();
		useAdapter(adapter.fromSerialInfo(stream.port?.getInfo?.()));
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

// Under a failed connect: the cable and driver tips, except where the radio answered with the open-firmware CPS
// (noAnswer kinds "openfw", "rfoundry-nodebug"): the connection worked, so the next step instead. openfw: that line
const ANSWERED = new Set(["openfw", "rfoundry-nodebug"]);
function connectHelp(e, openfw) {
	const kind = e?.params?.kind;
	if (!ANSWERED.has(kind)) return serialHelp();
	return h("p", {}, t(kind === "rfoundry-nodebug" ? "flash.err.rfoundry_nodebug_next" : openfw));
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
	return e instanceof NetworkModeError ? networkModeHelp() : errorNotice(errorText(e));
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
	const tr = await WebUsbTransport.request(S.board);
	useAdapter(adapter.fromUsbDevice(tr.device));
	return tr;
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
	S.keySource = "radio";
	diag.note(...crumbs.key("radio (debug link)"));
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
// opened by vendorfile.js: the best file for this radio, identified, one line per file for the page (report), and
// what is wrong with it when nothing in it serves (explain.js): problem for the codec, keyProblem for a restore of the
// original firmware, which takes any build of this radio. What was picked also goes into error reports (S.vendorPick)
async function pickedStock(f) {
	const picked = { name: f.name, size: f.data.length, sha256: await sha256(f.data) };
	const none = { sha: null, known: null, profile: null };
	try {
		const opened = await openVendorFile(f.name, f.data, fb(), S.board.id);
		const report = describeVendorFile(opened, fb(), S.board.id);
		const b = opened.best;
		const common = { picked, opened, report, analysis: analysisCandidate(opened, fb(), S.board.id),
			problem: pickProblem(opened, S.board.id), keyProblem: pickProblem(opened, S.board.id, { need: "key" }) };
		S.vendorPick = { picked, opened, problem: common.problem };
		const info = b ? await stockfw.identify(b.data, fb()) : none;
		const got = b ? { name: b.path.split("/").pop(), data: b.data, ...common, match: b.match, info } : { ...f, ...common, info };
		diag.note(...crumbs.filePicked(picked, got));
		S.picked = { name: f.name, data: f.data };
		return got;
	} catch (e) {
		// the opener reports what it can't read as a file problem, so this is the page's own fault
		diag.error("flash/open file", e);
		const problem = { problem: "internal", file: f.name, raw: e.message };
		S.vendorPick = { picked, opened: null, problem };
		return { ...f, picked, report: [e.message], problem, keyProblem: problem, info: none };
	}
}

// A stock file for this radio, whichever build: what a restore of the original firmware needs (the key and the
// application), not a codec profile. A flash readout is neither
// A file for another radio that only gives this one's codec (file_radio) is never this radio's firmware
// (nor kept in a backup as its stock firmware, which would offer it for a restore to stock)
const codecOnlyFile = (st) => !!(st?.match?.profile || st?.info?.profile)?.file_radio;
const stockForRadio = (st) => !!st && !codecOnlyFile(st)
	&& (st.match ? st.match.board === S.board.id && st.match.kind !== "readout"
		: !!st.info?.profile && st.info.profile.board === S.board.id);

// S.keySource says where the key came from, for error reports: "radio" (keyFromRadio), "saved", "file" or "sim"
async function getKey(board) {
	if (S.key) return S.key;
	if (S.sim) {
		S.keySource = "sim";
		return (S.key = S.sim.key);
	}
	const saved = savedKey(board);
	if (saved) {
		try {
			S.key = await checkKey(saved, board);
			S.keySource = "saved";
			diag.note(...crumbs.key("saved in this browser"));
			return S.key;
		} catch (e) { /* stale: derive again */ }
	}
	if (!S.stock) throw new Error(t("flash.err.need_stock_for_key"));
	// a file for another radio that gives this one's codec (an MD-9600 file for the DM-1701) never gives the key
	const only = S.stock.match?.profile || S.stock.info?.profile;
	if (only?.file_radio) throw new Error(t("flash.err.codec_only_key", { file: only.name, radio: only.file_radio, board: board.name }));
	// the opener already derived it from a build it had no profile for
	const found = S.stock.match?.board === board.id ? S.stock.match.key : null;
	S.key = await checkKey(found || await stockfw.deriveKey(S.stock.data, board.key_sha256, fb()), board);
	S.keySource = "file";
	diag.note(...crumbs.key(found ? "file (opener)" : "file (derived)"));
	try { localStorage.setItem(KEY_STORE(board.id), toHex(S.key)); } catch (e) { /* not remembered */ }
	return S.key;
}

// ---------------------------------------------------------------------------------------------------------------
// layout

function render() {
	S.openfwAbort?.abort();	// a backup of another open firmware stops when its step is left (openfwBackupNow)
	app.replaceChildren();
	document.querySelector(".page-head")?.classList.toggle("compact", !!S.task);
	if (!S.task) return app.append(...[versionBanner() || betaNotice(), startView()].filter(Boolean));
	const steps = stepsFor();
	const label = (s) => (S.radio && backend().stepLabel?.(s, S.task)) || s;
	const stepper = () => h("ol", { class: "stepper" }, stepsFor().map((s, i) =>
		h("li", { class: i < S.step ? "done" : "", "aria-current": i === S.step ? "step" : false }, t(`flash.step.${label(s)}`))));
	let shown = stepper();
	app.append(shown);
	render.stepper = () => { const n = stepper(); shown.replaceWith(n); shown = n; };
	if (S.sim) app.append(h("p", { class: "small muted sim-line" }, t("flash.sim.banner")));
	if (VERSION_REF && S.task === "install") app.append(versionBanner());
	else if (S.task === "install" && S.step === 0) app.append(...[betaNotice()].filter(Boolean));
	if (S.radio && S.radio.status !== "supported" && S.step > 0) app.append(prereleaseNotice());
	// a hardware variant in development, once the page read it or the user picked it (board.toml [hardware] variants)
	if (S.radio && S.variant && S.step > 0) app.append(...[variantWarning(variantVerdict(S, S.radio.flash.board))].filter(Boolean));
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
	// a step this browser can't run (no WebUSB or Web Serial): the manual install kit in its place
	const missing = S.radio && !S.sim && missingApi(S.radio, S.task, step, { usb: usbSupported(), serial: serialSupported(), stub: stubRoute() });
	const view = (missing && (() => kitView(missing))) || (step === "restore" && bundleBackend(S.bundle)?.views.restore) || (S.radio && backend().views[step])
		|| { update: updateView, radio: radioView, build: buildView, bundle: bundleView, symptom: symptomView, recover: recoverView }[step];
	app.append(view());
	app.append(h("div", { class: "nav-bar" },
		h("div", { class: "nav-back" },
			S.step > 0 ? button("← " + t("flash.back"), () => { S.step--; render(); }) : null,
			h("button", { class: "btn btn-quiet", type: "button", onclick: () => reset() }, t("flash.start_over"))),
		info, fwd));
}

// A pre-release newer than the release (site.json beta, site/cloudflare-build.sh): offered as a version link, never
// installed by default. Not while a version link is in use
function betaNotice() {
	const beta = S.data?.site?.beta, href = betaLink(beta, location.href, VERSION_REF);
	if (!href) return null;
	return h("div", { class: "notice info beta-notice", role: "note" },
		h("p", {}, h("strong", {}, t("flash.beta.title", { ref: beta })), " ", t("flash.beta.text")),
		h("p", {}, h("a", { href }, t("flash.beta.link", { ref: beta }))));
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
	setAdapter(null);	// the next connect names its own
	render();
}

// ---------------------------------------------------------------------------------------------------------------
// start

function startView() {
	// one banner: both APIs missing (Safari, Firefox), or the one that is
	const usb = S.sim || usbSupported(), serial = S.sim || serialSupported();
	const support = !usb && !serial ? [notice(t("flash.support.none"), "bad")]
		: [!usb && notice(t("flash.support.usb"), "bad"), !serial && notice(t("flash.support.serial"), "bad")].filter(Boolean);
	if (support.length) support.push(notice(t("flash.kit.start"), "info"));
	const choice = (task, icon) => h("button", { class: "choice", type: "button", onclick: () => { S.task = task; S.step = 0; render(); } },
		h("strong", {}, `${icon} ${t(`flash.task.${task}`)}`), h("span", { class: "small muted" }, t(`flash.task.${task}.text`)));
	return h("div", { class: "flasher" },
		...support,
		panel(t("flash.start.title"),
			h("div", { class: "choice-grid" }, choice("update", "⟳"), choice("install", "⤓"), choice("backup", "⎘"), choice("restore", "↺"),
				choice("help", "?"))),
		h("details", { class: "small" }, h("summary", {}, t("flash.start.privacy_title")), h("p", {}, t("flash.start.privacy"))),
		h("p", { class: "small muted" }, S.sim ? h("a", { href: "#", onclick: (e) => { e.preventDefault(); location.hash = ""; location.reload(); } }, t("flash.sim.leave"))
			: simLinks()));
}

// The practice links: the first radio, and the first one flashed through the C7 stub on stock firmware and on RadioFoundry
function simLinks() {
	const link = (hash, text) => h("a", { href: hash, onclick: () => setTimeout(() => location.reload(), 0) }, text);
	const c7 = S.data.radios.find((r) => r.flash.backend === "c7000-stub");
	// a radio the page can take over from another open firmware (openfw.js)
	const open = S.data.radios.find((r) => openfw.supported(S.data.fb.boards[r.flash.board]));
	return [link("#sim", t("flash.sim.try")), ...(open ? [" · ", link(`#sim=${open.id}-openfw`, t("flash.sim.try_openfw"))] : []),
		...(c7 ? [" · ", link(`#sim=${c7.id}-stock`, t("flash.sim.try_c7000", { name: c7.model })),
			" · ", link(`#sim=${c7.id}`, t("flash.sim.try_c7000_rfoundry"))] : [])];
}

// ---------------------------------------------------------------------------------------------------------------
// update: a radio that runs RadioFoundry, reached once, its build kept

// The newest release of the radio (or the version link's): what the update installs
const updateVersion = () => VERSION_REF ? versionShort()
	: S.data.releases.filter((x) => x.board === S.radio.id).at(-1)?.version || defaultRef(S.data.site.branch);

// One port chooser for every radio: RadioFoundry's USB port or a programming cable. Whatever answers names the radio, and
// the radio's backend takes the connection from there (c7000ui.js adoptCable: no second chooser)
async function connectForUpdate(status) {
	let stream = null, link;
	if (S.fakeRadio) link = await connectSerial();
	else if (S.fakeC7) link = new Link(stream = S.fakeC7.stream, { timeout: 1200 });
	else {
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		if (await networkModeSeen()) throw new NetworkModeError();
		// a port granted before where RadioFoundry answers needs no chooser
		const known = await knownRadioPort([RFOUNDRY_USB, ...c7ui.CABLES]);
		stream = known?.stream || await SerialStream.open(await navigator.serial.requestPort({ filters: [RFOUNDRY_USB, ...c7ui.CABLES] }));
		link = new Link(stream, { timeout: 1200 });
	}
	status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
	let p, r;
	try {
		p = await probe(link);
		if (!p) throw await noAnswer(link);
		r = S.data.radios.find((x) => S.data.fb.boards[x.flash.board]?.debuglink_names.includes(p.board));
		if (!r) throw new Error(t("flash.err.unknown_board", { board: p.board }));
	} catch (e) {
		// the port isn't S.link's or the cable session's yet, so dropSerial and dropC7 can't close it: the next Connect
		// would find it open
		if (stream && !S.fakeC7) try { await stream.close(); } catch { /* gone */ }
		throw e;
	}
	selectRadio(r);
	optionsFromProbe(p);
	if (c7ui.isC7000(r, fb())) {
		c7ui.adoptCable(stream, stream?.port?.getInfo?.() || {}, p);
		S.now = { kind: "rfoundry", probe: p, stubOffer: null };
	} else {
		if (stream) {
			S.link = new Link(stream);
			useAdapter(adapter.fromSerialInfo(stream.port?.getInfo?.()));
		}
		S.now = await rfoundryNow(S.link, p);
		await keyFromRadio(S.link);
		await codecFromRadio(S.link, p, status);
	}
	S.current = currentBuild(p, r, S.data.releases.filter((x) => x.board === r.id), S.data.reported);
	S.decided = {};
	planUpdate(S.current, S.selection, S.decided);
	// a build the radio doesn't say: the build step, starting from the closest variant
	S.editBuild = S.current.kind === "unknown";
	diag.note("update: radio", { board: p.board, git: p.git, build: S.current.kind, preset: S.current.preset || S.current.guess || null });
}

function updateView() {
	const out = h("div", { class: "flasher" });
	const status = h("div");
	const draw = () => {
		if (!S.now?.probe) {
			S.forward(button(t("flash.update.connect"), async () => {
				try {
					await connectForUpdate(status);
					status.replaceChildren();
					render();
				} catch (e) {
					await dropSerial();
					await c7ui.dropC7();
					if (e?.name === "NotFoundError") return status.replaceChildren();
					status.replaceChildren(connectError(e), connectHelp(e, "flash.update.not_rfoundry"));
				}
			}, { primary: true }));
			out.replaceChildren(panel(t("flash.update.title"), h("p", {}, t("flash.update.lead")),
				h("ol", { class: "steps" }, h("li", {}, t("flash.update.step_on")), h("li", {}, t("flash.update.step_cable")),
					h("li", {}, t("flash.update.step_choose"))),
				h("p", { class: "small muted" }, t("flash.update.first_install"), " ",
					h("a", { href: "#", onclick: (e) => { e.preventDefault(); S.task = "install"; S.step = 0; render(); } }, t("flash.update.to_install"))),
				status));
			return;
		}
		const p = S.now.probe, cur = S.current;
		// the radio's own build, unless the build step has the selection (edits kept when the user comes back)
		const plan = S.editBuild ? { preset: S.selection.matches(startCode), added: [], ask: [], ready: true } : planUpdate(cur, S.selection, S.decided);
		// an image built for another selection no longer applies
		if (S.firmware?.selection && S.firmware.selection !== selectionKey(S.selection)) S.firmware = null;
		const vtitle = (id) => (t(`variant.${id}.title`) !== `variant.${id}.title` ? t(`variant.${id}.title`) : S.data.variants.find((v) => v.id === id)?.title || id);
		const title = (n) => S.selection.byName[n]?.title || n;
		const buildText = cur.kind === "unknown" && !S.editBuild ? t("flash.update.build_unknown") : plan.preset ? vtitle(plan.preset) : t("flash.build.custom");
		const edit = button(t(S.editBuild ? "flash.update.keep_build" : "flash.update.edit_build"), () => {
			S.editBuild = !S.editBuild;
			render();	// off again: the radio's own build (planUpdate in the redraw)
		}, { quiet: true });
		const asks = plan.ask.length ? h("div", { class: "notice action" },
			h("p", {}, h("b", {}, t("flash.update.new_title")), " ", t("flash.update.new_custom")),
			...plan.ask.map((n) => h("div", { class: "update-new" },
				h("span", {}, h("b", {}, title(n)), h("span", { class: "why" }, plainHelp(S.selection.byName[n]?.help || ""))),
				h("span", { class: "update-pick" }, ...[true, false].map((on) => h("label", {},
					h("input", { type: "radio", name: `new-${n}`, checked: S.decided[n] === on, onchange: () => { S.decided[n] = on; draw(); } }),
					" ", t(on ? "flash.update.turn_on" : "flash.update.leave_off"))))))) : null;
		const added = plan.added.length ? notice(t("flash.update.new_preset", { names: plan.added.map(title).join(", ") }), "info") : null;
		const building = S.build && S.build.selection === selectionKey(S.selection);
		const image = S.firmware ? notice(t("flash.build.image_ready", { what: S.firmware.source }), "ok")
			: building && S.build.error ? buildErrorNotice(S.build.error)
			: building ? h("p", { class: "muted", role: "status" }, spinner(), S.build.text) : null;
		const backup = h("label", { class: "toggle" },
			h("input", { type: "checkbox", checked: !!S.updateBackup, onchange: (e) => { S.updateBackup = e.target.checked; S.c7want = null; draw(); } }),
			h("span", {}, h("b", {}, t("flash.update.backup")), h("span", { class: "why" }, t("flash.update.backup_text"))), h("span"));
		// the STM32 radios' backup is the one before an install; the HR_C7000 radios' comes in their check step
		const backupPanel = S.updateBackup && !c7ui.isC7000(S.radio, fb()) && !S.backupDone ? backupNow({ when: "before", onDone: draw }) : null;
		out.replaceChildren(panel(t("flash.update.title"),
			h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.update.radio")), h("dd", {}, S.radio.name),
				h("dt", {}, t("flash.update.version")), h("dd", {}, t("flash.update.from_to", { from: firmwareName(p.version), to: updateVersion() })),
				h("dt", {}, t("flash.update.build")), h("dd", {}, buildText, " ", cur.kind === "unknown" ? null : edit)),
			cur.kind === "unknown" ? notice(t("flash.update.unknown_text", { variant: vtitle(cur.guess) }), "info") : null,
			S.editBuild ? null : asks, S.editBuild ? null : added, backup, backupPanel, image, status));
		const busy = building && !S.build.error;
		const go = async () => {
			if (S.editBuild) return next();
			if (!S.firmware && S.sim) await ensureFirmware();
			if (!S.firmware && !S.sim) await requestBuild(draw);
			if (S.firmware) next();
		};
		S.forward(button(busy ? [spinner(), t("flash.nav.building")] : t(S.editBuild ? "flash.update.to_build" : "flash.update.go"), go,
			{ primary: true, disabled: busy || (!S.editBuild && !plan.ready) || (S.updateBackup && !!backupPanel) }),
			!S.editBuild && !plan.ready ? t("flash.update.decide_first") : null);
	};
	draw();
	return out;
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
		h("span", { class: "small" }, ok ? t(`status.${r.status}`) : t("flash.radio.not_yet")),
		...variantLines(r));
	}));
	const detect = button(t("flash.radio.detect"), async () => {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw await noAnswer(link);
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
			status.replaceChildren(connectError(e), connectHelp(e, "flash.radio.openfw_next"));
		}
	}, { quiet: true });
	const status = h("div");
	S.forward(null);	// picking a radio is the step's action
	return panel(t("flash.radio.title"), list,
		S.task === "help" ? null : h("p", { class: "small muted detect" }, t("flash.radio.detect_hint"), " ", detect), status);
}

// A radio card's hardware variants (board.toml [hardware] variants): which codes are supported, which in development,
// which not yet; nothing for a radio that lists none
function variantLines(r) {
	const g = variantGroups(r);
	const line = (key, vs) => (vs.length ? h("span", { class: "small muted" }, t(key, { codes: vs.map((v) => v.code).join(", ") })) : null);
	return [line("flash.radio.variants_supported", g.supported), line("flash.radio.variants_dev", g.dev), line("flash.radio.variants_none", g.none)].filter(Boolean);
}

function selectRadio(r) {
	if (S.radio && S.radio.id !== r.id) Promise.resolve(backend().release?.()).then(() => { S.c7 = null; });
	S.radio = r;
	S.board = fb().boards[r.flash.board];
	S.selection = new Selection(r, S.data.features, S.data.variants);
	S.selection.useVariant("full");
	S.langAdded = null;
	S.firmware = null;
	S.buildOptions = {};
	S.variant = null;	// the hardware variant is read from the radio again
	S.stockRead = null;
}

// ---------------------------------------------------------------------------------------------------------------
// install: the build (variant, a la carte)

function buildView() {
	const sel = S.selection;
	const wrap = h("div", { class: "flasher" });
	const vtitle = (v) => (t(`variant.${v.id}.title`) !== `variant.${v.id}.title` ? t(`variant.${v.id}.title`) : v.title);
	const gtitle = (g) => (t(`group.${slug(g)}`) !== `group.${slug(g)}` ? t(`group.${slug(g)}`) : g);
	const supportedCount = S.data.features.filter((f) => sel.supported(f.name) && !startCode(f.name)).length;
	// The build type follows the checkboxes: a selection equal to a prebuilt variant is that variant, anything else is
	// custom. The Custom card can also be picked by hand before any change (it opens the groups).
	const change = (name, value) => {
		sel.set(name, value);
		if (!value && name === S.langAdded) S.langAdded = null;
		if (!value && packCode(name) && packCode(name) === S.startLang) S.startLang = "en";	// its language left out by hand
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
	// the browser asks for a language the radio has a pack for and the build leaves it out: offer it (langsuggest.js)
	const packNotice = () => {
		const pack = suggestPack(S.data.features, globalThis.navigator?.languages, sel, S.packsDismissed);
		if (!pack) return null;
		const title = sel.byName[pack.name]?.title || pack.name;
		return h("div", { class: "notice pack-suggest", role: "status" },
			h("p", {}, t("flash.build.pack_text", { language: languageName(pack.code), feature: title })),
			h("div", { class: "actions" },
				button(t("flash.build.pack_on", { language: languageName(pack.code) }), () => change(pack.name, true), { primary: true }),
				button(t("flash.build.pack_no"), () => { S.packsDismissed.add(pack.code); draw(); }, { quiet: true })));
	};
	// the language the radio starts in (LANG_DEFAULT_<CODE>): a choice of its own, not a feature in the groups. Picked,
	// or on an installation the page's language when its pack is on; an update keeps the radio's
	const startChoice = () => {
		const langs = startLanguages(S.data.features, sel);
		if (langs.length < 2) return null;
		const cur = startLanguage(S.data.features, sel);
		return h("div", { class: "start-lang" },
			h("label", {}, h("b", {}, t("flash.build.start_lang")), " ",
				h("select", { onchange: (e) => pickStart(e.target.value) },
					langs.map((l) => h("option", { value: l.code, selected: l.code === cur },
						l.included ? languageName(l.code) : t("flash.build.start_lang_adds", { language: languageName(l.code) }))))),
			h("p", { class: "small muted" }, t("flash.build.start_lang_help")));
	};
	// a Radio language picked by hand is built in: its pack goes in with it, and out again when the pick changes
	const pickStart = (code) => {
		if (S.langAdded && S.langAdded !== `LANG_${code.toUpperCase()}`) sel.set(S.langAdded, false);
		S.langAdded = null;
		S.startLang = code;
		S.customPicked = false;
		draw();
	};
	const draw = () => {
		const want = S.startLang ?? (S.task === "install" ? LANG : null);
		if (want) {
			const before = new Set(sel.on);
			setStartLanguage(S.data.features, sel, want, { include: S.startLang != null });
			S.langAdded = [...sel.on].find((n) => packCode(n) && !before.has(n)) || S.langAdded;
		}
		// the variant cards and the summary don't count the start language: Full in Spanish is still Full
		const match = S.customPicked ? null : sel.matches(startCode);
		// an image built for another selection no longer applies
		if (S.firmware?.selection && S.firmware.selection !== selectionKey(sel)) S.firmware = null;
		const card = (checked, onclick, ...body) => h("button", { class: "choice", type: "button", role: "radio",
			"aria-checked": String(checked), onclick }, ...body);
		const options = buildOptions().map((o) => h("div", {},
			h("h3", {}, o.title), o.help ? h("p", { class: "small muted" }, o.help) : null,
			h("div", { class: "choice-grid", role: "radiogroup" }, o.values.map((v) =>
				card(S.buildOptions?.[o.name] === v.id, () => { S.buildOptions = { ...S.buildOptions, [o.name]: v.id }; draw(); },
					h("strong", {}, v.title), v.text ? h("span", { class: "small muted" }, v.text) : null)))));
		const cards = S.data.variants.map((v) => card(match === v.id, () => { sel.useVariant(v.id); S.langAdded = null; S.customPicked = false; draw(); },
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
		h("span", { class: "small muted" }, t("flash.build.count", { n: [...sel.on].filter((n) => !startCode(n)).length, total: supportedCount }))));
		// every feature this radio supports, one collapsible group per feature-matrix group, each with its count
		const groups = S.data.groups.map((g) => {
			// what this radio can't run is listed too, grayed out, so nobody wonders where it went
			const all = S.data.features.filter((f) => f.group === g && f.name !== "AUDIO_PROBE" && !startCode(f.name));
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
		d.added = d.added.filter((n) => !startCode(n));
		const baseTitle = vtitle(S.data.variants.find((v) => v.id === sel.base));
		const summary = h("div", {}, match ? h("b", {}, t("flash.build.prebuilt", { variant: vtitle(S.data.variants.find((v) => v.id === match)) }))
			: h("span", {}, h("b", {}, t("flash.build.custom")), " ",
				d.added.length || d.removed.length
					? t("flash.build.custom_diff", { added: d.added.length, removed: d.removed.length, variant: baseTitle })
					: t("flash.build.custom_start")));
		const busy = building && !S.build.error;
		const missing = buildOptions().find((o) => !S.buildOptions?.[o.name]);
		forward(button(busy ? [spinner(), t("flash.nav.building")] : t(missing ? "flash.nav.choose" : S.firmware || S.sim ? `flash.nav.to_${stepsFor()[S.step + 1]}`
			: VERSION_REF ? "flash.nav.build_version" : "flash.nav.build", { what: missing?.title.toLowerCase(), ref: VERSION_REF && versionShort() }), go, { primary: true, disabled: busy || !!missing }), summary);
		// every image, prebuilt choice or not, comes from the build service (or is the user's own)
		// the image's state, then building it yourself: one line and a fold under the features, not a card of its own
		const image = h("div", { class: "build-image" },
			S.firmware ? notice(t("flash.build.image_ready", { what: S.firmware.source }), "ok")
				: building && S.build.error ? buildErrorNotice(S.build.error)
				: building ? h("p", { class: "muted", role: "status" }, spinner(), S.build.text)
				: h("p", { class: "small muted" }, t(S.sim ? "flash.sim.image" : VERSION_REF ? "flash.build.version_on_request" : "flash.build.on_request", { ref: versionShort() })),
			h("details", { class: "small" }, h("summary", {}, t("flash.build.self_title")),
				h("p", {}, t("flash.build.self_text")), h("pre", {}, h("code", {}, sel.makeCommand(S.buildOptions))),
				filePicker(t("flash.build.own_image"), ".bin", (f) => loadOwnImage(f, draw)),
				filePicker(t("flash.build.own_manifest"), ".json", async (f) => { S.ownManifest = JSON.parse(new TextDecoder().decode(f.data)); })));
		wrap.replaceChildren(
			panel(t("flash.build.title"),
				// "back up first" only where the original firmware can be backed up before the installation
				S.radio.status !== "supported" ? notice(t(S.radio.backup?.stock === "none" ? "flash.build.untested_keep_file" : "flash.build.untested", { name: S.radio.name }), "bad") : null,
				...options,
				h("p", {}, t("flash.build.lead")),
				h("div", { class: "choice-grid", role: "radiogroup" }, cards),
				packNotice(),
				startChoice(),
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
		const id = useId();
		let b = await api("POST", "builds", { board: S.radio.id, ref: VERSION_REF || (rels.length ? rels[rels.length - 1].version : defaultRef(S.data.site.branch)), features: [...sel.on].sort(),
			options: S.buildOptions || {}, tool: "web", tool_version: S.data.site.version || "", task: "install", id, ...(id ? { languages: browserLanguages() } : {}) });
		while (b.status === "queued" || b.status === "running") {
			if (!current()) return;
			S.build.text = b.status === "queued" && b.position ? t("flash.build.queued", { n: b.position }) : t("flash.build.building");
			redraw();
			await new Promise((ok) => setTimeout(ok, 3000));
			b = await api("GET", `builds/${b.id}`);
		}
		if (b.status !== "done") {
			const why = b.error || b.status;
			const fit = tooBig(why);
			if (fit) throw new FitError(fit);
			// a feature or option the built version doesn't have: explained by errorText, not as a failed compile
			throw /^unknown (feature|option)/i.test(why) ? new ApiError(why) : new Error(t("flash.build.failed", { error: why }));
		}
		const [m, img] = await Promise.all([fetch(new URL(b.manifest, API)).then((r) => r.json()),
			fetch(new URL(b.image, API)).then((r) => r.arrayBuffer()).then((x) => new Uint8Array(x))]);
		await verify(m, img, fb(), S.board.id);
		for (const [name, value] of Object.entries(S.buildOptions || {}))
			if (m.build_options?.[name] !== value) throw new Error(t("flash.build.wrong_options", { name, value }));
		if (!current()) return;
		// variant: the prebuilt choice this is, for the flash log (the service names every image it builds "custom")
		S.firmware = { manifest: m, image: img, source: t("flash.build.built", { version: m.version }), origin: "custom",
			variant: sel.matches(startCode) || "custom", build: b.id, selection: key, pinned: !!VERSION_REF };
		S.build = null;
	} catch (e) {
		if (S.build?.selection === key) S.build.error = e instanceof FitError ? { text: fitText(e.fit), fit: e.fit }
			: { text: t("flash.build.unavailable", { error: errorText(e) }), rec: diag.currentError() };
	}
	redraw();
}

// What to do about a build too big for the radio: turn things off, and a Radio language picked into the build is one
function fitText(fit) {
	const text = t(fit.memory === "flash" ? "flash.build.too_big" : "flash.build.too_big_ram", { kb: fit.kb });
	return S.langAdded ? `${text} ${t("flash.build.too_big_lang", { language: languageName(packCode(S.langAdded)) })}` : text;
}

// The build step's notice for a failed build: one too big is a plain notice, any other an error with its report
const buildErrorNotice = (err) => (err.el ||= err.fit ? notice(err.text, "bad") : errorNotice(err.text, err.rec));

async function loadOwnImage(f, redraw) {
	try {
		if (startsWith(f.data, fb().magic)) throw new Error(t("flash.err.stock_as_image"));
		// an image merged with its codec (tools/hrc7000_codec.py): the backend keeps the codec, the rest is the image
		if (backend().ownImage) f = { ...f, data: await backend().ownImage(fb(), S.radio.flash.board, f.data) };
		const m = S.ownManifest || await forDevBuild(S.board, f.data);
		const board = await verify(m, f.data, fb(), S.board.id);
		S.firmware = { manifest: m, image: f.data, source: f.name, origin: "own", board };
	} catch (e) {
		const text = errorText(e);
		alert(reportingActive() && diag.currentError()?.eventId ? `${text}\n\n${t("report.reported_short")}` : text);
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
		// "openfw" and "unknown" are both kind "unknown" to the install (explain.js nowFor); the pressed one is told apart
		const choice = (id, onclick) => h("button", { class: "choice", type: "button", "aria-pressed": String(nowChoice(S.now) === id), onclick },
			h("strong", {}, t(`flash.now.${id}`)), h("span", { class: "small muted" }, t(`flash.now.${id}_text`)));
		const pick = (id) => () => { S.now = nowFor(id); draw(); };
		const opts = h("div", { class: "choice-grid" }, choice("rfoundry", checkRFoundry), choice("stock", pick("stock")),
			choice("openfw", pick("openfw")), choice("unknown", pick("unknown")));
		const detail = h("div");
		if (S.now?.kind === "rfoundry" && S.now.probe) {
			const p = S.now.probe;
			detail.append(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.board")), h("dd", {}, p.board),
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, firmwareName(p)),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, codecText(p.codec))),
			S.backupDone ? notice(t("flash.backup.done_before"), "ok") : backupNow({ when: "before", onDone: draw, actions: (b) => { runBackup = b; } }));
		} else if (S.now?.kind === "stock") {
			const versions = S.radio.flash.stock_versions;
			detail.append(h("label", {}, t("flash.now.which_version"), " ",
				h("select", { onchange: (e) => { S.now.version = e.target.value || null; } },
					h("option", { value: "" }, t("flash.now.version_unknown")),
					versions.map((v) => h("option", { value: v, selected: S.now.version === v }, v)))),
			notice(t(S.radio.backup.stock === "none" ? "flash.now.stock_backup_later" : "flash.now.stock_backup_planned"), "info"));
		} else if (openfwShown()) {
			detail.append(openfwDetail({ draw, check: checkRFoundry, setRun: (b) => { runBackup = b; } }));
		}
		out.replaceChildren(panel(t("flash.now.title"), opts, detail, status));
		// on RadioFoundry the backup comes first: saving it is the main action, skipping it the quiet one beside it
		const skip = (S.now?.kind === "rfoundry" || openfwRecognizedNow()) && !S.backupDone && runBackup;
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
			if (!p) throw await noAnswer(link);
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			const odd = optionMismatch(p);
			if (odd) throw new Error(t("flash.err.wrong_option", { board: p.board, option: odd[0].title.toLowerCase(), value: odd[1].title }));
			S.now = await rfoundryNow(link, p);
			await keyFromRadio(link);
			await codecFromRadio(link, p, status);
			status.replaceChildren();
		} catch (e) {
			await dropSerial();
			if (openfwRecognized(e, status, draw)) return;
			// another open firmware (or RadioFoundry without the debug link): the "Another open firmware" choice is what
			// this radio needs, one click away
			const other = ANSWERED.has(e.params?.kind) ? h("p", {}, t("flash.now.openfw_switch"), " ",
				button(t("flash.now.openfw_use"), () => { S.now = nowFor("openfw"); status.replaceChildren(); draw(); })) : null;
			status.replaceChildren(connectError(e), other || serialHelp());
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
		// the factory calibration is the SPI flash's security registers (readRadio: 3 x 256 bytes, over the debug link or the loader)
		calibration: { bytes: 768, what: { internal: false, spi: "none" } },
	};
	let pick = "all";
	const times = {};
	// the loader reads the internal flash at about 500 KB/s and the SPI flash at about 280 KB/s (stub-stm32f4.md 11),
	// plus the hand-over and the restart; the codeplug alone is no faster that way, so the debug link reads it. The
	// calibration alone takes the loader only where the build doesn't serve the security registers (readRadio)
	const viaLoader = () => !!(f4Board() && S.now?.kind === "rfoundry" && S.now.stubOffer && pick !== "codeplug");
	const seconds = (id) => (id !== "codeplug" && id !== "calibration" && f4Board() && S.now?.stubOffer
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
			// only a read long enough to time says how fast the memory reads: the calibration is mostly round trips
			if (!bulk?.used && choices[pick].bytes >= 0x10000) S.readRate = choices[pick].bytes / Math.max(0.5, (performance.now() - t0) / 1000);
			if (S.stock && !codecOnlyFile(S.stock)) S.backupParts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: S.stock.data, complete: true });
			const after = when === "after";
			const meta = {
				radio: { board: S.radio.id, name: S.radio.name, model: S.radio.model, chip: p.chip, version: p.version },
				firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec, features: p.features },
				firmware_before: after && S.now?.kind === "stock" ? { kind: "stock", version: S.now.version, stock_file_sha256: S.stock?.info.sha || null }
					: after && openfwRecognizedNow() ? { kind: "openfw", git: S.now.git, radio_type: S.now.radioType } : undefined,
				contents: pick,
				notes: after && S.now?.kind !== "rfoundry" ? [t("flash.backup.note_after")] : [],
			};
			const bytes = await makeBundle(meta, S.backupParts, { git: S.data.site.git, url: location.origin });
			const name = bundleName(meta);
			download(name, bytes, "application/zip");
			S.lastBackup = { name, data: bytes };
			diag.note("backup saved", { contents: pick, size: bytes.length, parts: S.backupParts.map((x) => `${x.what}${x.complete ? "" : " (incomplete)"}`) });
			S.backupDone = true;
			flashFinished(started);
			// the codeplug protocol's copy of the security registers kept beside the loader's is not a failed read
			const incomplete = S.backupParts.filter((x) => !x.complete && !["spi-flash", "security-registers-unchecked"].includes(x.what)).map((x) => x.file);
			const noCal = !S.backupParts.some((x) => x.what === "security-registers");
			box.append(notice(t("flash.backup.saved", { name, size: Math.round(bytes.length / 1024) }), "ok"));
			if (incomplete.length) box.append(notice(t("flash.backup.incomplete", { files: incomplete.join(", ") })));
			if (noCal) box.append(notice(t("flash.backup.no_security"), pick === "calibration" ? "bad" : ""));
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
			box.append(errorNotice(errorText(e)));
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
		option("all"), option("firmware"), option("codeplug"), option("calibration"));
	const runBtn = button(t("flash.backup.run"), run, { primary: true });
	if (actions) actions(runBtn);
	else box.append(h("div", { class: "actions" }, runBtn));
	// the estimates come from this radio: a short timed read, unless a backup already measured it
	if (!S.readRate) {
		measureRate().then(() => { for (const id of Object.keys(choices)) times[id]?.replaceChildren(estimate(choices[id].bytes, seconds(id))); });
	}
	return box;
}

// ---- another open firmware (openfw.js), on the current-firmware step. Kept together here: nowView only calls in

// The step's detail for it: before connecting, the offer to check the radio; once recognized, the backup
// The "Another open firmware" choice (explain.js nowFor) on a board this page backs up from; "Other / Not sure" doesn't
// offer it: its route is the stock file, and a probe from the RadioFoundry choice still recognizes the radio
const openfwShown = () => nowChoice(S.now) === "openfw" && openfw.supported(S.board);

// That choice once a probe recognized the radio: its radio type and revision are kept on it
const openfwRecognizedNow = () => !!(nowChoice(S.now) === "openfw" && S.now.radioType != null);

function openfwDetail({ draw, check, setRun }) {
	if (!openfwRecognizedNow()) {
		// another open firmware is found by connecting: its backup gives the key and the codec, no stock file needed
		return h("p", { class: "small" }, t("flash.now.openfw_check_text"), " ", button(t("flash.now.openfw_check"), check, { quiet: true }));
	}
	return h("div", {}, h("p", {}, t("flash.now.openfw_found", { git: S.now.git || "?", type: S.now.radioType })),
		S.backupDone ? h("div", {}, notice(S.openfwSaved ? t("flash.backup.saved", S.openfwSaved) : t("flash.backup.done_before"), "ok"), ...openfwFound())
			: openfwBackupNow({ onDone: draw, actions: setRun }));
}

// A probe that found another open firmware (link.js whyNoAnswer, kind "openfw"): on a board this page backs up from
// it, the radio's own state with the backup; a radio type of another radio is a wrong pick, said as such, with the
// stock-file route one click away. false: not handled here (the caller says what it found)
function openfwRecognized(e, status, draw) {
	const route = openfw.recognized(S.board, e.params);
	if (route !== "backup" && route !== "wrong-type") return false;
	if (route === "wrong-type") {
		status.replaceChildren(errorNotice(t("flash.openfw.wrong_type", { type: e.params.radioType, name: S.board.name })),
			h("p", {}, t("flash.openfw.wrong_type_go_on"), " ",
				button(t("flash.openfw.wrong_type_use"), () => { S.now = nowFor("unknown"); status.replaceChildren(); draw(); })));
		return true;
	}
	S.now = { ...nowFor("openfw"), radioType: e.params.radioType, git: e.params.git };
	status.replaceChildren();
	draw();
	return true;
}

// On the codec step, when the stock file is needed after all: why the radio's backup didn't give the codec or key
function openfwCodecNote(needs) {
	if (nowChoice(S.now) !== "openfw" || !openfw.supported(S.board) || !needs.length) return null;
	const why = !S.openfwFound ? "flash.openfw.step_skipped" : !S.openfwFound.codec ? "flash.openfw.step_no_codec" : "flash.openfw.step_no_key";
	return notice(t(why), "info");
}

// What the backup of a radio on another open firmware found: the key, the codec (each one line)
const openfwFound = () => (!S.openfwFound ? [] : [
	notice(t(S.openfwFound.key ? "flash.openfw.key_found" : "flash.openfw.key_missing"), S.openfwFound.key ? "ok" : "warn"),
	notice(t(S.openfwFound.codec ? "flash.openfw.codec_found" : "flash.openfw.codec_missing"), S.openfwFound.codec ? "ok" : "warn")]);

// The backup of a radio on another open firmware, before the install (openfw.js): everything, over that firmware's
// CPS, read only. From it the bootloader key and the codec, kept as if read from a RadioFoundry radio, so the codec step
// merges the saved copy and needs no stock file. A radio that stops answering keeps what was read: after a power cycle
// the same button continues; what was read can be saved meanwhile
function openfwBackupNow({ onDone = null, actions = null } = {}) {
	const box = h("div", { class: "panel" });
	const prog = progressBar();
	const status = h("div");
	const bytes = openfw.backupBytes(S.board, S.radio);
	const saveBundle = async (st) => {
		const meta = {
			radio: { board: S.radio.id, name: S.radio.name, model: S.radio.model, chip: null, version: null },
			firmware_now: openfw.firmware(st.info),
			contents: "all",
			notes: openfw.complete(st) ? [] : [t("flash.openfw.note_incomplete")],
		};
		const parts = openfw.bundleParts(st, S.board);
		if (S.stock && !codecOnlyFile(S.stock)) parts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: S.stock.data, complete: true });
		const zipped = await makeBundle(meta, parts, { git: S.data.site.git, url: location.origin });
		const name = bundleName(meta);
		download(name, zipped, "application/zip");
		S.lastBackup = { name, data: zipped };
		return { name, size: Math.round(zipped.length / 1024) };
	};
	// the key and the codec, from as much of the MCU flash as was read
	const take = async (st) => {
		const { key, codec } = await openfw.analyze(st, S.board, fb(), (line) => diag.note(line));
		if (key) {
			S.key = key;
			S.keyFromRadio = true;
			S.keySource = "radio";
			if (!S.sim) try { localStorage.setItem(KEY_STORE(S.board.id), toHex(key)); } catch (e) { /* not remembered */ }
		}
		if (codec) S.savedCodec = { profile: codec.profile, segments: codec.save, from: "radio", stored: S.sim ? false : await saveCodec(S.board, codec.save) };
		diag.note("open firmware backup", { key: !!key, codec: codec ? `${codec.profile.name} ${codec.where}` : null, requests: st.requests, stopped: st.stopped });
		S.openfwFound = { key: !!key, codec: !!codec };
		return openfwFound();
	};
	const partial = button(t("flash.openfw.save_partial"), async () => {
		try {
			const s = await saveBundle(S.openfwRead);
			status.append(notice(t("flash.backup.saved", { name: s.name, size: s.size }), "ok"));
		} catch (e) {
			diag.error("flash/openfw save partial", e);
			status.append(errorNotice(errorText(e)));
		}
	}, { quiet: true });
	const run = async () => {
		runBtn.disabled = true;
		// leaving the step (Back, Skip, Start Over: render()) stops the read too, keeping what was read
		const stop = S.openfwAbort = new AbortController();
		status.replaceChildren(h("div", { class: "actions" }, button(t("flash.openfw.stop"), () => stop.abort(), { quiet: true })));
		box.append(prog.el);
		const started = flashStarted({ task: "backup", source: "openfw", radio: S.board.id, firmware: S.now?.git || "", features: "" });
		try {
			const link = await connectSerial();
			S.openfwRead = await openfw.readBackup(link, S.board, S.radio, { state: S.openfwRead, signal: stop.signal,
				progress: (part, d, total) => prog.set(d, total, t(`flash.backup.part.${part}`)) });
			const st = S.openfwRead;
			const found = await take(st);
			if (st.stopped) {
				flashFinished(started, new Error(`stopped at ${hex(st.stopped.addr)}: ${st.stopped.why}`));
				await dropSerial();
				const why = { "no answer": "flash.openfw.stopped", refused: "flash.openfw.refused", canceled: "flash.openfw.canceled" }[st.stopped.why];
				status.replaceChildren(notice(t(why, { addr: hex(st.stopped.addr), part: t(`flash.backup.part.${st.stopped.part}`) }), "action"),
					...found, h("div", { class: "actions" }, partial));
				runBtn.textContent = t("flash.openfw.again");
				runBtn.disabled = false;
				return;
			}
			const s = await saveBundle(st);
			S.openfwSaved = s;
			S.backupDone = true;
			flashFinished(started);
			prog.el.remove();
			status.replaceChildren(notice(t("flash.backup.saved", { name: s.name, size: s.size }), "ok"), ...found);
			onDone?.();
		} catch (e) {
			flashFinished(started, e);
			await dropSerial();
			const key = e instanceof openfw.OpenFwError ? (e.code === "type" ? "flash.openfw.wrong_type" : "flash.openfw.no_info") : null;
			// the way on without this backup: skip it, and the codec step asks for the stock file
			status.replaceChildren(errorNotice(key ? t(key, { type: e.params?.radioType, name: S.board.name }) : errorText(e)),
				h("p", { class: "small" }, t("flash.openfw.skip_hint")));
			runBtn.disabled = false;
		}
	};
	const runBtn = button(t(S.openfwRead?.stopped ? "flash.openfw.again" : "flash.openfw.run"), run, { primary: true });
	box.append(h("h3", {}, t("flash.openfw.title")),
		h("p", {}, t("flash.openfw.lead"), " ", estimate(bytes, bytes / openfw.RATE)), status);
	if (actions) actions(runBtn);
	else box.append(h("div", { class: "actions" }, runBtn));
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
		box.append(notice(t("flash.storage.failed"), "bad"), reportLine());
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
	const size = mb >= 1 ? `${num(mb, { maximumFractionDigits: mb >= 10 ? 0 : 1, minimumFractionDigits: mb >= 10 ? 0 : 1 })} MB` : `${num(Math.round(bytes / 1024))} KB`;
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
			out.replaceChildren(panel(t("flash.codec.title"), errorNotice(errorText(e))));
			forward(null);
			return;
		}
		const haveStock = fb().profiles[S.stock?.info.sha]?.board === S.board.id;
		// a picked build of this radio without a codec profile still gives the key (the opener derived it)
		const keyStored = S.sim || !!S.key || !!savedKey(S.board);
		const keyKnown = keyStored || (S.stock?.match?.board === S.board.id && !!S.stock.match.key);
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
				runsStock: S.now?.kind === "stock",
			});
		} catch (e) {
			S.decision = null;
			err = e.key ? said(e.key, e.args) : e.reason || e.message;
		}
		noteDecision(err);
		// The stock file is needed when the codec has to come from it, or the bootloader key can't come from anywhere
		// else. Whatever the case, the page says what happens and what the user has to do.
		const needs = [];
		if (!S.decision && !haveStock) needs.push(`${err} ${t("flash.codec.need_codec")}`);
		// a file for another radio that gives this one's codec (file_radio) never gives the key: getKey refuses it
		const codecOnly = haveStock ? fb().profiles[S.stock.info.sha].file_radio : null;
		const stockKey = haveStock && !codecOnly;
		if (S.decision && !keyKnown && codecOnly) {
			const prof = fb().profiles[S.stock.info.sha];
			needs.push(t("flash.err.codec_only_key", { file: prof.name, radio: codecOnly, board: S.board.name }));
		} else if (S.decision && !keyKnown && !stockKey) needs.push(t("flash.codec.need_key"));
		const ready = S.decision && (keyKnown || stockKey);
		// a merge from the copy saved from the radio isn't one from the stock file: its own label
		const modeKey = S.decision?.mode === MERGE && S.decision.source === "saved" ? "merge_saved" : S.decision?.mode;
		const plan = S.decision ? `${t(`flash.codec.mode.${modeKey}`)}: ${S.decision.whyKey ? said(S.decision.whyKey, S.decision.args) : S.decision.why}.` : null;
		const keyLine = S.keyFromRadio ? t("flash.codec.key_from_radio") : keyStored && !S.sim ? t("flash.codec.key_saved") : null;
		const fromSaved = S.decision?.source === "saved";
		const codecLine = !fromSaved ? null : S.savedCodec.from === "browser" ? t("flash.codec.codec_saved")
			: t(S.savedCodec.stored ? "flash.codec.codec_from_radio" : "flash.codec.codec_from_radio_unsaved");
		const picker = S.sim ? button(t("flash.sim.use_stock"), async () => { await useStock({ name: "simulated-stock.bin", data: S.sim.stock }); })
			: filePicker(t("flash.codec.stock_pick"), ".bin,.zip", useStock);
		// shown only for a file that serves (a problem has its own notice)
		const loadedText = S.stock && t("flash.codec.stock_loaded", { name: S.stock.name, what: S.stock.info.known || t("flash.codec.stock_build", { radio: S.board.name }) });
		// anything that gives no codec: why, and what to select instead (explain.js); what each file in the pick is. A
		// build of this radio without a codec map (it may still give the bootloader key) is "unmapped": the notice names
		// the version, offers to send the file and lists every supported file
		const problem = S.stock?.problem || null;
		const report = S.stock?.report?.length && (S.stock.report.length > 1 || !haveStock)
			? h("details", { class: "small" }, h("summary", {}, t("flash.codec.stock_report")), h("ul", {}, S.stock.report.map((l) => h("li", {}, l)))) : null;
		const loaded = !S.stock ? null : [problem ? problemNotice(problem, { offer: S.stock.analysis }) : h("p", {}, "✓ ", loadedText), report];
		// a download link only where there is a known one; no vague pointers. The unmapped notice lists the files itself
		const download = S.radio.flash.stock_download ? h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.codec.stock_download")) : null;
		const where = problem?.problem === "unmapped" ? (download ? [h("p", {}, download)] : [])
			: [h("p", {}, t("flash.codec.stock_files", { files: stockFiles() }), download ? [" ", download] : null),
				S.board.codec.sources.length > STOCK_NAMES_MAX ? h("details", { class: "small" }, h("summary", {}, t("flash.codec.stock_all")),
					h("ul", {}, S.board.codec.sources.map((n) => h("li", {}, n)))) : null];
		// the install goes on without the codec from the picked file: say what DMR voice then depends on, before writing
		const keeping = keepNote(problem, S.decision);
		const parts = [h("p", {}, t("flash.codec.lead")), openfwCodecNote(needs)].filter(Boolean);
		const viaStub = stubRoute();
		if (viaStub) {
			parts.push(h("div", { class: "notice ok" }, h("strong", {}, t("flash.f4.codec_title")),
				h("p", {}, t("flash.f4.codec_text"))));
		} else if (needs.length) {
			parts.push(h("div", { class: "notice action" }, h("strong", {}, t("flash.codec.action_title")),
				needs.length > 1 ? h("ul", {}, needs.map((n) => h("li", {}, n))) : h("p", {}, needs[0]), where, picker, loaded));
			if (!S.decision && S.now?.kind !== "rfoundry" && !openfwRecognizedNow()) parts.push(h("p", { class: "small muted" }, t("flash.codec.or_connect")));
		} else if (!S.decision) {
			parts.push(notice(err, "bad"));
		} else {
			parts.push(h("div", { class: "notice ok" }, h("strong", {}, t(haveStock && S.stock ? "flash.codec.ready_title" : "flash.codec.none_title")),
				h("p", {}, plan), codecLine ? h("p", {}, codecLine) : null, keyLine ? h("p", {}, keyLine) : null, problem ? null : loaded));
			// a file that gives no codec has its own notice, outside the "ready" one, and then what the install does instead
			if (problem) parts.push(...loaded.filter(Boolean));
			if (keeping) parts.push(h("div", { class: `notice ${keeping === "ok" ? "info" : keeping === "none" ? "bad" : "warn"}` },
				h("strong", {}, t(`flash.codec.keep_${keeping}.title`)), h("p", {}, t(`flash.codec.keep_${keeping}.text`))));
			if (S.decision.risk) parts.push(h("p", { class: "small muted" }, S.decision.riskKey ? t(S.decision.riskKey)
				: S.decision.risk[0].toUpperCase() + S.decision.risk.slice(1) + "."));
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
		if (!S.keyFromRadio) S.key = S.keySource = null;
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

// Update mode steps beside the radio's own picture (radiotop.js), with the keys to hold marked on it and named under
// it (board.toml [flash.bootloader] hold). The picture makes no claim about the LED or the screen: what a radio shows in
// its bootloader is in its steps' words, not in the data
function withPicture(steps) {
	const pic = radioPicture(S.radio, { keys: holdKeys(S.radio) });
	return pic ? h("div", { class: "radio-grid" }, steps, pic) : steps;
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
			// the stock file is what the key and the codec come from: a problem with it is explained, not just quoted
			box.replaceChildren(h("h2", {}, title), failureNotice(e, S.stock));
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
				status.replaceChildren(...h("div", {}, errorNotice(errorText(e)),
					onWindows() && usbSupported() ? windowsDriverHelp({ failed: true }) : null,
					e?.name === "NotFoundError" ? null : manualKitLine()).childNodes);
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
				await new Dfu(transport, { log: log.add }).run(plan, S.key, (d, total) => {
					if (d > 0) flashMark(started, "first_byte");
					prog.set(d, total, t("flash.write.writing"));
				});
				flashMark(started, "last_byte");
				flashFinished(started);
				log.add(t("flash.write.finished"));
				await transport.close?.();
				onDone();
			} catch (e) {
				flashFinished(started, e);
				status.replaceChildren(...h("div", {}, errorNotice(t("flash.write.failed", { error: errorText(e) })), helpLink(), manualKitLine()).childNodes);
				write.disabled = false;
				place(connect);
				connect.disabled = false;
			}
		}, { primary: true });
		place(connect);
		// through h(): the DOM's own replaceChildren writes a null as the text "null"
		box.replaceChildren(...h("div", {}, inline ? null : h("h2", {}, title),
			h("h3", {}, t("flash.write.bootloader_title")),
			withPicture(h("ol", { class: "steps" }, S.radio.flash.bootloader.map((s) => h("li", {}, s)),
				h("li", {}, t("flash.write.then_connect")))),
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
		ensureFirmware().then(render, (e) => out.replaceChildren(errorNotice(errorText(e))));
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
		} else if (S.decision.mode === MERGE) ({ profile: prof, segments: codec } = await stockfw.extract(S.stock.data, S.key, fb(), fb().profiles, S.board.id));
		const plan = makePlan(S.board, segments(fw.manifest, fw.image), S.decision.mode, codec,
			prof ? stockfw.layoutWindows(prof, fb()) : S.board.codec.windows);
		notePlan(plan, prof);
		return plan;
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
				progress: (done, todo) => {
					if (done > 0) flashMark(started, "first_byte");
					prog.set(done, todo, t("flash.f4.writing", { done, todo }));
				} });
			flashMark(started, "last_byte");
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
				status.replaceChildren(notice(t("flash.f4.failed", { error: t("flash.c7.write.lost") }), "bad"), h("p", {}, t("flash.f4.after_fail")), reportLine());
				S.forward(null);
			}, { primary: true });
			status.replaceChildren(...h("div", {}, errorNotice(t("flash.f4.failed", { error: errorText(e) })),
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
				status.replaceChildren(errorNotice(t("flash.f4.enter_failed", { error: errorText(err) })));
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
			status.replaceChildren(errorNotice(t("flash.f4.enter_failed", { error: errorText(e) })));
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
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw await noAnswer(link, { written: S.firmware?.manifest });
			// the loader route never touches the codec, so it is there afterwards as it was before
			const good = !(S.decision ? S.decision.expectCodec : true) || p.codec === S.board.codec.id;
			diag.note(...crumbs.verify(p, good));
			flashMark(S.lastStarted, "verify", { ok: good, reason: good ? "" : `codec ${p.codec || "none"}` });
			S.reportResult = good ? "problem" : "failed";
			intro.replaceChildren();	// "the radio is restarting" is over: what it says now replaces it
			status.replaceChildren(h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.now.firmware")), h("dd", {}, firmwareName(p)),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, codecText(p.codec))),
			notice(good ? t("flash.done.ready") : t("flash.done.no_codec"), good ? "ok" : "bad"), ...(good ? [] : [helpLink(), reportLine()]));	// the DOM writes a null as "null"
			const channels = () => bringChannels(status, { finish });
			if (!S.backupDone) {
				S.now = { ...S.now, probe: S.now?.kind === "rfoundry" ? p : S.now?.probe, after: p };
				status.append(backupNow({ when: "after", onDone: channels,
					actions: (b) => S.forward([button(t("flash.nav.finish_no_backup"), channels, { quiet: true }), b]) }));
			} else {
				// the other open firmware's backup holds the whole memory chip: its data in RadioFoundry's settings area can go
				if (openfwRecognizedNow() && openfw.complete(S.openfwRead)) await prepareStorage(status);
				await channels();
			}
		} catch (e) {
			await dropSerial();
			check.disabled = false;
			flashMark(S.lastStarted, "verify", { ok: false, reason: String(e?.message || e) });
			diag.note("verify failed", { kind: e?.params?.kind || null, message: String(e?.message || e).slice(0, 120) });
			S.reportResult = "failed";
			status.replaceChildren(connectError(e), connectHelp(e, "flash.done.openfw_next"), helpLink());
		}
	}, { primary: true });
	S.forward(check);
	const intro = h("div", {}, S.stubResult ? notice(S.stubResult, "ok") : null, h("p", {}, t("flash.done.lead")));
	// whatever the check says: the user may know better (a radio that verifies but misbehaves)
	const quiet = h("p", { class: "small muted" }, t("report.something_wrong"), " ", reportButton());
	box.append(panel(t("flash.done.title"), intro, status, quiet));
	return box;
}

// The original firmware's channels, after an install (migrate.js): read the memory chip; if it (or this visit's
// backup, read before the settings area was claimed) holds a stock codeplug this page converts, convert it, prepare the
// settings area, write the sectors that change and read them back. With a backup from this visit that holds the
// original codeplug it runs at once; without one it waits for the user, since on some radios the write replaces the
// original bytes. Any failure stops the step and leaves the radio as RadioFoundry, without channels. An update, a move
// from another open firmware and a radio whose backup shows no stock codeplug skip it (channelsWanted)
async function bringChannels(status, { finish }) {
	const saved = S.backupDone ? S.backupParts?.find((x) => x.what === "spi-flash")?.data : null;
	let m;
	try {
		m = await import("./migrate.js");
	} catch (e) {
		diag.error("flash/channels load", e);
	}
	if (!m?.channelsWanted({ before: nowChoice(S.now), spiSize: S.radio?.backup?.spi_size, saved })) {
		S.forward(finish());
		return;
	}
	const box = h("div");
	const prog = progressBar();
	status.append(box);
	const say = (...x) => box.append(...x);
	// what a failure says depends on whether this visit's backup holds the original codeplug
	const failed = (key, copy, x = {}) => t(copy ? key : `${key}_no_backup`, x);
	let link, read, src = { layout: null };
	try {
		link = await connectSerial();
		box.append(prog.el);
		const reading = (d, n) => prog.set(d, n, t("flash.channels.reading"));
		// the radio's codeplug span as it is now, for the sectors that change; the stock codeplug from the backup if
		// this visit took one (migrate.js stockSource)
		read = await link.cpsDump(1, 0, m.readSpan(null), { progress: reading });
		if (!read.complete) throw new Error(`memory chip read stopped at ${read.data.length}`);
		src = m.stockSource(read.data, saved);
		// the stock tables reach past RadioFoundry's codeplug span on some families
		if (src.from === "radio" && src.layout && m.readSpan(src.layout) > read.data.length) {
			const more = await link.cpsDump(1, read.data.length, m.readSpan(src.layout) - read.data.length, { progress: reading });
			if (!more.complete) throw new Error("memory chip read stopped past the codeplug span");
			const all = new Uint8Array(read.data.length + more.data.length);
			all.set(read.data);
			all.set(more.data, read.data.length);
			src = m.stockSource(all, null);
		}
	} catch (e) {
		diag.error("flash/channels read", e);
		prog.el.remove();
		say(notice(failed("flash.channels.read_failed", !!m.savedStock(saved)), "bad"), reportLine());
		S.forward(finish());
		return;
	}
	prog.el.remove();
	const { layout } = src;
	const copy = src.from === "backup";
	diag.note("channels", { layout, from: src.from, read: read.data.length });
	if (!layout) {
		S.forward(finish());
		return;
	}
	let conv;
	try {
		conv = m.convertStock(src.bytes, layout);
	} catch (e) {
		diag.error("flash/channels convert", e);
		say(notice(failed("flash.channels.convert_failed", copy), "bad"), reportLine());
		S.forward(finish());
		return;
	}
	const counts = (c) => t("flash.channels.counts", c);
	const notes = conv.notes.length ? h("ul", { class: "small" }, conv.notes.map((x) => h("li", {}, t(`editor.qdmr.note.${x.code}`, { n: x.n })))) : null;
	diag.note("channels converted", { layout, ...conv.counts, notes: conv.notes.map((x) => `${x.code}:${x.n}`).join(" ") });
	if (!conv.counts.channels) {
		say(notice(t("flash.channels.none"), "info"));
		S.forward(finish());
		return;
	}
	const run = async () => {
		S.forward(null);
		box.replaceChildren(prog.el);
		try {
			const radio = read.data.subarray(0, m.readSpan(null));
			const sectors = m.migrationSectors(conv.image, radio, S.board?.protect);
			await prepareStorage(box);	// the radio refuses every codeplug write until its settings area is RadioFoundry's
			const refused = await writeCodeplug(link, sectors, (d, n) => prog.set(d, n, t("flash.channels.writing")));
			const back = await link.cpsDump(1, 0, m.readSpan(null), { progress: (d, n) => prog.set(d, n, t("flash.channels.checking")) });
			const check = m.checkMigration(sectors, back.data, conv.counts);
			prog.el.remove();
			diag.note("channels written", { sectors: sectors.length, refused: refused.length, differ: check.differ.length, have: check.have });
			if (refused.length || !back.complete || !check.ok) {
				S.reportResult = "problem";
				say(notice(failed("flash.channels.check_failed", copy, { want: counts(conv.counts), have: check.have ? counts(check.have) : "?" }), "bad"),
					reportLine());
			} else say(notice(t("flash.channels.done", { counts: counts(conv.counts) }), "ok"), notes);
		} catch (e) {
			diag.error("flash/channels write", e);
			prog.el.remove();
			S.reportResult = "problem";
			say(notice(failed("flash.channels.write_failed", copy), "bad"), connectError(e), reportLine());
		}
		S.forward(finish());
	};
	if (copy) return run();
	// no backup: the user decides, knowing the original codeplug is not saved anywhere
	say(h("p", {}, t("flash.channels.found", { counts: counts(conv.counts) })), notes, notice(t("flash.channels.no_backup"), "warn"));
	S.forward([button(t("flash.nav.finish_no_channels"), () => reset(), { quiet: true }),
		button(t("flash.channels.go"), run, { primary: true })]);
}

// ---------------------------------------------------------------------------------------------------------------
// backup task

function backupView() {
	const box = h("div", { class: "flasher" });
	const status = h("div");
	// the step asks what the radio runs, as the install does; each choice is a different way to read the radio
	const choices = ["rfoundry", ...(openfw.supported(S.board) ? ["openfw"] : []), "stock", "unknown"];
	const choose = (id) => () => {
		S.backupPick = id;
		S.now = id === "openfw" ? nowFor("openfw") : null;
		status.replaceChildren();
		draw();
	};
	const connect = button(t("flash.backup.connect"), async () => {
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw await noAnswer(link);
			if (!S.board.debuglink_names.includes(p.board)) throw new Error(t("flash.err.wrong_board", { board: p.board, name: S.board.name }));
			S.now = await rfoundryNow(link, p);
			status.replaceChildren(h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, firmwareName(p)),
				h("dt", {}, t("flash.now.codec")), h("dd", {}, p.codec || "?")),
			h("details", { class: "small" }, h("summary", {}, t("flash.backup.add_stock")),
				filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); })),
			backupNow({ when: "task", actions: (b) => S.forward(b),
				onDone: () => S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true })) }));
		} catch (e) {
			await dropSerial();
			if (openfwRecognized(e, status, () => { S.backupPick = "openfw"; draw(); })) return;
			status.replaceChildren(connectError(e), connectHelp(e, "flash.backup.openfw_next"));
		}
	}, { primary: true });
	const finish = () => S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
	const draw = () => {
		const pick = S.backupPick || null;
		const opts = h("div", { class: "choice-grid" }, choices.map((id) =>
			h("button", { class: "choice", type: "button", "aria-pressed": String(pick === id), onclick: choose(id) },
				h("strong", {}, t(`flash.now.${id}`)), h("span", { class: "small muted" }, t(`flash.backup.from.${id}_text`)))));
		let detail = null;
		if (pick === "rfoundry") {
			detail = h("div", {}, h("p", {}, t("flash.backup.task_lead")), status);
			// Connect is the step's action until the radio answers; then the backup's own run button takes its place
			if (S.now?.kind !== "rfoundry") S.forward(connect);
		} else if (pick === "openfw") {
			// the install's own backup of another open firmware (openfwBackupNow); done, only Finish is left
			detail = h("div", {}, openfwDetail({ draw, check: connectOpenfw, setRun: (b) => S.forward(b) }), status);
			if (!openfwRecognizedNow()) S.forward(null);
			else if (S.backupDone) finish();
		} else {
			detail = pick ? notice(t("flash.backup.stock_none"), "info") : null;
			S.forward(null);
		}
		box.replaceChildren(panel(t("flash.backup.task_title", { name: S.radio.name }), opts, detail));
	};
	// "Check the Radio" on another open firmware: a probe that answers with its CPS is what openfwRecognized takes
	async function connectOpenfw() {
		status.replaceChildren(h("p", { class: "muted" }, t("flash.now.connecting")));
		try {
			const link = await connectSerial();
			const p = await probe(link);
			if (!p) throw await noAnswer(link);
			// the debug link answered: this is RadioFoundry, not another open firmware
			await dropSerial();
			status.replaceChildren(notice(t("flash.backup.is_rfoundry"), "info"));
		} catch (e) {
			await dropSerial();
			if (openfwRecognized(e, status, draw)) return;
			status.replaceChildren(connectError(e), serialHelp());
		}
	}
	draw();
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// restore task

// A radio whose backend restores from the original firmware file (vendorRestore) takes that file here too: a .bin,
// or a zip without the backup's backup.json (bundleFolder)
function bundleView() {
	const status = h("div");
	const vendor = S.radio && backend().vendorRestore;
	return panel(t("flash.restore.title"), h("p", {}, t(vendor ? "flash.restore.lead_or_file" : "flash.restore.lead", { radio: S.radio?.name })),
		filePicker(t(vendor ? "flash.restore.pick_or_file" : "flash.restore.pick"), vendor ? ".zip,.bin" : ".zip", async (f) => {
			try {
				if (vendor && bundleFolder((await unzip(f.data).catch(() => null)) || new Map()) === null) return await vendorRestoreFile(f, status);
				S.bundle = await openBundle(f.data);
				const r = S.data.radios.find((x) => x.id === S.bundle.meta.radio.board);
				if (!r) throw new Error(t("flash.err.unknown_board", { board: S.bundle.meta.radio.board }));
				selectRadio(r);
				const st = S.bundle.parts.get("stock-firmware");
				// the stock file the install used, identified as a picked file is (any build of this radio restores)
				if (st) S.stock = await pickedStock({ name: "stock-firmware.bin", data: st.data });
				next();
			} catch (e) {
				status.replaceChildren(failureNotice(e, vendor ? f : null));
			}
		}), status);
}

// The original firmware file for the restore: any build for this radio does (no codec is taken from it). Anything
// else is explained as on the install's codec step, with the same offer to send the file
async function vendorRestoreFile(f, status) {
	const boardId = S.radio.flash.board;
	const opened = await openVendorFile(f.name, f.data, fb(), boardId);
	S.vendorPick = { picked: { name: f.name, size: f.data.length, sha256: await sha256(f.data) }, opened, decision: "restore" };
	const problem = pickProblem(opened, boardId, { need: "key" });
	// the restore's file is a pick like the codec's: logged with what the page made of it, or why it was refused
	diag.note(...crumbs.filePicked(S.vendorPick.picked, opened.best || { problem, why: whyNot(opened) }));
	if (problem) return status.replaceChildren(problemNotice(problem, { offer: analysisCandidate(opened, fb(), boardId), boardId }));
	S.bundle = backend().vendorRestore(S.radio, opened.best, fb());
	next();
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
	// a backup taken on another open firmware can put that firmware back; no other bundle offers it
	const choices = h("div", { class: "choice-grid" }, ["rfoundry", "codeplug", "stock", ...(avail.includes("openfw") ? ["openfw"] : [])].map((tg) =>
		h("button", { class: "choice", type: "button", disabled: !avail.includes(tg), "aria-pressed": String(S.restoreTarget === tg),
			onclick: () => { S.restoreTarget = tg; render(); } },
		h("strong", {}, t(`flash.restore.to_${tg}`)), h("span", { class: "small muted" }, t(`flash.restore.to_${tg}_text`)))));
	const parts = [panel(t("flash.restore.what"), summary, choices)];
	// the stock file: the application to write back, or (any target) where the bootloader key comes from
	if (S.restoreTarget === "codeplug") parts.push(afterRestore());
	else if (S.restoreTarget && !stockForRadio(S.stock) && (S.restoreTarget === "stock" || (!S.sim && !S.key && !savedKey(S.board))))
		parts.push(panel(t("flash.codec.stock_title"), h("p", {}, t("flash.restore.stock_needed")), stockPickProblem(),
			filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); render(); })));
	else if (S.restoreTarget) {
		parts.push(bootloaderPanel(t("flash.restore.write_title"), async (key) =>
			(S.restoreTarget === "rfoundry" || S.restoreTarget === "openfw" ? planInternal(b, S.board) : planStock(S.stock.data, key, S.board, fb())), () => {
			if (S.fakeRadio && S.restoreTarget === "stock") S.fakeRadio.running = "stock";
			if (S.fakeRadio && S.restoreTarget === "openfw") S.fakeRadio.running = "openfw";
			box.append(afterRestore());
		}, { task: "restore", source: S.restoreTarget, radio: S.board.id,
			firmware: S.restoreTarget === "stock" ? "stock" : describeFirmware(meta.firmware_now) }));
	}
	box.append(...parts);
	return box;
}

function afterRestore() {
	// the other open firmware is back; the memory chip was not written (restore.js): its own programming software can
	// write its channels back
	if (S.restoreTarget === "openfw") {
		S.forward(button(t("flash.nav.finish"), () => reset(), { primary: true }));
		return panel(t("flash.restore.done_openfw_title"), h("p", {}, t("flash.restore.done_openfw")));
	}
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
			const sectors = codeplugSectors(spi.data, now.data, S.board?.protect);
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
// through an update mode the radio stays in when its application is incomplete (recovery.extra, stuck); it is listed
// after the dark screen it is mistaken for
const symptoms = () => (backend().recovery.extra?.(S.radio, fb()).some((x) => x.stuck) ? ["dark", "iap", ...SYMPTOMS.slice(1)] : SYMPTOMS);

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
// The USB device in use (adapter.js): on the trail, as a tag on error reports, in the flash log and problem reports
function useAdapter(a) {
	if (!a) {
		// a port without USB ids (the "all ports" choice) after a cable: no stale chip, no stale CH340 banner
		if (S.adapter) {
			S.adapter = null;
			setAdapter(null);
			diag.note("adapter none (no USB ids)");
		}
		return;
	}
	if (S.adapter && adapter.describe(S.adapter) === adapter.describe(a)) return;
	S.adapter = a;
	setAdapter(a);
	diag.note(`adapter ${adapter.describe(a)}`);
}

// The codec decision and why, on the trail (decide.js); err: why there is none
function noteDecision(err = null) {
	diag.note(...crumbs.decision(S.decision, { err, stock: !!S.stock?.data, saved: S.savedCodec?.from || null }));
}

// What the write erases and writes, for the trail and the problem report
function notePlan(p, prof = null) {
	const [line] = crumbs.plan(p, prof);
	S.planText = line.replace(/^write plan /, "");
	diag.note(line);
}

// For a problem report (report.js, problemreport.js): what the page decided and the files involved, by hash only
function reportMeta() {
	const file = (role, name, data, sha, id) => (data ? { role, name: String(name || "").slice(0, 200), len: data.length, sha256: sha || undefined, id: id || undefined } : null);
	const files = [
		file("stock", S.stock?.name, S.stock?.data, S.stock?.info?.sha, S.stock ? logId(S.stock) : null),
		file("readout", "internal-flash.bin", S.readoutBytes, S.readout?.sha256, S.readout?.id),
		file("backup", S.lastBackup?.name, S.lastBackup?.data, null, null),
	].filter(Boolean);
	const last = diag.lastError();
	return {
		tool: "web", tool_version: S.data?.site?.version || S.data?.site?.git || "", board: S.board?.id || S.radio?.id || "", task: S.task || "",
		step: String(stepsFor()?.[S.step] ?? S.step ?? ""), flash_id: S.lastFlashId || undefined, adapter: S.adapter || undefined,
		result: S.reportResult || "failed", error: last ? `${last.stage}: ${last.message}` : undefined,
		decisions: { codec_mode: S.decision?.mode || "", codec_source: S.decision ? S.decision.source || "stock" : "", key_source: S.keySource || "",
			plan: (S.planText || "").slice(0, 300), files },
	};
}

// The files the user can attach to a problem report, all checked: the vendor file picked, the internal flash read, the
// backup the page made
function reportAttachments() {
	const out = [];
	if (S.picked?.data) out.push({ name: S.picked.name, kind: "stock", size: S.picked.data.length, checked: true,
		label: t("report.attach_stock", { name: S.picked.name }), data: () => S.picked.data });
	if (S.readoutBytes) out.push({ name: "internal-flash.bin", kind: "readout", size: S.readoutBytes.length, checked: true,
		label: t("report.attach_readout"), data: () => S.readoutBytes });
	// the program memory read through a stock build the page had not loaded its stub through before (c7000ui.js)
	if (S.stockRead?.data) out.push({ name: S.stockRead.name, kind: "stock", size: S.stockRead.data.length, checked: true,
		label: t("report.attach_stock_read", { name: S.stockRead.name }), data: () => S.stockRead.data });
	if (S.lastBackup?.data) out.push({ name: S.lastBackup.name, kind: "backup", size: S.lastBackup.data.length, checked: true,
		label: t("report.attach_backup", { name: S.lastBackup.name }), data: () => S.lastBackup.data });
	return out;
}

function reportContext() {
	const p = S.now?.probe;
	return {
		adapter: S.adapter ? adapter.describe(S.adapter) : null,
		task: S.task, step: stepsFor()?.[S.step] ?? S.step, radio: S.radio?.id || null, board: S.board?.id || null, sim: !!S.sim,
		version: VERSION_REF || null,
		firmware: S.firmware ? { source: S.firmware.source, version: S.firmware.manifest?.version, git: S.firmware.manifest?.git } : null,
		now: S.now ? { kind: S.now.kind, openfw: !!S.now.openfw, version: S.now.version || null, board: p?.board, git: p?.git, features: p?.features, codec: p?.codec } : null,
		// the vendor file picked and what was found in it (names, sizes, hashes; never its bytes): enough to tell a wrong
		// radio's file, a build without a codec map or a fault in the page apart from the report alone
		vendor_file: vendorContext({ boardId: S.board?.id || null, ...(S.vendorPick || {}), keySource: S.keySource,
			decision: S.decision ? { mode: S.decision.mode, source: S.decision.source || null } : S.vendorPick?.decision || null }),
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
		withPicture(h("ol", { class: "steps" }, S.radio.flash.bootloader.map((x) => h("li", {}, x)), h("li", {}, t("flash.help.um_then")))),
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
		withPicture(h("ol", { class: "steps small" }, S.radio.flash.bootloader.map((x) => h("li", {}, x)))),
		h("p", { class: "small muted" }, t("flash.help.iap_original")));
}

// The board's supported stock files as a short text: their names when there are a few, else how many there are and
// their versions (the codec page lists every name under it)
const STOCK_NAMES_MAX = 6;
function stockFiles() {
	const names = S.board.codec.sources;
	if (names.length <= STOCK_NAMES_MAX) return names.join(", ");
	const versions = [...new Set(Object.values(fb().profiles).filter((p) => p.board === S.board.id)
		.map((p) => p.version.match(/\d+\.\d+/)?.[0]).filter(Boolean))].sort((a, b) => parseFloat(a) - parseFloat(b));
	return t("flash.codec.stock_versions", { n: names.length, versions: versions.join(", ") });
}

// A picked stock file that can't restore the original firmware (not this radio's, not a vendor file, damaged): why,
// and what each file in the pick is
function stockPickProblem() {
	if (!S.stock || stockForRadio(S.stock)) return null;
	const p = S.stock.keyProblem || { problem: "not-vendor", file: S.stock.name };
	return h("div", {}, problemNotice(p, { offer: S.stock.analysis }), S.stock.report?.length
		? h("details", { class: "small" }, h("summary", {}, t("flash.codec.stock_report")), h("ul", {}, S.stock.report.map((l) => h("li", {}, l)))) : null);
}

// STM32 radios back on their original firmware, from the owner's stock file, through update mode (no backup needed).
// Any build of this radio does, mapped or not: the bootloader key and the application come from it, not the codec
function stockRecovery(go) {
	if (!stockForRadio(S.stock)) {
		const picker = S.sim ? button(t("flash.sim.use_stock"), async () => { S.stock = { name: "simulated-stock.bin", data: S.sim.stock, info: await stockfw.identify(S.sim.stock, fb()) }; render(); })
			: filePicker(t("flash.codec.stock_pick"), ".bin,.zip", async (f) => { S.stock = await pickedStock(f); render(); });
		const where = S.radio.flash.stock_download ? h("a", { href: S.radio.flash.stock_download, target: "_blank", rel: "noopener" }, t("flash.codec.stock_download")) : null;
		return panel(t("flash.help.stock_title"), h("p", {}, t("flash.help.stock_need", { radio: S.board.name, files: stockFiles() }), " ", where),
			stockPickProblem(), picker);
	}
	return bootloaderPanel(t("flash.help.stock_title"), async (key) => planStock(S.stock.data, key, S.board, fb()), () => {
		if (S.fakeRadio) S.fakeRadio.running = "stock";
		go("fixed", { how: "stock" });
	}, { task: "restore", source: "stock", radio: S.board.id, firmware: "stock" });
}

// practice mode (#sim...): a problem report is answered in the page, nothing sent, also before the simulation is up
const SIM_HASH = /(?:^#|&)sim(?:=|&|$)/;

// ---------------------------------------------------------------------------------------------------------------
// the manual install kit (manual.js): in place of a step this browser can't run, and under a failed install

// The stock file the kit carries: the one picked for the codec (the inner vendor file of a zip), or the file the C7000
// codec step took; none when it gave no codec
function kitStock() {
	if (S.stock?.data && !S.stock.problem) return { name: S.stock.name, data: S.stock.data };
	const best = S.vendorPick?.opened?.best;
	return best?.match?.kind === "codec" && !S.vendorPick.problem ? { name: best.path.split("/").pop(), data: best.data } : null;
}

const kitArgs = () => ({ radio: S.radio, board: S.board, firmware: S.firmware, stock: kitStock(), site: S.data.site, root: ROOT,
	url: new URL(ROOT || "./", location.href).href });

// Under a failed install (also c7000ui.js): the kit, for a radio rfoundry installs. Never in practice mode, whose image
// is random bytes
function manualKitLine() {
	return S.sim || !S.radio ? null : kitLine(kitArgs);
}

function kitView(api) {
	const out = h("div", { class: "flasher" });
	const draw = () => {
		const { el, action } = kitPanel(kitArgs, api, { pickStock: async (f) => { S.stock = await pickedStock(f); draw(); } });
		out.replaceChildren(el);
		S.forward(action);
	};
	draw();
	return out;
}

// ---------------------------------------------------------------------------------------------------------------

async function main() {
	showBetaNotice();	// before the data loads: the first thing a visitor reads, once per session
	initReporting({ tool: "flasher", context: reportContext, attachments: reportAttachments, backend: { api: API, meta: reportMeta, practice: () => !!S.sim || SIM_HASH.test(location.hash) } });
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
	c7ui.init({ S, next, render, reset, helpLink, manualKitLine, fb, flashStarted, flashFinished, flashMark, noteReadout, errorText, analysisOffer, problemNotice, failureNotice, useAdapter,
		stubJson: () => fetch(`${ROOT}data/c7000-stub.json`).then((r) => r.json()) });
	// #sim=<board>[-stock|-romfail|-slowcable|-openfw|-openfw-nocodec]: that board, running RadioFoundry, its stock firmware or another
	// open firmware (a board the page backs up from one: openfw.js; -nocodec: one without the codec, for the stock-file
	// fallback); -romfail: an HR_C7000 radio on stock firmware whose boot ROM refuses the load, for the factory update
	// mode; -slowcable: one on a cable too slow for the boot ROM's answer window, which the answer sent ahead gets through; #sim: the first
	// radio on stock firmware
	const simAsk = location.hash.match(/(?:^#|&)sim(?:=(\w+?)(-stock|-romfail|-slowcable|-openfw(?:-nocodec)?)?)?(?:&|$)/);
	const simRadio = simAsk && (simAsk[1] ? S.data.radios.find((r) => r.id === simAsk[1]) : S.data.radios[0]);
	const simOpen = !!simAsk?.[2]?.startsWith("-openfw") && openfw.supported(S.data.fb.boards[simRadio?.flash.board]);
	const simStock = !!simAsk && (!simAsk[1] || (!!simAsk[2] && !simOpen));
	if (simRadio && c7ui.isC7000(simRadio, S.data.fb)) {
		// on its cable: stock firmware (a stock console answers if the board has one), or RadioFoundry with the stub hand-over
		const board = simRadio.id;
		const sim = await c7000Simulation(S.data.fb.c7000, board, 7000, simRadio);
		S.sim = { fb: { ...S.data.fb, c7000: sim.c } };
		// -slowcable: the radio's own 48 ms window, and a cable that takes 80 ms
		const slow = simAsk[2] === "-slowcable";
		S.fakeC7 = new FakeC7000({ progmem: sim.progmem, speed: 8, stubEntry: true, romWindow: slow ? 48 : 600,
			faults: { silentAfterReset: true, romNak: simAsk[2] === "-romfail", slowCable: slow ? 80 : 0 },
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
		S.fakeRadio = new FakeRadio(S.sim, { state: simOpen ? "openfw" : simStock ? "stock" : "rfoundry", spiSize: 0x400000,
			openfw: { codec: simAsk[2] !== "-openfw-nocodec" } });
		await S.fakeRadio.ready;
		// a smaller SPI flash keeps the simulated backup quick
		simRadio.backup = { ...simRadio.backup, spi_size: 0x400000 };
	}
	S.backupParts = [];
	const m = location.hash.match(/radio=(\w+)/);
	const r = m && S.data.radios.find((x) => x.id === m[1]);
	// #update: the Update task (the Codeplug Editor's Update Firmware button)
	if (/(?:^#|&)update(?:&|$)/.test(location.hash)) {
		S.task = "update";
		S.step = 0;
	} else if (r && available(r, "install")) {
		S.task = "install";
		selectRadio(r);
		S.step = 1;
	}
	render();
}

main();
