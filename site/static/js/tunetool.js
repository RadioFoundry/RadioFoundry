// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Power tuning (docs/spec/power-tune.md, docs/user/16-battery-and-power.md): the radio's transmit bands as tabs,
// up to three tuning frequencies per band, a card per power level. Each card keys a test transmission into the
// owner's dummy load, takes the wattmeter's reading, suggests the next adjustment and saves the result in the radio.
// Everything comes from the radio (bands, levels, names, limits), so a new radio, band or level needs no change here.

import { Link, SerialStream, RFOUNDRY_USB, CABLES, browserErrorKey, noDeviceChosen, probe, serialApi,
	serialSupported } from "./flasher/link.js";
import { page } from "./i18n.js";
import { TuneRadio, ST, NAME_MAX, cleanName } from "./tune-radio.js";
import { bandInfo, db, grade, mhzText, neighborSlope, parseMhz, parseReading, parseWatts, suggestDrive } from "./tune-math.js";

const { t, num, has } = page;
const $ = (id) => document.getElementById(id);
const FREQS_PER_BAND = 3;
const COOLDOWN_MS = 3000;
const EXPORT_FORMAT = "rfoundry-power-tune";
// Each board's display name, from the site build
const RADIO_NAMES = (() => {
	try { return JSON.parse($("tune-radios")?.textContent || "{}"); } catch (e) { return {}; }
})();

const el = {
	confirm: $("tune-confirm"), connect: $("tune-connect-btn"), radio: $("tune-radio"), radioName: $("tune-radio-name"),
	radioState: $("tune-radio-state"), status: $("tune-status"), work: $("tune-work"), bands: $("tune-bands"),
	range: $("tune-band-range"), strip: $("tune-strip"), chips: $("tune-chips"), add: $("tune-add"),
	addMhz: $("tune-add-mhz"), addErr: $("tune-add-err"), addCancel: $("tune-add-cancel"), levels: $("tune-levels"),
	levelsTitle: $("tune-levels-title"), hold: $("tune-hold"), advanced: $("tune-advanced"), saved: $("tune-saved"),
	savedSum: $("tune-saved-sum"), savedBody: document.querySelector("#tune-saved-table tbody"), exportBtn: $("tune-export"),
	importInput: $("tune-import"), clear: $("tune-clear"), tpl: $("tune-level-tpl"), dialog: $("tune-dialog"),
	dialogTitle: $("tune-dialog-title"), dialogText: $("tune-dialog-text"), dialogOk: $("tune-dialog-ok"),
};

const S = {
	stream: null, radio: null, probe: null, info: null, record: null,
	band: 0,			// the band tab shown
	active: new Map(),		// band -> the tuning frequency shown (kHz)
	added: new Map(),		// band -> frequencies added this session, not saved yet
	limits: [],			// per level at the active frequency: {factory, tuned, lo, hi, name}
	work: new Map(),		// "khz:level" -> {delta, readings: [{drive, watts}], lastDrive, reading}
	names: new Map(),		// "band:level" -> {name, target} typed, not saved yet
	busy: false, cooldownUntil: 0, keying: null,
};

// ---- small helpers

const key = (khz, level) => `${khz}:${level}`;
const fmtW = (w) => (w >= 1 ? num(+w.toPrecision(3)) : num(+w.toPrecision(2)));
const signed = (n) => (n > 0 ? `+${num(n)}` : num(n));
const bandOf = (khz) => S.info.bands.findIndex((b) => khz >= b.lo && khz <= b.hi);

function status(text, kind = "") {
	el.status.textContent = text;
	el.status.dataset.kind = kind;
}

function errorText(e) {
	if (e?.tuneStep === "tx") {
		const k = `tools.tune.deny.${e.deny}`;
		const why = !e.deny || e.deny === "none" ? t("tools.tune.deny.unknown") : has(k) ? t(k) : t("tools.tune.deny.other", { code: e.deny });
		return t("tools.tune.err.no_tx", { why });
	}
	if (e?.tuneStep) return t("tools.tune.err.step", { step: t(`tools.tune.step.${e.tuneStep}`) });
	switch (e?.status) {
	case ST.REFUSED: return t("tools.tune.err.refused", { n: S.info?.maxPoints ?? 0 });
	case ST.BAD_ARG: return t("tools.tune.err.bad_arg");
	case ST.UNAVAILABLE: return t("tools.tune.err.no_store");
	case ST.BUSY: return t("tools.tune.err.flash");
	default: break;
	}
	const browser = browserErrorKey(e);
	return browser ? t(browser) : t("tools.tune.err.link", { error: e?.message || String(e) });
}

function ask(title, text, ok) {
	el.dialogTitle.textContent = title;
	el.dialogText.textContent = text;
	el.dialogOk.textContent = ok;
	el.dialog.returnValue = "";
	el.dialog.showModal();
	return new Promise((resolve) => el.dialog.addEventListener("close", () => resolve(el.dialog.returnValue === "ok"), { once: true }));
}

// One radio operation at a time; the page shows what failed and stays usable
async function run(what, f) {
	if (S.busy) return false;
	S.busy = true;
	render();
	try {
		await f();
		return true;
	} catch (e) {
		status(errorText(e), "bad");
		if (e?.name === "NetworkError" || /closed|lost|disconnected/i.test(e?.message || "")) await disconnect(false);
		return false;
	} finally {
		S.busy = false;
		render();
	}
}

// ---- the radio's data

const bandNames = (b) => S.info.bands[b].names;
const factoryName = (b, level) => bandNames(b)[level] || t("tools.tune.level_n", { n: level + 1 });

function points(band, level) {
	return (S.record?.points || []).filter((p) => bandOf(p.khz) === band && (level == null || p.level === level));
}

function savedPoint(khz, level) {
	return (S.record?.points || []).find((p) => p.khz === khz && p.level === level) || null;
}

function savedLabel(band, level) {
	return (S.record?.labels || []).find((l) => bandOf(l.khz) === band && l.level === level && l.state === "applied") || null;
}

// The name and target the card shows: typed, else saved, else the factory name and the watts it names
function shownName(band, level) {
	const typed = S.names.get(key(band, level));
	if (typed) return typed;
	const l = savedLabel(band, level);
	const name = l?.name || factoryName(band, level);
	return { name, target: l?.mw ? l.mw / 1000 : parseWatts(name) };
}

function freqs(band) {
	const set = new Set(points(band).map((p) => p.khz));
	for (const f of S.added.get(band) || []) set.add(f);
	if (!set.size) set.add(bandInfo(S.info.bands[band].lo, S.info.bands[band].hi).khz);
	return [...set].sort((a, b) => a - b);
}

function activeKhz() {
	const fs = freqs(S.band);
	const a = S.active.get(S.band);
	return fs.includes(a) ? a : fs[0];
}

function work(khz, level) {
	const k = key(khz, level);
	if (!S.work.has(k)) {
		const p = savedPoint(khz, level);
		S.work.set(k, { delta: p && p.state === "applied" ? p.delta : 0, readings: [], lastDrive: 0, reading: "" });
	}
	return S.work.get(k);
}

async function refresh() {
	S.record = await S.radio.read();
	S.limits = await S.radio.limits(activeKhz());
}

// ---- connect

async function connect() {
	if (!serialSupported()) {
		status(t("flash.err.no_serial"), "bad");
		return;
	}
	let port;
	try {
		port = await serialApi().requestPort({ filters: [RFOUNDRY_USB, ...CABLES] });
	} catch (e) {
		if (!noDeviceChosen(e)) status(errorText(e), "bad");
		return;
	}
	await run("connect", async () => {
		status(t("tools.tune.status.connecting"));
		S.stream = await SerialStream.open(port);
		const link = new Link(S.stream);
		S.probe = await probe(link);
		if (!S.probe) throw new Error(t("tools.tune.err.not_radio"));
		S.radio = new TuneRadio(link);
		try {
			S.info = await S.radio.info();
		} catch (e) {
			if (e?.status === 1 || e?.status === ST.UNAVAILABLE) throw new Error(t("tools.tune.err.no_feature"));
			throw e;
		}
		if (!S.info.bands.length || !S.info.levels) throw new Error(t("tools.tune.err.no_bands"));
		S.band = 0;
		await refresh();
		status(S.info.store ? t("tools.tune.status.connected") : t("tools.tune.err.no_store"), S.info.store ? "ok" : "bad");
	});
	if (!S.info) await disconnect(false);
}

async function disconnect(restore = true) {
	const radio = S.radio, stream = S.stream;
	S.radio = S.stream = S.info = S.record = S.probe = null;
	S.work.clear();
	S.names.clear();
	S.added.clear();
	S.active.clear();
	if (radio && restore) await radio.restore();
	if (stream) {
		try { await stream.close(); } catch (e) { /* gone already */ }
	}
	render();
}

// ---- actions

async function transmit(level) {
	if (!el.confirm.checked) {
		status(t("tools.tune.status.confirm_first"), "bad");
		el.confirm.closest(".card").classList.add("tune-attention");
		el.confirm.focus();
		return;
	}
	const wait = S.cooldownUntil - Date.now();
	if (wait > 0) return;
	const khz = activeKhz(), w = work(khz, level), lim = S.limits[level];
	const hold = +el.hold.value;
	const ok = await run("key", async () => {
		S.keying = { level, left: hold };
		status(t("tools.tune.status.keying", { level: shownName(S.band, level).name, mhz: mhzText(khz) }), "tx");
		await S.radio.key(khz, level, w.delta, hold, (left) => {
			S.keying = { level, left };
			renderKeying();
		});
		w.lastDrive = Math.max(lim.lo, Math.min(lim.hi, lim.factory + w.delta));
		w.reading = "";
	});
	S.keying = null;
	S.cooldownUntil = Date.now() + COOLDOWN_MS;
	cooldown();
	if (ok) {
		status(t("tools.tune.status.read_meter"), "ok");
		const input = el.levels.querySelector(`[data-level="${level}"] .tune-reading`);
		input?.focus();
	}
}

function cooldown() {
	render();
	if (Date.now() < S.cooldownUntil) setTimeout(cooldown, 250);
}

function takeReading(level, text) {
	const w = work(activeKhz(), level);
	w.reading = text;
	const watts = parseReading(text);
	if (watts && w.lastDrive) {
		w.readings = w.readings.filter((r) => r.drive !== w.lastDrive);
		w.readings.push({ drive: w.lastDrive, watts });
	}
	renderLevel(level);
}

function suggest(level) {
	const khz = activeKhz(), w = work(khz, level), lim = S.limits[level];
	const { target } = shownName(S.band, level);
	const slope = neighborSlope(S.limits.map((l, i) => ({ factory: l.factory, watts: parseWatts(factoryName(S.band, i)) })), level);
	const d = suggestDrive(w.readings, target, { lo: lim.lo, hi: lim.hi, slope });
	if (d === null) return;
	const before = w.delta;
	w.delta = d - lim.factory;
	const hint = w.delta === before ? t("tools.tune.hint.at_limit") : t("tools.tune.hint.suggested", { delta: signed(w.delta) });
	renderLevel(level, hint);
}

function setDelta(level, delta) {
	const lim = S.limits[level];
	work(activeKhz(), level).delta = Math.max(lim.lo - lim.factory, Math.min(lim.hi - lim.factory, Math.round(delta) || 0));
	renderLevel(level);
}

function dirty(level) {
	const khz = activeKhz(), w = work(khz, level), p = savedPoint(khz, level);
	const typed = S.names.get(key(S.band, level));
	return (p ? p.state !== "applied" || p.delta !== w.delta : w.delta !== 0) || !!typed;
}

async function save(level) {
	const khz = activeKhz(), w = work(khz, level), band = S.band;
	const typed = S.names.get(key(band, level));
	await run("save", async () => {
		const p = savedPoint(khz, level);
		if (p ? p.state !== "applied" || p.delta !== w.delta : w.delta !== 0) {
			const last = w.readings.find((r) => r.drive === Math.max(S.limits[level].lo,
				Math.min(S.limits[level].hi, S.limits[level].factory + w.delta)));
			await S.radio.putPoint(khz, level, w.delta, last ? last.watts * 1000 : 0);
		}
		if (typed) {
			const fac = factoryName(band, level);
			const same = typed.name === fac && (typed.target || 0) === (parseWatts(fac) || 0);
			await S.radio.putLabel(khz, level, same ? "" : typed.name, same ? 0 : (typed.target || 0) * 1000);
			S.names.delete(key(band, level));
		}
		await refresh();
		status(t("tools.tune.status.saved", { level: shownName(band, level).name, mhz: mhzText(khz) }), "ok");
	});
}

async function reset(level) {
	const khz = activeKhz(), band = S.band;
	await run("reset", async () => {
		if (savedPoint(khz, level)) await S.radio.delPoint(khz, level);
		if (savedLabel(band, level)) await S.radio.putLabel(khz, level, "", 0);
		S.names.delete(key(band, level));
		S.work.delete(key(khz, level));
		await refresh();
		status(t("tools.tune.status.reset", { level: factoryName(band, level) }), "ok");
	});
}

async function removeFreq(khz) {
	const pts = (S.record?.points || []).filter((p) => p.khz === khz);
	if (pts.length && !await ask(t("tools.tune.remove.title", { mhz: mhzText(khz) }), t("tools.tune.remove.text"), t("tools.tune.remove.ok")))
		return;
	const band = S.band;
	await run("remove", async () => {
		for (const p of pts) await S.radio.delPoint(p.khz, p.level);
		S.added.set(band, (S.added.get(band) || []).filter((f) => f !== khz));
		for (const k of [...S.work.keys()]) if (k.startsWith(`${khz}:`)) S.work.delete(k);
		S.active.delete(band);
		await refresh();
	});
}

async function selectFreq(khz) {
	S.active.set(S.band, khz);
	await run("limits", async () => { S.limits = await S.radio.limits(khz); });
}

async function selectBand(b) {
	S.band = b;
	await run("limits", async () => { S.limits = await S.radio.limits(activeKhz()); });
	el.bands.querySelector(`[data-band="${b}"]`)?.focus();
}

function addFreq(ev) {
	ev.preventDefault();
	const band = S.info.bands[S.band];
	const khz = parseMhz(el.addMhz.value);
	if (!khz || khz < band.lo || khz > band.hi) {
		el.addErr.textContent = t("tools.tune.freqs.out_of_band", { lo: mhzText(band.lo), hi: mhzText(band.hi) });
		return;
	}
	if (freqs(S.band).includes(khz)) {
		el.addErr.textContent = t("tools.tune.freqs.exists");
		return;
	}
	S.added.set(S.band, [...(S.added.get(S.band) || []), khz]);
	el.add.hidden = true;
	selectFreq(khz);
}

async function clearAll() {
	if (!await ask(t("tools.tune.clear.title"), t("tools.tune.clear.text"), t("tools.tune.clear.ok"))) return;
	await run("clear", async () => {
		await S.radio.clear();
		S.work.clear();
		S.names.clear();
		S.added.clear();
		await refresh();
		status(t("tools.tune.status.cleared"), "ok");
	});
}

function exportFile() {
	const data = { format: EXPORT_FORMAT, version: 1, board: S.probe.board, uid: S.probe.chip?.uid || "",
		firmware: S.probe.git || "", points: S.record?.points || [], labels: S.record?.labels || [] };
	const blob = new Blob([JSON.stringify(data, null, 2)], { type: "application/json" });
	const a = document.createElement("a");
	a.href = URL.createObjectURL(blob);
	a.download = `power-tuning-${S.probe.board}-${(S.probe.chip?.uid || "").slice(-6) || "radio"}.json`;
	a.click();
	setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function importFile(file) {
	let data;
	try {
		data = JSON.parse(await file.text());
	} catch (e) {
		status(t("tools.tune.import.bad"), "bad");
		return;
	}
	if (data?.format !== EXPORT_FORMAT || !Array.isArray(data.points) || !Array.isArray(data.labels)) {
		status(t("tools.tune.import.bad"), "bad");
		return;
	}
	// Tuning belongs to one radio: another radio's adjustments would be wrong here, so the file must be this radio's
	if (data.board !== S.probe.board || !data.uid || data.uid !== (S.probe.chip?.uid || "")) {
		status(t("tools.tune.import.other"), "bad");
		return;
	}
	if (!await ask(t("tools.tune.import.title"), t("tools.tune.import.text"), t("tools.tune.import.ok"))) return;
	let put = 0, skipped = 0;
	await run("import", async () => {
		await S.radio.clear();
		const lims = new Map();
		const limitsAt = async (khz) => {
			if (!lims.has(khz)) lims.set(khz, await S.radio.limits(khz));
			return lims.get(khz);
		};
		// Only what was tuned against the factory drive this radio has now: anything else would be stale
		for (const p of data.points) {
			const l = (await limitsAt(p.khz))[p.level];
			if (!l || l.factory !== p.factory || p.state !== "applied") { skipped++; continue; }
			await S.radio.putPoint(p.khz, p.level, p.delta, p.mw || 0);
			put++;
		}
		for (const l of data.labels) {
			const lim = (await limitsAt(l.khz))[l.level];
			if (!lim || lim.factory !== l.factory || l.state !== "applied") { skipped++; continue; }
			await S.radio.putLabel(l.khz, l.level, l.name, l.mw || 0);
			put++;
		}
		S.work.clear();
		S.names.clear();
		await refresh();
		status(t("tools.tune.import.done", { n: put, skipped }), skipped ? "bad" : "ok");
	});
}

// ---- drawing

function render() {
	const on = !!S.info;
	el.connect.textContent = on ? t("tools.tune.disconnect") : t("tools.tune.connect");
	el.connect.classList.toggle("btn-primary", !on);
	el.connect.disabled = S.busy && !on;
	el.radio.hidden = !on;
	el.work.hidden = !on;
	if (!on) return;
	const n = S.record?.points.filter((p) => p.state === "applied").length || 0;
	const stale = (S.record?.points || []).filter((p) => p.state !== "applied").length;
	el.radioName.textContent = RADIO_NAMES[S.probe.board] || S.probe.board;
	el.radioState.textContent = !S.info.store ? t("tools.tune.state.none") : n ? t("tools.tune.state.tuned", { n }) :
		t("tools.tune.state.factory");
	el.radioState.className = `badge ${!S.info.store ? "badge-planned" : n ? "badge-supported" : "badge-testing"}`;
	el.radioState.title = stale ? t("tools.tune.state.stale", { n: stale }) : "";
	renderBands();
	renderFreqs();
	renderLevels();
	renderSaved();
}

function renderBands() {
	el.bands.replaceChildren(...S.info.bands.map((b, i) => {
		const info = bandInfo(b.lo, b.hi);
		const tab = document.createElement("button");
		tab.type = "button";
		tab.role = "tab";
		tab.className = "tune-band";
		tab.dataset.band = i;
		tab.ariaSelected = String(i === S.band);
		tab.tabIndex = i === S.band ? 0 : -1;
		const tuned = points(i).filter((p) => p.state === "applied").length;
		tab.innerHTML = `<strong></strong><span></span>`;
		tab.querySelector("strong").textContent = info.name;
		if (tuned) tab.querySelector("strong").insertAdjacentHTML("beforeend", '<i class="tune-dot" aria-hidden="true"></i>');
		tab.querySelector("span").textContent = t("tools.tune.band_range", { lo: mhzText(b.lo), hi: mhzText(b.hi) });
		tab.disabled = S.busy;
		tab.addEventListener("click", () => selectBand(i));
		return tab;
	}));
	el.bands.hidden = S.info.bands.length < 2;
}

const SVG = "http://www.w3.org/2000/svg";
function svg(tag, attrs, text) {
	const e = document.createElementNS(SVG, tag);
	for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
	if (text != null) e.textContent = text;
	return e;
}

function renderFreqs() {
	const b = S.info.bands[S.band], info = bandInfo(b.lo, b.hi), fs = freqs(S.band), act = activeKhz();
	el.range.textContent = t("tools.tune.band_range", { lo: mhzText(b.lo), hi: mhzText(b.hi) });
	// The band strip: the band's edges, the amateur band inside, the tuning frequencies; solid between them (the radio
	// blends), dashed past the outer ones (held)
	// Drawn at the strip's own width, so the labels keep their shape on any screen
	const W = Math.max(240, Math.round(el.strip.clientWidth || 600)), x = (khz) => 12 + (W - 24) * (khz - b.lo) / Math.max(1, b.hi - b.lo);
	const kids = [el.strip.querySelector("title")];
	const ham = [[28000, 29700], [50000, 54000], [144000, 148000], [219000, 225000], [420000, 450000], [902000, 928000]]
		.find(([lo, hi]) => lo >= b.lo && hi <= b.hi);
	kids.push(svg("rect", { x: 12, y: 26, width: W - 24, height: 6, rx: 3, class: "strip-band" }));
	if (ham) kids.push(svg("rect", { x: x(ham[0]), y: 24, width: x(ham[1]) - x(ham[0]), height: 10, rx: 3, class: "strip-ham" }));
	if (fs.length) {
		const lo = x(fs[0]), hi = x(fs[fs.length - 1]);
		kids.push(svg("line", { x1: 12, x2: lo, y1: 29, y2: 29, class: "strip-held" }));
		kids.push(svg("line", { x1: hi, x2: W - 12, y1: 29, y2: 29, class: "strip-held" }));
		if (fs.length > 1) kids.push(svg("line", { x1: lo, x2: hi, y1: 29, y2: 29, class: "strip-blend" }));
	}
	for (const f of fs) {
		kids.push(svg("circle", { cx: x(f), cy: 29, r: f === act ? 7 : 5, class: f === act ? "strip-pt strip-act" : "strip-pt" }));
		kids.push(svg("text", { x: x(f), y: 14, "text-anchor": "middle", class: "strip-lbl" }, mhzText(f)));
	}
	kids.push(svg("text", { x: 12, y: 56, class: "strip-edge" }, mhzText(b.lo)));
	kids.push(svg("text", { x: W - 12, y: 56, "text-anchor": "end", class: "strip-edge" }, mhzText(b.hi)));
	if (ham) kids.push(svg("text", { x: (x(ham[0]) + x(ham[1])) / 2, y: 56, "text-anchor": "middle", class: "strip-edge" }, info.ham));
	el.strip.setAttribute("viewBox", `0 0 ${W} 64`);
	el.strip.replaceChildren(...kids);

	const chips = fs.map((f) => {
		const chip = document.createElement("span");
		chip.className = `tune-chip${f === act ? " is-active" : ""}`;
		const pick = document.createElement("button");
		pick.type = "button";
		pick.textContent = t("tools.tune.mhz", { mhz: mhzText(f) });
		pick.ariaPressed = String(f === act);
		pick.disabled = S.busy;
		pick.addEventListener("click", () => selectFreq(f));
		chip.append(pick);
		if (fs.length > 1 || (S.record?.points || []).some((p) => p.khz === f)) {
			const x = document.createElement("button");
			x.type = "button";
			x.className = "tune-chip-x";
			x.textContent = "×";
			x.ariaLabel = t("tools.tune.freqs.remove", { mhz: mhzText(f) });
			x.disabled = S.busy;
			x.addEventListener("click", () => removeFreq(f));
			chip.append(x);
		}
		return chip;
	});
	if (fs.length < FREQS_PER_BAND) {
		const add = document.createElement("button");
		add.type = "button";
		add.className = "btn btn-quiet tune-chip-add";
		add.textContent = t("tools.tune.freqs.add");
		add.disabled = S.busy;
		add.addEventListener("click", () => {
			el.add.hidden = false;
			el.addErr.textContent = "";
			el.addMhz.value = "";
			el.addMhz.placeholder = mhzText(b.lo + (b.hi - b.lo) * (fs.length === 1 ? 0.85 : 0.15));
			el.addMhz.focus();
		});
		chips.push(add);
	}
	const used = S.record?.points.length || 0;
	const cap = document.createElement("span");
	cap.className = "small muted tune-cap";
	cap.textContent = t("tools.tune.freqs.capacity", { n: used, max: S.info.maxPoints });
	chips.push(cap);
	el.chips.replaceChildren(...chips);
}

function renderLevels() {
	const khz = activeKhz();
	el.levelsTitle.textContent = t("tools.tune.levels_at", { mhz: mhzText(khz) });
	if (el.levels.dataset.at !== `${S.band}:${khz}:${S.limits.length}`) {
		el.levels.dataset.at = `${S.band}:${khz}:${S.limits.length}`;
		el.levels.replaceChildren(...S.limits.map((_, level) => buildLevel(level)));
	}
	S.limits.forEach((_, level) => renderLevel(level));
}

function buildLevel(level) {
	const card = el.tpl.content.firstElementChild.cloneNode(true);
	card.dataset.level = level;
	const q = (c) => card.querySelector(c);
	q(".tune-name").maxLength = Math.min(NAME_MAX, S.info.nameLen || NAME_MAX);
	q(".tune-name").ariaLabel = t("tools.tune.name_label");
	q(".tune-name").addEventListener("input", (e) => {
		const cur = shownName(S.band, level);
		const name = cleanName(e.target.value);
		const parsed = parseWatts(name);
		S.names.set(key(S.band, level), { name: name || factoryName(S.band, level), target: parsed ?? cur.target });
		renderLevel(level);
	});
	q(".tune-target").addEventListener("change", (e) => {
		const cur = shownName(S.band, level);
		const w = parseReading(e.target.value);
		S.names.set(key(S.band, level), { name: cur.name, target: w });
		renderLevel(level);
	});
	q(".tune-reading").addEventListener("input", (e) => takeReading(level, e.target.value));
	q(".tune-reading").addEventListener("keydown", (e) => { if (e.key === "Enter") suggest(level); });
	q(".tune-slider").addEventListener("input", (e) => setDelta(level, +e.target.value));
	q(".tune-delta").addEventListener("change", (e) => setDelta(level, +e.target.value));
	card.querySelectorAll(".tune-step").forEach((b) => b.addEventListener("click", (e) => {
		const step = +b.dataset.step * (e.shiftKey ? 10 : 1);
		setDelta(level, work(activeKhz(), level).delta + step);
	}));
	q(".tune-key").addEventListener("click", () => transmit(level));
	q(".tune-suggest").addEventListener("click", () => suggest(level));
	q(".tune-save").addEventListener("click", () => save(level));
	q(".tune-reset").addEventListener("click", () => reset(level));
	return card;
}

function renderLevel(level, hint) {
	const card = el.levels.querySelector(`[data-level="${level}"]`);
	if (!card) return;
	const q = (c) => card.querySelector(c);
	const khz = activeKhz(), lim = S.limits[level], w = work(khz, level), p = savedPoint(khz, level);
	const shown = shownName(S.band, level), fac = factoryName(S.band, level);
	const transmits = lim.factory > 0;
	card.classList.toggle("is-off", !transmits);
	card.classList.toggle("is-keying", S.keying?.level === level);

	const name = q(".tune-name");
	if (document.activeElement !== name) name.value = shown.name;
	q(".tune-factory").textContent = shown.name !== fac ? t("tools.tune.factory_name", { name: fac }) : "";
	const target = q(".tune-target");
	if (document.activeElement !== target) target.value = shown.target ? fmtW(shown.target) : "";
	const reading = q(".tune-reading");
	if (document.activeElement !== reading) reading.value = w.reading;
	reading.disabled = !w.lastDrive;
	reading.placeholder = w.lastDrive ? "" : t("tools.tune.reading_after");

	// The badge: what the radio does with this level here
	const badge = q(".tune-badge");
	const isDirty = dirty(level);
	const [cls, text] = !transmits ? ["badge-planned", t("tools.tune.badge.off")] :
		isDirty ? ["badge-testing", t("tools.tune.badge.changed")] :
		p?.state === "stale" ? ["badge-planned", t("tools.tune.badge.stale")] :
		p ? ["badge-supported", t("tools.tune.badge.tuned")] : ["badge-neutral", t("tools.tune.badge.factory")];
	badge.className = `badge tune-badge ${cls}`;
	badge.textContent = text;
	badge.title = p?.state === "stale" ? t("tools.tune.badge.stale_help") : "";

	// The error: the reading against the target, once both are known
	const watts = parseReading(w.reading);
	const out = q(".tune-error");
	const g = grade(watts, shown.target);
	out.className = `tune-error${g ? ` is-${g}` : ""}`;
	out.textContent = g ? t(`tools.tune.grade.${g}`, { db: `${db(watts, shown.target) > 0 ? "+" : ""}${num(+db(watts, shown.target).toFixed(1))}` }) : "";

	// The adjustment, within what the radio allows
	const lo = lim.lo - lim.factory, hi = lim.hi - lim.factory;
	const slider = q(".tune-slider"), delta = q(".tune-delta");
	slider.min = delta.min = lo;
	slider.max = delta.max = hi;
	slider.value = w.delta;
	if (document.activeElement !== delta) delta.value = w.delta;
	const fixed = lo === hi;
	slider.disabled = delta.disabled = fixed || !transmits;
	card.querySelectorAll(".tune-step").forEach((b) => {
		b.disabled = fixed || !transmits || (+b.dataset.step < 0 ? w.delta <= lo : w.delta >= hi);
	});
	slider.style.setProperty("--fill", `${hi > lo ? ((w.delta - lo) / (hi - lo)) * 100 : 50}%`);
	slider.style.setProperty("--zero", `${hi > lo ? ((0 - lo) / (hi - lo)) * 100 : 50}%`);
	const drive = Math.max(lim.lo, Math.min(lim.hi, lim.factory + w.delta));
	q(".tune-drive").textContent = el.advanced.checked ?
		t("tools.tune.drive", { drive, factory: lim.factory, lo: lim.lo, hi: lim.hi }) :
		w.delta >= hi && hi === 0 ? t("tools.tune.top_limit") : w.delta <= lo && lo === 0 ? t("tools.tune.bottom_limit") : "";

	// The buttons
	const cool = Math.max(0, S.cooldownUntil - Date.now());
	const keyBtn = q(".tune-key");
	keyBtn.disabled = S.busy || !transmits || cool > 0;
	keyBtn.textContent = S.keying?.level === level ? t("tools.tune.keying", { s: Math.ceil(S.keying.left / 1000) }) :
		cool > 0 ? t("tools.tune.cooling", { s: Math.ceil(cool / 1000) }) :
		t("tools.tune.key_s", { s: +el.hold.value / 1000 });
	q(".tune-suggest").disabled = S.busy || !watts || !shown.target || g === "good";
	q(".tune-save").disabled = S.busy || !S.info.store || !isDirty;
	q(".tune-reset").disabled = S.busy || !S.info.store || (!p && !savedLabel(S.band, level) && w.delta === 0 && !S.names.has(key(S.band, level)));

	const h = q(".tune-hint");
	if (hint !== undefined) h.textContent = hint;
	else if (!transmits) h.textContent = t("tools.tune.hint.off");
	else if (!w.lastDrive) h.textContent = t("tools.tune.hint.start");
	else if (!watts) h.textContent = t("tools.tune.hint.type");
	else if (g === "good") h.textContent = isDirty ? t("tools.tune.hint.good_save") : t("tools.tune.hint.good");
	else if (!shown.target) h.textContent = t("tools.tune.hint.no_target");
	else if (!h.textContent || h.dataset.auto) h.textContent = t("tools.tune.hint.adjust");
	h.dataset.auto = hint === undefined ? "1" : "";
}

function renderKeying() {
	if (S.keying) renderLevel(S.keying.level);
}

function renderSaved() {
	const rec = S.record || { points: [], labels: [] };
	const n = rec.points.length + rec.labels.length;
	const stale = [...rec.points, ...rec.labels].filter((e) => e.state !== "applied").length;
	el.savedSum.textContent = n ? t("tools.tune.saved.sum", { n, stale }) : t("tools.tune.saved.none");
	if (stale) el.saved.open = true;
	const rows = [];
	const row = (cells, cls, del) => {
		const tr = document.createElement("tr");
		if (cls) tr.className = cls;
		for (const c of cells) {
			const td = document.createElement("td");
			td.textContent = c;
			tr.append(td);
		}
		const td = document.createElement("td");
		if (del) {
			const b = document.createElement("button");
			b.type = "button";
			b.className = "btn btn-quiet";
			b.textContent = t("tools.tune.saved.delete");
			b.disabled = S.busy;
			b.addEventListener("click", del);
			td.append(b);
		}
		tr.append(td);
		rows.push(tr);
	};
	const bandName = (khz) => {
		const b = bandOf(khz);
		return b < 0 ? "—" : bandInfo(S.info.bands[b].lo, S.info.bands[b].hi).name;
	};
	const levelName = (khz, level) => {
		const b = bandOf(khz);
		return b < 0 ? t("tools.tune.level_n", { n: level + 1 }) : shownName(b, level).name;
	};
	for (const p of [...rec.points].sort((a, b) => a.khz - b.khz || a.level - b.level)) {
		row([bandName(p.khz), mhzText(p.khz), levelName(p.khz, p.level), signed(p.delta),
			p.mw ? t("tools.tune.w", { w: fmtW(p.mw / 1000) }) : "—", t(`tools.tune.entry.${p.state}`)],
		p.state !== "applied" ? "is-stale" : "",
		() => run("delete", async () => { await S.radio.delPoint(p.khz, p.level); S.work.delete(key(p.khz, p.level)); await refresh(); }));
	}
	for (const l of rec.labels) {
		const b = bandOf(l.khz);
		row([bandName(l.khz), "—", b < 0 ? l.name : `${factoryName(b, l.level)} → ${l.name}`, t("tools.tune.saved.name"),
			l.mw ? t("tools.tune.w", { w: fmtW(l.mw / 1000) }) : "—", t(`tools.tune.entry.${l.state}`)],
		l.state !== "applied" ? "is-stale" : "",
		() => run("delete", async () => { await S.radio.putLabel(l.khz, l.level, "", 0); await refresh(); }));
	}
	el.savedBody.replaceChildren(...rows);
	el.exportBtn.disabled = S.busy || !n;
	el.importInput.disabled = S.busy || !S.info.store;
	el.clear.disabled = S.busy || !n || !S.info.store;
}

// ---- wiring

el.connect.addEventListener("click", () => (S.info ? run("disconnect", () => disconnect(true)) : connect()));
el.confirm.addEventListener("change", () => el.confirm.closest(".card").classList.remove("tune-attention"));
el.add.addEventListener("submit", addFreq);
el.addCancel.addEventListener("click", () => { el.add.hidden = true; });
el.advanced.addEventListener("change", render);
el.hold.addEventListener("change", render);
el.exportBtn.addEventListener("click", exportFile);
el.importInput.addEventListener("change", () => {
	const f = el.importInput.files[0];
	el.importInput.value = "";
	if (f) importFile(f);
});
el.clear.addEventListener("click", clearAll);
el.bands.addEventListener("keydown", (e) => {
	const n = S.info?.bands.length || 0;
	if (!n || (e.key !== "ArrowRight" && e.key !== "ArrowLeft")) return;
	e.preventDefault();
	selectBand((S.band + (e.key === "ArrowRight" ? 1 : n - 1)) % n);
});
// The strip is drawn in pixels: redraw at a new width
let stripW = 0;
new ResizeObserver(() => {
	const w = Math.round(el.strip.clientWidth);
	if (w && w !== stripW && S.info?.bands.length) { stripW = w; renderFreqs(); }
}).observe(el.strip);
window.addEventListener("pagehide", () => { if (S.radio) S.radio.ptt(false).catch(() => {}); });
if (!serialSupported()) status(t("flash.err.no_serial"), "bad");
render();
