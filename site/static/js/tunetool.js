// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Radio tuning (docs/spec/radio-tune.md, docs/user/16-battery-and-power.md): the radio's transmit bands as tabs,
// up to three tuning frequencies per band, a card per power level, the oscillator, each mode's carrier and deviation,
// and the battery reading with its empty and full voltages. Every change runs on the radio at once (RADIO TUNE_SET,
// TX_DRIVE, SETTING 35); Save to radio stores the record.
// Everything comes from the radio (bands, levels, names, drives, what the board trims), so a new radio, band, level or
// mode needs no change here. The connection is watched all the time: a radio whose own transmission upsets the USB
// link is found again by itself, and the unsaved changes go back on the radio.

import { Link, LinkError, CableError, SerialStream, RFOUNDRY_USB, CABLES, browserErrorKey, knownRadioPort, noDeviceChosen,
	probe, serialApi, serialSupported } from "./flasher/link.js";
import { page } from "./i18n.js";
import { TuneRadio, KIND, MODE, ST, BATT, BATT_MV_MAX, BATT_SPAN_MV, DEV_MAX, FREQ_MAX, GAIN_MAX, MAX_ENTRIES, NAME_LEN,
	REF_TRIM_MAX, VALUE_MAX, battRange, cleanName, encodeRecord, gridKhz, owner, toWords, valueAt } from "./tune-radio.js";
import { bandInfo, db, grade, mhzText, neighborSlope, parseMhz, parseReading, parseWatts, suggestDrive,
	trimHz } from "./tune-math.js";

const { t, num, has } = page;
const $ = (id) => document.getElementById(id);
const FILTERS = [RFOUNDRY_USB, ...CABLES];
const FREQS_PER_BAND = 3;
const WATCH_MS = 1500;		// how often a lost radio is looked for
const BEAT_MS = 2000;		// how often an idle connection is checked
const LIVE_MS = 150;		// a change goes to the radio this long after the last one
const REKEY_MS = 700;		// a transmission restarts this long after the last change it must show
const REF_MS = 400;		// the oscillator is stored this long after the last change
const BATT_MS = 1500;		// the battery is read again this long after a change (the radio reads each second)
const EXPORT_FORMAT = "rfoundry-radio-tune";
const RADIO_NAMES = (() => {
	try { return JSON.parse($("tune-radios")?.textContent || "{}"); } catch (e) { return {}; }
})();

const el = Object.fromEntries(["bar", "conn", "conn-text", "radio-name", "dirty", "discard", "save", "connect-btn",
	"status", "lost", "lost-text", "retry", "give-up", "confirm", "work", "bands", "band-range", "strip", "chips", "add",
	"add-mhz", "add-err", "add-cancel", "levels", "levels-title", "hold", "advanced", "freq", "freq-rows", "dev",
	"dev-rows", "saved", "saved-sum", "export", "import", "clear", "level-tpl", "row-tpl", "dialog", "dialog-title",
	"dialog-text", "dialog-ok", "cap", "batt", "batt-volts", "batt-raw"].map((id) => [id.replace(/-(.)/g, (_, c) => c.toUpperCase()), $(`tune-${id}`)]));
el.savedBody = document.querySelector("#tune-saved-table tbody");

const S = {
	conn: "idle",		// idle, connecting, on, lost
	stream: null, radio: null, probe: null, id: null, own: 0, back: null,
	core: null,		// the core view's facts (tune-radio.js parseCore)
	area: null,		// the tune area, null where nothing can be stored
	saved: [],		// the record in force on the radio
	work: [],		// what the radio runs now: saved plus unsaved changes
	factory: new Map(),	// kHz -> each level's factory drive there
	names: new Map(),	// board band index -> each level's factory name
	ref: null,		// the oscillator trim, ppb (null: the radio has none)
	batt: null,		// the battery's last reading (TuneRadio.battery), null before one or without a battery
	band: 0, active: new Map(), added: new Map(),
	meter: new Map(),	// "khz:level" -> {readings: [{drive, watts}], lastDrive, reading}
	targets: new Map(),	// "band:level" -> watts typed
	tx: null,		// the test transmission: {kind, level, mode, khz, hold, on, end}
	busy: "",		// connect, save, restore: the page waits
	looking: false,
	saving: false,
	q: Promise.resolve(), pending: 0,
	timers: {},
};

// ---- helpers

const key = (a, b) => `${a}:${b}`;
const fmtW = (w) => (w >= 1 ? num(+w.toPrecision(3)) : num(+w.toPrecision(2)));
const signed = (n, digits = 0) => `${n > 0 ? "+" : n < 0 ? "−" : ""}${num(+Math.abs(n).toFixed(digits))}`;
const txBands = () => (S.core?.bands || []).filter((b) => b.tx);
const curBand = () => txBands()[S.band];
const bandOf = (khz) => txBands().findIndex((b) => khz >= b.lo && khz <= b.hi);
const hasMode = (m) => String(S.probe?.features || "").split(",").includes(m === MODE.DMR ? "DMR" : "FM");
const modeName = (m) => t(`tools.tune.mode.${m === MODE.DMR ? "dmr" : "fm"}`);
const clone = (es) => es.map((e) => ({ ...e }));
const wordsUsed = (es = S.work) => es.filter((e) => e.kind !== KIND.NAME).length;
const namesUsed = (es = S.work) => es.filter((e) => e.kind === KIND.NAME).length;
const confirmed = () => el.confirm.checked;

function canon(es) {
	return JSON.stringify(es.map((e) => (e.kind === KIND.NAME ? [3, e.sel, e.band, e.name] :
		[e.kind, e.sel, e.khz, e.value, e.kind === KIND.DRIVE ? e.factory : 0])).sort());
}
const dirty = () => canon(S.work) !== canon(S.saved);

function status(text, kind = "") {
	el.status.textContent = text;
	el.status.dataset.kind = kind;
}

function setTimer(name, ms, f) {
	clearTimeout(S.timers[name]);
	S.timers[name] = setTimeout(f, ms);
}
function clearTimers() {
	for (const k of Object.keys(S.timers)) clearTimeout(S.timers[k]);
	S.timers = {};
}

function errorText(e) {
	if (e?.tuneStep === "tx") {
		const k = `tools.tune.deny.${e.deny}`;
		const why = !e.deny || e.deny === "none" ? t("tools.tune.deny.unknown") : has(k) ? t(k) : t("tools.tune.deny.other", { code: e.deny });
		return t("tools.tune.err.no_tx", { why });
	}
	if (e?.tuneStep === "verify") return t("tools.tune.err.verify");
	if (e?.tuneStep) return t("tools.tune.err.step", { step: t(`tools.tune.step.${e.tuneStep}`) });
	switch (e?.status) {
	case ST.REFUSED: return t("tools.tune.err.refused");
	case ST.BAD_ARG: return t("tools.tune.err.bad_arg");
	case ST.UNAVAILABLE: return t("tools.tune.err.unavailable");
	case ST.BUSY: return t("tools.tune.err.flash");
	default: break;
	}
	const browser = browserErrorKey(e);
	return browser ? t(browser) : e?.message || String(e);
}

// The radio stopped answering, or its port went away: not a refusal (a status), not a page bug
const isLoss = (e) => e?.status === undefined && (e instanceof LinkError || e instanceof CableError || S.stream?.lost ||
	["NetworkError", "NotFoundError", "InvalidStateError", "NotReadableError"].includes(e?.name));

function ask(title, text, ok) {
	el.dialogTitle.textContent = title;
	el.dialogText.textContent = text;
	el.dialogOk.textContent = ok;
	el.dialog.returnValue = "";
	el.dialog.showModal();
	return new Promise((resolve) => el.dialog.addEventListener("close", () => resolve(el.dialog.returnValue === "ok"), { once: true }));
}

// One radio operation at a time, in order. A lost link moves the page to "lost"; any other failure is shown
function op(f) {
	S.pending++;
	const p = S.q.then(() => (S.conn === "on" ? f() : undefined));
	S.q = p.catch(() => {}).finally(() => { S.pending--; });
	return p.catch((e) => {
		if (isLoss(e)) lost();
		else status(errorText(e), "bad");
		throw e;
	});
}
const quietly = (p) => p.catch(() => {});

// ---- the connection

async function connect() {
	if (!serialSupported()) {
		status(t("flash.err.no_serial"), "bad");
		return;
	}
	let port;
	try {
		port = await serialApi().requestPort({ filters: FILTERS });
	} catch (e) {
		if (!noDeviceChosen(e)) status(errorText(e), "bad");
		return;
	}
	S.conn = "connecting";
	S.busy = "connect";
	status(t("tools.tune.status.connecting"));
	render();
	try {
		const stream = await SerialStream.open(port);
		S.stream = stream;
		const link = new Link(stream);
		const p = await probe(link);
		if (!p) throw new Error(t("tools.tune.err.not_radio"));
		await start(stream, p);
		status(S.area ? t("tools.tune.status.connected") : t("tools.tune.err.no_store"), S.area ? "ok" : "bad");
	} catch (e) {
		status(errorText(e), "bad");
		await disconnect(false);
	} finally {
		S.busy = "";
		render();
	}
}

// A radio that answered: what it is, what it can tune, what it has stored; the VFO on the first tuning frequency
async function start(stream, p) {
	S.stream = stream;
	S.probe = p;
	S.id = { board: p.board, uid: p.chip?.uid || "" };
	S.own = owner(p.board, S.id.uid);
	S.radio = new TuneRadio(new Link(stream));
	S.core = await S.radio.core();
	if (!S.core.built) throw new Error(t("tools.tune.err.no_feature"));
	if (!txBands().length || !S.core.levels) throw new Error(t("tools.tune.err.no_bands"));
	S.area = await S.radio.area();
	S.saved = [];
	if (S.area) {
		const c = await S.radio.copies(S.area, S.own);
		S.saved = c.record?.entries || [];
		await S.radio.load();	// live words left from an earlier visit give way to what is stored
	}
	S.work = clone(S.saved);
	S.ref = await S.radio.refTrim().catch(() => null);
	S.conn = "on";
	S.band = 0;
	// The factory drives where drive trims are stored, so the page can tell the ones out of date
	for (const khz of new Set(S.saved.filter((e) => e.kind === KIND.DRIVE && bandOf(e.khz) >= 0).map((e) => e.khz))) await visit(khz);
	await visit(activeKhz());
	if (S.core.battery) S.batt = await S.radio.battery();
	beat();
}

async function disconnect(restore = true) {
	const radio = S.radio, stream = S.stream;
	clearTimers();
	S.conn = "idle";
	S.radio = S.stream = S.core = S.probe = S.id = S.back = S.batt = null;
	S.tx = null;
	for (const m of [S.factory, S.names, S.active, S.added, S.meter, S.targets]) m.clear();
	S.saved = S.work = [];
	if (radio && restore) {
		S.busy = "restore";
		render();
		await S.q;
		await radio.restore();
		S.busy = "";
	}
	if (stream) await quietly(stream.close());
	render();
}

// Lost: the radio stopped answering or its port went away (often its own transmission upsetting the USB link). The
// page keeps what was changed and looks for the radio until the same one answers again
function lost() {
	if (S.conn !== "on") return;
	clearTimers();
	S.conn = "lost";
	S.back = S.radio?.saved || S.back;	// the radio's state before the page, for the restore at the end
	const stream = S.stream;
	S.stream = S.radio = null;
	S.tx = null;
	S.q = Promise.resolve();
	status(t("tools.tune.lost.kept"), "bad");
	render();
	if (stream) quietly(stream.close());
	setTimer("watch", WATCH_MS, lookAgain);
}

async function lookAgain() {
	if (S.conn !== "lost" || S.looking) return;
	S.looking = true;
	let found = null;
	try {
		found = await knownRadioPort(FILTERS, (p) => p.board === S.id.board && (p.chip?.uid || "") === S.id.uid);
	} catch (e) { /* a cable that takes no data yet */ }
	S.looking = false;
	if (S.conn !== "lost") {
		if (found) quietly(found.stream.close());
		return;
	}
	if (!found) {
		setTimer("watch", WATCH_MS, lookAgain);
		return;
	}
	await resume(found);
}

// The same radio again: the page's changes go back on the radio (it may have restarted), and work goes on
async function resume({ stream, probe: p }) {
	S.stream = stream;
	S.probe = p;
	S.radio = new TuneRadio(new Link(stream));
	S.radio.saved = S.back;
	S.conn = "on";
	const wasSaving = S.saving;
	S.saving = false;
	render();
	try {
		await op(async () => {
			S.core = await S.radio.core();
			if (S.area) {
				const c = await S.radio.copies(S.area, S.own);
				S.saved = c.record?.entries || [];
			}
			await S.radio.setWords(liveWords());
			await visit(activeKhz());
		});
		status(wasSaving ? t("tools.tune.status.back_saving") : t("tools.tune.status.back"), wasSaving ? "bad" : "ok");
		beat();
	} catch (e) { /* lost again: still looking */ }
	render();
}

// An idle link is asked for its version now and then, so a lost radio shows at once and not at the next click
function beat() {
	setTimer("beat", BEAT_MS, async () => {
		if (S.conn !== "on") return;
		if (!S.pending && !S.busy) {
			try {
				await op(async () => {
					if (S.tx?.on) {
						const c = await S.radio.core();
						if (c.state !== "tx" && S.tx?.on) txEnded(c);
					} else if (S.core?.battery) await readBattery();
					else await S.radio.link.version();
				});
			} catch (e) { /* lost() or shown */ }
		}
		if (S.conn === "on") beat();
	});
}

// ---- the radio's data

const factoryAt = (khz, level) => S.factory.get(khz)?.[level];
const liveWords = () => toWords(S.work, factoryAt);
const levelNames = (b) => S.names.get(txBands()[b]?.index) || [];
const factoryName = (b, level) => levelNames(b)[level] || t("tools.tune.level_n", { n: level + 1 });

function freqs(band) {
	const b = txBands()[band];
	const set = new Set(S.work.filter((e) => e.kind !== KIND.NAME && e.khz >= b.lo && e.khz <= b.hi).map((e) => e.khz));
	for (const f of S.added.get(band) || []) set.add(f);
	if (!set.size) set.add(gridKhz(bandInfo(b.lo, b.hi).khz));
	return [...set].sort((x, y) => x - y);
}

function activeKhz() {
	const fs = freqs(S.band);
	const a = S.active.get(S.band);
	return fs.includes(a) ? a : fs[0];
}

// The VFO on khz (FM, no tone): each level's factory drive and name there
async function visit(khz) {
	const c = await S.radio.setup(khz);
	S.core = { ...S.core, ...c, bands: S.core.bands };
	S.factory.set(khz, c.drives);
	const b = bandOf(khz);
	if (b >= 0 && c.levelNames.length) S.names.set(txBands()[b].index, c.levelNames);
}

// Each level's limits at khz: the radio keeps the top level at or under its factory drive and the lowest at or over
function limits(khz, level) {
	const d = S.factory.get(khz) || [];
	const f = d[level] || 0, lo = d[0] || 0, hi = d[d.length - 1] || 0;
	return { factory: f, lo: Math.max(lo - f, -VALUE_MAX), hi: Math.min(hi - f, VALUE_MAX) };
}

const entry = (kind, sel, khz) => S.work.find((e) => e.kind === kind && e.sel === sel && e.khz === khz);
const savedEntry = (kind, sel, khz) => S.saved.find((e) => e.kind === kind && e.sel === sel && e.khz === khz);
const nameEntry = (band, level, es = S.work) => es.find((e) => e.kind === KIND.NAME && e.band === txBands()[band]?.index && e.sel === level);
const stale = (e) => e.kind === KIND.DRIVE && factoryAt(e.khz, e.sel) !== undefined && factoryAt(e.khz, e.sel) !== e.factory;

// The value the radio uses at khz: this frequency's trim, else the band's blend
function valueHere(kind, sel, khz) {
	const b = txBands()[bandOf(khz)];
	const es = S.work.filter((e) => !stale(e));
	return b ? valueAt(es, kind, sel, khz, b.lo, b.hi) : 0;
}

function shownName(band, level) {
	const n = nameEntry(band, level);
	const name = n?.name || factoryName(band, level);
	const typed = S.targets.get(key(band, level));
	return { name, target: typed ?? parseWatts(name) };
}

function meter(khz, level) {
	const k = key(khz, level);
	if (!S.meter.has(k)) S.meter.set(k, { readings: [], lastDrive: 0, reading: "" });
	return S.meter.get(k);
}

const driveNow = (khz, level) => {
	const l = limits(khz, level);
	return l.factory + Math.max(l.lo, Math.min(l.hi, valueHere(KIND.DRIVE, level, khz)));
};

// ---- changes: in the working set, on the radio at once, in a transmission running now

function setTrim(kind, sel, khz, value) {
	let e = entry(kind, sel, khz);
	if (!e) {
		if (wordsUsed() >= S.core.maxWords || S.work.length >= MAX_ENTRIES) {
			status(t("tools.tune.err.full", { max: S.core.maxWords }), "bad");
			return false;
		}
		e = { kind, sel, khz, value: 0, factory: 0 };
		S.work.push(e);
	}
	e.value = value;
	if (kind === KIND.DRIVE) e.factory = limits(khz, sel).factory;
	changed(kind, sel);
	return true;
}

function clearTrim(kind, sel, khz) {
	S.work = S.work.filter((e) => !(e.kind === kind && e.sel === sel && e.khz === khz));
	changed(kind, sel);
}

function setName(band, level, name) {
	const b = txBands()[band].index;
	S.work = S.work.filter((e) => !(e.kind === KIND.NAME && e.band === b && e.sel === level));
	const n = cleanName(name);
	if (n && n !== factoryName(band, level)) {
		if (namesUsed() >= S.core.maxNames || S.work.length >= MAX_ENTRIES) {
			status(t("tools.tune.err.names_full", { max: S.core.maxNames }), "bad");
		} else S.work.push({ kind: KIND.NAME, sel: level, band: b, name: n });
	}
	render();
}

function changed(kind, sel) {
	setTimer("live", LIVE_MS, () => quietly(op(() => S.radio.setWords(liveWords()))));
	const tx = S.tx;
	if (tx?.on) {
		if (kind === KIND.DRIVE && tx.kind === "drive" && tx.level === sel && S.core.liveDrive) {
			setTimer("drive", LIVE_MS, () => quietly(op(() => S.radio.txDrive(driveNow(tx.khz, sel)))));
		} else if ((kind === KIND.DRIVE && tx.kind === "drive" && tx.level === sel) ||
			   ((kind === KIND.FREQ || kind === KIND.DEV) && tx.mode === sel) || kind === "osc") {
			setTimer("rekey", REKEY_MS, rekey);
		}
	}
	render();
}

// The oscillator: in effect and stored at once (the Freq trim setting)
function setRef(ppb) {
	S.ref = Math.max(-REF_TRIM_MAX, Math.min(REF_TRIM_MAX, Math.round(ppb / 100) * 100));
	setTimer("ref", REF_MS, () => quietly(op(async () => { S.ref = await S.radio.setRefTrim(S.ref); render(); })));
	changed("osc", 0);
}

// ---- the battery: no transmission, the radio takes a change at its next reading

const battEntry = (sel, es = S.work) => es.find((e) => e.kind === KIND.BATT && e.sel === sel);
const battValue = (sel) => battEntry(sel)?.value || 0;
const volts = (mv) => num(+(mv / 1000).toFixed(2));
const parseVolts = (s) => {
	const v = +String(s ?? "").trim().replace(",", ".");
	return Number.isFinite(v) && v > 0 ? v : null;
};
const battNow = () => battRange(S.core.battery.empty, S.core.battery.full, battValue(BATT.EMPTY), battValue(BATT.FULL));
const BATT_HINT = ["tools.tune.batt.reading_hint", "tools.tune.batt.empty_hint", "tools.tune.batt.full_hint"];
const BATT_TITLE = ["tools.tune.batt.reading", "tools.tune.batt.empty", "tools.tune.batt.full"];

async function readBattery() {
	S.batt = await S.radio.battery();
	renderBattery();
}

function setBatt(sel, value) {
	if (value) setTrim(KIND.BATT, sel, 0, value);
	else clearTrim(KIND.BATT, sel, 0);
	setTimer("batt", BATT_MS, () => quietly(op(readBattery)));
}

function battHint(sel, text, kind = "") {
	const h = el.batt.querySelector(`[data-batt="${sel}"] .tune-hint`);
	h.textContent = text || t(BATT_HINT[sel]);
	h.dataset.kind = kind;
}

// The voltage a meter shows across the battery: the gain that brings the radio's reading there
function correctReading(v) {
	const raw = S.batt?.raw;
	if (!raw) return battHint(BATT.GAIN, t("tools.tune.batt.err_none"), "bad");
	const gain = Math.round((v * 1000 / raw - 1) * 1000);
	if (Math.abs(gain) > GAIN_MAX) return battHint(BATT.GAIN, t("tools.tune.batt.err_gain", { pct: GAIN_MAX / 10 }), "bad");
	setBatt(BATT.GAIN, gain);
	battHint(BATT.GAIN, t("tools.tune.batt.applied"), "ok");
}

// Where empty (0 %) or full (100 %) sits: an offset from the board's own voltage, kept BATT_SPAN_MV apart
function setLimit(sel, v) {
	const b = S.core.battery, base = sel === BATT.EMPTY ? b.empty : b.full;
	const mv = Math.round(v * 1000), off = mv - base, now = battNow();
	if (Math.abs(off) > BATT_MV_MAX) {
		return battHint(sel, t("tools.tune.batt.err_range", { lo: volts(base - BATT_MV_MAX), hi: volts(base + BATT_MV_MAX) }), "bad");
	}
	if (sel === BATT.EMPTY ? now.full - mv < BATT_SPAN_MV : mv - now.empty < BATT_SPAN_MV) {
		return battHint(sel, t("tools.tune.batt.err_span", { v: volts(BATT_SPAN_MV) }), "bad");
	}
	setBatt(sel, off);
	battHint(sel, t("tools.tune.batt.applied"), "ok");
}

function battSubmit(sel, v) {
	if (v === null) return battHint(sel, t("tools.tune.batt.err_number"), "bad");
	return sel === BATT.GAIN ? correctReading(v) : setLimit(sel, v);
}

// ---- the test transmission

const sameTx = (a, b) => a && b && a.kind === b.kind && (a.level ?? -1) === (b.level ?? -1) && (a.mode ?? 0) === (b.mode ?? 0);

async function transmit(what) {
	if (S.tx) {
		const same = sameTx(S.tx, what);
		await stopTx();
		if (same) return;
	}
	if (!confirmed()) {
		status(t("tools.tune.status.confirm_first"), "bad");
		el.confirm.closest(".card").classList.add("tune-attention");
		el.confirm.focus();
		return;
	}
	const khz = activeKhz();
	const tx = { ...what, khz, mode: what.mode ?? MODE.FM, level: what.level ?? 0, hold: +el.hold.value, on: false, end: 0 };
	S.tx = tx;
	status(t("tools.tune.status.keying", { what: txLabel(tx), mhz: mhzText(khz) }), "tx");
	render();
	try {
		await op(async () => {
			clearTimeout(S.timers.live);
			await S.radio.setWords(liveWords());
			const c = await S.radio.setup(khz, tx.mode);
			if (tx.mode === MODE.FM) S.factory.set(khz, c.drives);
			await S.radio.setLevel(tx.level);
			await keyOn(tx);
		});
	} catch (e) {
		if (S.tx === tx) S.tx = null;
		if (S.conn === "on") quietly(op(() => S.radio.txLimit(0)));
		render();
	}
}

function txLabel(tx) {
	if (tx.kind === "drive") return shownName(S.band, tx.level).name;
	if (tx.kind === "osc") return t("tools.tune.osc.title");
	return t(tx.kind === "dev" ? "tools.tune.dev.row" : "tools.tune.freq.row", { mode: modeName(tx.mode) });
}

async function keyOn(tx) {
	if (S.tx !== tx) return;
	await S.radio.keyUp(tx.hold + 1000);
	tx.end = Date.now() + tx.hold;
	await new Promise((r) => setTimeout(r, 350));
	const c = await S.radio.core();
	if (c.state !== "tx") {
		await quietly(S.radio.keyDown());
		throw Object.assign(new Error("tx"), { tuneStep: "tx", deny: c.deny });
	}
	tx.on = true;
	if (tx.kind === "drive") {
		const m = meter(tx.khz, tx.level);
		m.lastDrive = driveNow(tx.khz, tx.level);
		m.reading = "";
	}
	tick();
}

function tick() {
	const tx = S.tx;
	if (!tx?.on) return;
	if (Date.now() >= tx.end) {
		stopTx(true);
		return;
	}
	renderTx();
	setTimer("tick", 250, tick);
}

// A change the radio takes only at key-up: the transmission ends and starts again with it
function rekey() {
	const tx = S.tx;
	if (!tx?.on) return;
	tx.on = false;
	quietly(op(async () => {
		await S.radio.setWords(liveWords());
		await S.radio.keyDown();
		await S.radio.waitCore((c) => c.state !== "tx" && c.state !== "tx_tail", "end", 3000);
		await keyOn(tx);
	}).catch(() => { if (S.tx === tx) S.tx = null; render(); }));
}

function stopTx(timeUp = false) {
	const tx = S.tx;
	S.tx = null;
	clearTimeout(S.timers.tick);
	clearTimeout(S.timers.rekey);
	render();
	if (!tx) return Promise.resolve();
	if (timeUp && tx.kind === "drive") {
		status(t("tools.tune.status.read_meter"), "ok");
		el.levels.querySelector(`[data-level="${tx.level}"] .tune-reading`)?.focus();
	} else status(t("tools.tune.status.stopped"), "");
	return quietly(op(async () => {
		await S.radio.keyDown();
		await quietly(S.radio.txDrive(0));
		await S.radio.txLimit(0);
	}));
}

function txEnded(c) {
	const tx = S.tx;
	S.tx = null;
	clearTimeout(S.timers.tick);
	status(t("tools.tune.err.no_tx", { why: c.deny && c.deny !== "none" && has(`tools.tune.deny.${c.deny}`) ?
		t(`tools.tune.deny.${c.deny}`) : t("tools.tune.deny.ended") }), tx ? "bad" : "");
	quietly(op(() => S.radio.txLimit(0)));
	render();
}

// ---- power: readings and suggestions

function takeReading(level, text) {
	const m = meter(activeKhz(), level);
	m.reading = text;
	const watts = parseReading(text);
	if (watts && m.lastDrive) {
		m.readings = m.readings.filter((r) => r.drive !== m.lastDrive);
		m.readings.push({ drive: m.lastDrive, watts });
	}
	renderLevel(level);
}

function suggest(level) {
	const khz = activeKhz(), m = meter(khz, level), lim = limits(khz, level);
	const { target } = shownName(S.band, level);
	const drives = S.factory.get(khz) || [];
	const slope = neighborSlope(drives.map((d, i) => ({ factory: d, watts: parseWatts(factoryName(S.band, i)) })), level);
	const d = suggestDrive(m.readings, target, { lo: lim.factory + lim.lo, hi: lim.factory + lim.hi, slope });
	if (d === null) return;
	const before = valueHere(KIND.DRIVE, level, khz);
	const delta = d - lim.factory;
	if (delta === before) {
		renderLevel(level, t("tools.tune.hint.at_limit"));
		return;
	}
	setTrim(KIND.DRIVE, level, khz, delta);
	renderLevel(level, t("tools.tune.hint.suggested", { delta: signed(delta) }));
}

function setDrive(level, delta) {
	const khz = activeKhz(), lim = limits(khz, level);
	setTrim(KIND.DRIVE, level, khz, Math.max(lim.lo, Math.min(lim.hi, Math.round(delta) || 0)));
}

// ---- save, discard, restore

async function saveAll() {
	if (!S.area || S.busy) return;
	if (S.tx) await stopTx();
	const entries = clone(S.work);
	S.busy = "save";
	S.saving = true;
	status(t("tools.tune.status.saving"));
	render();
	try {
		await op(async () => {
			const c = await S.radio.copies(S.area, S.own);
			const copies = await S.radio.save(S.area, c.at, encodeRecord(c.next, S.own, entries));
			const after = await S.radio.copies(S.area, S.own);
			S.saved = after.record?.entries || [];
			if (canon(S.saved) !== canon(entries)) throw Object.assign(new Error("verify"), { tuneStep: "verify" });
			S.work = clone(S.saved);
			status(copies === 3 ? t("tools.tune.status.saved") : t("tools.tune.err.one_copy"), copies === 3 ? "ok" : "bad");
		});
	} catch (e) { /* shown, or lost: the record in force before still is */ }
	S.saving = false;
	S.busy = "";
	render();
}

async function discard() {
	if (S.tx) await stopTx();
	S.work = clone(S.saved);
	S.targets.clear();
	await quietly(op(async () => {
		if (S.area) await S.radio.load();
		else await S.radio.setWords([]);
		status(t("tools.tune.status.discarded"), "ok");
	}));
	render();
}

async function clearAll() {
	if (!await ask(t("tools.tune.clear.title"), t("tools.tune.clear.text"), t("tools.tune.clear.ok"))) return;
	S.work = [];
	S.targets.clear();
	if (S.ref) setRef(0);
	changed();
	await saveAll();
}

function exportFile() {
	const data = { format: EXPORT_FORMAT, version: 2, board: S.id.board, uid: S.id.uid, firmware: S.probe.git || "",
		ref_ppb: S.ref, entries: S.saved };
	const blob = new Blob([JSON.stringify(data, null, 2)], { type: "application/json" });
	const a = document.createElement("a");
	a.href = URL.createObjectURL(blob);
	a.download = `radio-tuning-${S.id.board}-${S.id.uid.slice(-6) || "radio"}.json`;
	a.click();
	setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// A backup comes back as unsaved changes: live on the radio at once, stored with Save to radio
async function importFile(file) {
	let data;
	try {
		data = JSON.parse(await file.text());
	} catch (e) {
		data = null;
	}
	if (data?.format !== EXPORT_FORMAT || !Array.isArray(data.entries)) {
		status(t("tools.tune.import.bad"), "bad");
		return;
	}
	// Tuning belongs to one radio: another radio's trims would be wrong here, so the file must be this radio's
	if (data.board !== S.id.board || !data.uid || data.uid !== S.id.uid) {
		status(t("tools.tune.import.other"), "bad");
		return;
	}
	if (!await ask(t("tools.tune.import.title"), t("tools.tune.import.text"), t("tools.tune.import.ok"))) return;
	const ok = (e) => Number.isInteger(e?.kind) && Number.isInteger(e.sel) && (e.kind === KIND.NAME ?
		typeof e.name === "string" && Number.isInteger(e.band) : Number.isInteger(e.khz) && Number.isInteger(e.value));
	const all = data.entries.filter(ok).slice(0, MAX_ENTRIES).map((e) => (e.kind === KIND.NAME ?
		{ kind: KIND.NAME, sel: e.sel, band: e.band, name: cleanName(e.name) } :
		{ kind: e.kind, sel: e.sel, khz: gridKhz(e.khz), value: e.value, factory: e.factory | 0 }));
	const kept = all.filter((e) => !stale(e));
	S.work = kept;
	if (Number.isInteger(data.ref_ppb) && S.ref !== null) setRef(data.ref_ppb);
	changed();
	status(t("tools.tune.import.done", { n: kept.length, skipped: data.entries.length - kept.length }),
		kept.length === data.entries.length ? "ok" : "bad");
}

// ---- moving around

async function selectFreq(khz) {
	if (S.tx) await stopTx();
	S.active.set(S.band, khz);
	render();
	await quietly(op(() => visit(khz)));
	render();
}

async function selectBand(b) {
	if (S.tx) await stopTx();
	S.band = b;
	render();
	await quietly(op(() => visit(activeKhz())));
	render();
	el.bands.querySelector(`[data-band="${b}"]`)?.focus();
}

function addFreq(ev) {
	ev.preventDefault();
	const band = curBand();
	const typed = parseMhz(el.addMhz.value);
	const khz = typed && gridKhz(typed);
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

async function removeFreq(khz) {
	const here = S.work.filter((e) => e.kind !== KIND.NAME && e.khz === khz);
	if (here.length && !await ask(t("tools.tune.remove.title", { mhz: mhzText(khz) }), t("tools.tune.remove.text"),
		t("tools.tune.remove.ok"))) return;
	S.work = S.work.filter((e) => !(e.kind !== KIND.NAME && e.khz === khz));
	S.added.set(S.band, (S.added.get(S.band) || []).filter((f) => f !== khz));
	S.active.delete(S.band);
	changed();
	await selectFreq(activeKhz());
}

// ---- drawing

function render() {
	const on = S.conn === "on", lostNow = S.conn === "lost", known = on || lostNow;
	el.bar.dataset.conn = S.conn;
	el.connText.textContent = t(`tools.tune.conn.${S.conn}`);
	el.radioName.textContent = S.id ? RADIO_NAMES[S.id.board] || S.id.board : "";
	el.connectBtn.textContent = known ? t("tools.tune.disconnect") : t("tools.tune.connect");
	el.connectBtn.classList.toggle("btn-primary", !known);
	el.connectBtn.disabled = S.conn === "connecting" || !!S.busy;
	const d = known && dirty();
	el.dirty.hidden = !d;
	el.save.hidden = el.discard.hidden = !known || (!d && S.busy !== "save");
	el.save.disabled = !on || !!S.busy || !S.area || !d;
	el.discard.disabled = !on || !!S.busy;
	el.save.textContent = S.busy === "save" ? t("tools.tune.saving") : t("tools.tune.save");
	el.lost.hidden = !lostNow;
	el.work.hidden = !known;
	el.work.inert = lostNow || !!S.busy;
	el.work.classList.toggle("is-waiting", lostNow || !!S.busy);
	if (!known || !S.core) return;
	renderBands();
	renderFreqs();
	renderLevels();
	renderTrims();
	renderBattery();
	renderSaved();
}

function renderBands() {
	el.bands.replaceChildren(...txBands().map((b, i) => {
		const info = bandInfo(b.lo, b.hi);
		const tab = document.createElement("button");
		tab.type = "button";
		tab.role = "tab";
		tab.className = "tune-band";
		tab.dataset.band = i;
		tab.ariaSelected = String(i === S.band);
		tab.tabIndex = i === S.band ? 0 : -1;
		const tuned = S.work.some((e) => e.kind !== KIND.NAME && e.khz >= b.lo && e.khz <= b.hi);
		tab.innerHTML = "<strong></strong><span></span>";
		tab.querySelector("strong").textContent = info.name;
		if (tuned) tab.querySelector("strong").insertAdjacentHTML("beforeend", '<i class="tune-dot" aria-hidden="true"></i>');
		tab.querySelector("span").textContent = t("tools.tune.band_range", { lo: mhzText(b.lo), hi: mhzText(b.hi) });
		tab.addEventListener("click", () => selectBand(i));
		return tab;
	}));
	el.bands.hidden = txBands().length < 2;
}

const SVG = "http://www.w3.org/2000/svg";
function svg(tag, attrs, text) {
	const e = document.createElementNS(SVG, tag);
	for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
	if (text != null) e.textContent = text;
	return e;
}

function renderFreqs() {
	const b = curBand(), info = bandInfo(b.lo, b.hi), fs = freqs(S.band), act = activeKhz();
	el.bandRange.textContent = t("tools.tune.band_range", { lo: mhzText(b.lo), hi: mhzText(b.hi) });
	// The band strip: the band's edges, the amateur band inside, the tuning frequencies; solid between them (the radio
	// blends), dashed past the outer ones (held). Drawn at the strip's own width, so the labels keep their shape
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
		pick.addEventListener("click", () => selectFreq(f));
		chip.append(pick);
		if (fs.length > 1 || S.work.some((e) => e.kind !== KIND.NAME && e.khz === f)) {
			const xb = document.createElement("button");
			xb.type = "button";
			xb.className = "tune-chip-x";
			xb.textContent = "×";
			xb.ariaLabel = t("tools.tune.freqs.remove", { mhz: mhzText(f) });
			xb.addEventListener("click", () => removeFreq(f));
			chip.append(xb);
		}
		return chip;
	});
	if (fs.length < FREQS_PER_BAND) {
		const add = document.createElement("button");
		add.type = "button";
		add.className = "btn btn-quiet tune-chip-add";
		add.textContent = t("tools.tune.freqs.add");
		add.addEventListener("click", () => {
			el.add.hidden = false;
			el.addErr.textContent = "";
			el.addMhz.value = "";
			el.addMhz.placeholder = mhzText(gridKhz(b.lo + (b.hi - b.lo) * (fs.length === 1 ? 0.85 : 0.15)));
			el.addMhz.focus();
		});
		chips.push(add);
	}
	el.chips.replaceChildren(...chips);
	el.cap.textContent = t("tools.tune.freqs.capacity", { n: wordsUsed(), max: S.core.maxWords, names: namesUsed(),
		names_max: S.core.maxNames });
}

function renderLevels() {
	const khz = activeKhz();
	el.levelsTitle.textContent = t("tools.tune.levels_at", { mhz: mhzText(khz) });
	const at = `${S.band}:${khz}:${S.core.levels}`;
	if (el.levels.dataset.at !== at) {
		el.levels.dataset.at = at;
		el.levels.replaceChildren(...Array.from({ length: S.core.levels }, (_, level) => buildLevel(level)));
	}
	for (let level = 0; level < S.core.levels; level++) renderLevel(level);
}

function stepper(root, get, set) {
	root.querySelector(".tune-slider").addEventListener("input", (e) => set(+e.target.value));
	// The number box shows the slider's value / scale (ppm, Hz, %): back to slider units
	root.querySelector(".tune-num").addEventListener("change", (e) => {
		const v = +String(e.target.value).replace(",", ".");
		if (Number.isFinite(v)) set(Math.round(v * (+root.dataset.scale || 1)));
	});
	root.querySelectorAll(".tune-step").forEach((b) => b.addEventListener("click", (e) => {
		set(get() + +b.dataset.step * (e.shiftKey ? 10 : 1));
	}));
}

function buildLevel(level) {
	const card = el.levelTpl.content.firstElementChild.cloneNode(true);
	card.dataset.level = level;
	const q = (c) => card.querySelector(c);
	q(".tune-name").maxLength = NAME_LEN;
	q(".tune-name").ariaLabel = t("tools.tune.name_label");
	q(".tune-name").addEventListener("change", (e) => setName(S.band, level, e.target.value));
	q(".tune-target").addEventListener("change", (e) => {
		const w = parseReading(e.target.value);
		if (w) S.targets.set(key(S.band, level), w);
		else S.targets.delete(key(S.band, level));
		renderLevel(level);
	});
	q(".tune-reading").addEventListener("input", (e) => takeReading(level, e.target.value));
	q(".tune-reading").addEventListener("keydown", (e) => { if (e.key === "Enter") suggest(level); });
	stepper(card, () => valueHere(KIND.DRIVE, level, activeKhz()), (v) => setDrive(level, v));
	q(".tune-key").addEventListener("click", () => transmit({ kind: "drive", level }));
	q(".tune-suggest").addEventListener("click", () => suggest(level));
	q(".tune-reset").addEventListener("click", () => {
		clearTrim(KIND.DRIVE, level, activeKhz());
		setName(S.band, level, "");
		S.targets.delete(key(S.band, level));
		S.meter.delete(key(activeKhz(), level));
	});
	return card;
}

function badge(node, kind) {
	const map = { factory: "badge-neutral", tuned: "badge-supported", changed: "badge-testing", stale: "badge-planned", off: "badge-planned" };
	node.className = `badge tune-badge ${map[kind]}`;
	node.textContent = t(`tools.tune.badge.${kind}`);
	node.title = kind === "stale" ? t("tools.tune.badge.stale_help") : kind === "changed" ? t("tools.tune.badge.changed_help") : "";
}

// The state of a trim at khz: factory (none), tuned (saved), changed (not saved), stale (saved against another drive)
function trimState(kind, sel, khz) {
	const w = entry(kind, sel, khz), s = savedEntry(kind, sel, khz);
	if (w && stale(w)) return "stale";
	if ((w?.value ?? null) !== (s?.value ?? null)) return "changed";
	return w ? "tuned" : "factory";
}

function setRange(root, lo, hi, value, disabled, scale = 1, digits = 0) {
	const slider = root.querySelector(".tune-slider"), n = root.querySelector(".tune-num");
	root.dataset.scale = scale;
	slider.min = lo;
	slider.max = hi;
	slider.value = value;
	n.min = lo / scale;
	n.max = hi / scale;
	n.step = 1 / scale;
	if (document.activeElement !== n) n.value = (value / scale).toFixed(digits);
	slider.disabled = n.disabled = disabled || lo === hi;
	root.querySelectorAll(".tune-step").forEach((b) => {
		b.disabled = slider.disabled || (+b.dataset.step < 0 ? value <= lo : value >= hi);
	});
	slider.style.setProperty("--zero", `${hi > lo ? ((0 - lo) / (hi - lo)) * 100 : 50}%`);
}

function keyButton(btn, what, ok = true) {
	const mine = sameTx(S.tx, what);
	btn.disabled = !ok || (!!S.tx && !mine && !S.tx.on);
	btn.classList.toggle("is-on", mine);
	btn.textContent = mine ? (S.tx.on ? t("tools.tune.stop_s", { s: Math.max(0, Math.ceil((S.tx.end - Date.now()) / 1000)) }) :
		t("tools.tune.starting")) : t("tools.tune.key");
	btn.ariaPressed = String(mine);
}

function renderLevel(level, hint) {
	const card = el.levels.querySelector(`[data-level="${level}"]`);
	if (!card) return;
	const q = (c) => card.querySelector(c);
	const khz = activeKhz(), lim = limits(khz, level), m = meter(khz, level);
	const shown = shownName(S.band, level), fac = factoryName(S.band, level);
	const transmits = lim.factory > 0;
	const delta = valueHere(KIND.DRIVE, level, khz);
	const mine = S.tx?.kind === "drive" && S.tx.level === level;
	card.classList.toggle("is-off", !transmits);
	card.classList.toggle("is-keying", mine);

	const name = q(".tune-name");
	if (document.activeElement !== name) name.value = shown.name;
	q(".tune-factory").textContent = shown.name !== fac ? t("tools.tune.factory_name", { name: fac }) : "";
	const target = q(".tune-target");
	if (document.activeElement !== target) target.value = shown.target ? fmtW(shown.target) : "";
	const reading = q(".tune-reading");
	if (document.activeElement !== reading) reading.value = m.reading;
	reading.disabled = !m.lastDrive;
	reading.placeholder = m.lastDrive ? "" : t("tools.tune.reading_after");

	const nameChanged = canon([nameEntry(S.band, level)].filter(Boolean)) !== canon([nameEntry(S.band, level, S.saved)].filter(Boolean));
	const st = !transmits ? "off" : nameChanged ? "changed" : trimState(KIND.DRIVE, level, khz);
	badge(q(".tune-badge"), st);

	const watts = parseReading(m.reading);
	const g = grade(watts, shown.target);
	const out = q(".tune-error");
	out.className = `tune-error${g ? ` is-${g}` : ""}`;
	out.textContent = g ? t(`tools.tune.grade.${g}`, { db: signed(db(watts, shown.target), 1) }) : "";

	setRange(card, lim.lo, lim.hi, Math.max(lim.lo, Math.min(lim.hi, delta)), !transmits);
	const drive = lim.factory + Math.max(lim.lo, Math.min(lim.hi, delta));
	q(".tune-drive").textContent = el.advanced.checked ? t("tools.tune.drive", { drive, factory: lim.factory,
		lo: lim.factory + lim.lo, hi: lim.factory + lim.hi }) :
		delta >= lim.hi && lim.hi === 0 && transmits ? t("tools.tune.top_limit") :
		delta <= lim.lo && lim.lo === 0 && transmits ? t("tools.tune.bottom_limit") : "";

	keyButton(q(".tune-key"), { kind: "drive", level }, transmits);
	q(".tune-suggest").disabled = !watts || !shown.target || g === "good";
	q(".tune-reset").disabled = st === "factory" && !S.targets.has(key(S.band, level));

	const h = q(".tune-hint");
	if (hint !== undefined) h.textContent = hint;
	else if (!transmits) h.textContent = t("tools.tune.hint.off");
	else if (mine && S.tx.on) h.textContent = S.core.liveDrive ? t("tools.tune.hint.live") : t("tools.tune.hint.rekey");
	else if (!m.lastDrive) h.textContent = t("tools.tune.hint.start");
	else if (!watts) h.textContent = t("tools.tune.hint.type");
	else if (g === "good") h.textContent = dirty() ? t("tools.tune.hint.good_save") : t("tools.tune.hint.good");
	else if (!shown.target) h.textContent = t("tools.tune.hint.no_target");
	else if (!h.textContent || h.dataset.auto) h.textContent = t("tools.tune.hint.adjust");
	h.dataset.auto = hint === undefined ? "1" : "";
}

// The frequency and deviation rows: one per trim, built once per radio
function rows() {
	const out = [];
	if (S.ref !== null) out.push({ id: "osc", box: "freq", kind: "osc" });
	for (const m of [MODE.FM, MODE.DMR]) if (hasMode(m)) out.push({ id: `freq${m}`, box: "freq", kind: "freq", mode: m });
	for (const m of [MODE.FM, MODE.DMR]) if (S.core.devModes & (1 << m) && hasMode(m)) out.push({ id: `dev${m}`, box: "dev", kind: "dev", mode: m });
	return out;
}

function buildRow(r) {
	const row = el.rowTpl.content.firstElementChild.cloneNode(true);
	row.dataset.row = r.id;
	const q = (c) => row.querySelector(c);
	const kind = r.kind === "dev" ? KIND.DEV : KIND.FREQ;
	if (r.kind === "osc") {
		q(".tune-row-title").textContent = t("tools.tune.osc.title");
		q(".tune-unit-label").textContent = t("tools.tune.unit.ppm");
		stepper(row, () => S.ref / 100, (v) => setRef(Math.round(v * 100)));
		q(".tune-reset").addEventListener("click", () => setRef(0));
	} else {
		q(".tune-row-title").textContent = t(r.kind === "dev" ? "tools.tune.dev.row" : "tools.tune.freq.row", { mode: modeName(r.mode) });
		q(".tune-unit-label").textContent = t(r.kind === "dev" ? "tools.tune.unit.pct" : "tools.tune.unit.hz");
		const scale = r.kind === "dev" ? 1 : 10;	// the slider: 0.1 % or 10 Hz steps
		stepper(row, () => Math.round(valueHere(kind, r.mode, activeKhz()) / scale), (v) => {
			const max = r.kind === "dev" ? DEV_MAX : FREQ_MAX;
			setTrim(kind, r.mode, activeKhz(), Math.max(-max, Math.min(max, Math.round(v * scale))));
		});
		q(".tune-reset").addEventListener("click", () => clearTrim(kind, r.mode, activeKhz()));
	}
	q(".tune-key").addEventListener("click", () => transmit({ kind: r.kind, mode: r.mode ?? MODE.FM }));
	return row;
}

function renderTrims() {
	const rs = rows();
	const sig = rs.map((r) => r.id).join(",");
	if (el.freqRows.dataset.sig !== sig) {
		el.freqRows.dataset.sig = sig;
		el.freqRows.replaceChildren(...rs.filter((r) => r.box === "freq").map(buildRow));
		el.devRows.replaceChildren(...rs.filter((r) => r.box === "dev").map(buildRow));
	}
	el.dev.hidden = !rs.some((r) => r.box === "dev");
	el.freq.hidden = !rs.some((r) => r.box === "freq");
	const khz = activeKhz();
	for (const r of rs) {
		const row = document.querySelector(`[data-row="${r.id}"]`);
		const q = (c) => row.querySelector(c);
		const what = { kind: r.kind, mode: r.mode ?? MODE.FM };
		keyButton(q(".tune-key"), what);
		const mine = sameTx(S.tx, what) && S.tx.on;
		row.classList.toggle("is-keying", sameTx(S.tx, what));
		if (r.kind === "osc") {
			setRange(row, -REF_TRIM_MAX / 100, REF_TRIM_MAX / 100, S.ref / 100, false, 10, 1);
			q(".tune-row-sub").textContent = t("tools.tune.osc.sub", { hz: signed(trimHz(S.ref, khz)), mhz: mhzText(khz) });
			badge(q(".tune-badge"), S.ref ? "tuned" : "factory");
			q(".tune-badge").title = t("tools.tune.osc.saved_help");
			q(".tune-reset").disabled = !S.ref;
			q(".tune-hint").textContent = mine ? t("tools.tune.hint.rekey") : t("tools.tune.osc.hint");
			continue;
		}
		const kind = r.kind === "dev" ? KIND.DEV : KIND.FREQ;
		const v = valueHere(kind, r.mode, khz);
		if (r.kind === "dev") {
			setRange(row, -DEV_MAX, DEV_MAX, v, false, 10, 1);
			q(".tune-row-sub").textContent = t("tools.tune.dev.sub", { pct: signed(v / 10, 1) });
		} else {
			setRange(row, -FREQ_MAX / 10, FREQ_MAX / 10, Math.round(v / 10), false, 0.1, 0);
			q(".tune-row-sub").textContent = t("tools.tune.freq.sub", { hz: signed(v), mhz: mhzText(khz) });
		}
		const st = trimState(kind, r.mode, khz);
		badge(q(".tune-badge"), st);
		q(".tune-reset").disabled = !entry(kind, r.mode, khz);
		q(".tune-hint").textContent = mine ? t("tools.tune.hint.rekey") :
			t(r.kind === "dev" ? "tools.tune.dev.hint" : r.mode === MODE.DMR ? "tools.tune.freq.hint_dmr" : "tools.tune.freq.hint");
	}
}

function renderBattery() {
	const b = S.core?.battery;
	el.batt.hidden = !b;
	if (!b) return;
	const r = S.batt, range = battNow(), gain = battValue(BATT.GAIN);
	el.battVolts.textContent = r?.mv ? t("tools.tune.batt.now", { v: volts(r.mv), pct: num(r.pct) }) : t("tools.tune.batt.waiting");
	el.battRaw.textContent = r?.raw && gain ? t("tools.tune.batt.raw", { v: volts(r.raw) }) : "";
	for (const row of el.batt.querySelectorAll("[data-batt]")) {
		const sel = +row.dataset.batt, q = (c) => row.querySelector(c);
		const input = q(".tune-batt-in");
		badge(q(".tune-badge"), trimState(KIND.BATT, sel, 0));
		q(".tune-reset").disabled = !battEntry(sel);
		if (q(".tune-batt-now")) q(".tune-batt-now").disabled = !r?.mv;
		if (sel === BATT.GAIN) {
			q(".tune-row-sub").textContent = gain ? t("tools.tune.batt.gain_sub", { pct: signed(gain / 10, 1) }) : "";
			input.placeholder = r?.mv ? volts(r.mv) : "";
		} else {
			q(".tune-row-sub").textContent = t("tools.tune.batt.factory_sub", { v: volts(sel === BATT.EMPTY ? b.empty : b.full) });
			if (document.activeElement !== input) input.value = volts(sel === BATT.EMPTY ? range.empty : range.full);
		}
	}
	// Values the radio sets aside (an imported file): full too close to empty
	if (!range.ok) battHint(BATT.FULL, t("tools.tune.batt.err_span", { v: volts(BATT_SPAN_MV) }), "bad");
}

function renderTx() {
	if (S.tx?.kind === "drive") renderLevel(S.tx.level);
	else if (S.tx) renderTrims();
}

function renderSaved() {
	const es = [...S.work];
	for (const s of S.saved) {
		const same = (e) => e.kind === s.kind && e.sel === s.sel && (s.kind === KIND.NAME ? e.band === s.band : e.khz === s.khz);
		if (!es.some(same)) es.push({ ...s, removed: true });
	}
	const nSaved = S.saved.length, nStale = S.work.filter(stale).length;
	el.savedSum.textContent = es.length ? t("tools.tune.saved.sum", { n: nSaved, stale: nStale }) : t("tools.tune.saved.none");
	if (nStale) el.saved.open = true;
	const bandName = (khz) => {
		const b = txBands()[bandOf(khz)];
		return b ? bandInfo(b.lo, b.hi).name : "—";
	};
	const what = (e) => {
		if (e.kind === KIND.NAME) {
			const b = txBands().findIndex((x) => x.index === e.band);
			return t("tools.tune.saved.what_name", { level: b < 0 ? t("tools.tune.level_n", { n: e.sel + 1 }) : factoryName(b, e.sel) });
		}
		if (e.kind === KIND.DRIVE) {
			const b = bandOf(e.khz);
			return b < 0 ? t("tools.tune.level_n", { n: e.sel + 1 }) : shownName(b, e.sel).name;
		}
		if (e.kind === KIND.BATT) return t(BATT_TITLE[e.sel] || "tools.tune.batt.title");
		return t(e.kind === KIND.DEV ? "tools.tune.dev.row" : "tools.tune.freq.row", { mode: modeName(e.sel) });
	};
	const value = (e) => (e.kind === KIND.NAME ? e.name : e.kind === KIND.DRIVE ? signed(e.value) :
		e.kind === KIND.DEV || (e.kind === KIND.BATT && e.sel === BATT.GAIN) ? t("tools.tune.pct", { v: signed(e.value / 10, 1) }) :
		e.kind === KIND.BATT ? t("tools.tune.batt.v", { v: signed(e.value / 1000, 2) }) : t("tools.tune.hz", { v: signed(e.value) }));
	const group = (e) => (e.kind === KIND.NAME ? 2 : e.kind === KIND.BATT ? 1 : 0);
	const noFreq = (e) => e.kind === KIND.NAME || e.kind === KIND.BATT;
	const state = (e) => {
		if (e.removed) return "removed";
		if (stale(e)) return "stale";
		const s = S.saved.find((x) => canon([x]) === canon([e]));
		return s ? "saved" : "changed";
	};
	const rowsHtml = es.sort((a, b) => group(a) - group(b) || (a.khz || 0) - (b.khz || 0) || a.kind - b.kind || a.sel - b.sel)
		.map((e) => {
			const tr = document.createElement("tr");
			const st = state(e);
			tr.className = `is-${st}`;
			const cells = [noFreq(e) ? "—" : bandName(e.khz), noFreq(e) ? "—" : mhzText(e.khz), what(e),
				value(e), t(`tools.tune.entry.${st}`)];
			for (const c of cells) {
				const td = document.createElement("td");
				td.textContent = c;
				tr.append(td);
			}
			const td = document.createElement("td");
			if (!e.removed) {
				const b = document.createElement("button");
				b.type = "button";
				b.className = "btn btn-quiet";
				b.textContent = t("tools.tune.saved.delete");
				b.addEventListener("click", () => {
					S.work = S.work.filter((x) => x !== e);
					changed(e.kind, e.sel);
				});
				td.append(b);
			}
			tr.append(td);
			return tr;
		});
	el.savedBody.replaceChildren(...rowsHtml);
	el.export.disabled = !S.saved.length && !S.ref;
	el.import.disabled = !S.area;
	el.clear.disabled = !S.area || (!S.saved.length && !S.work.length && !S.ref);
}

// ---- wiring

el.connectBtn.addEventListener("click", () => {
	if (S.conn === "idle") connect();
	else disconnect(S.conn === "on");
});
el.save.addEventListener("click", saveAll);
el.discard.addEventListener("click", discard);
el.retry.addEventListener("click", () => { clearTimeout(S.timers.watch); lookAgain(); });
el.giveUp.addEventListener("click", () => disconnect(false));
el.confirm.addEventListener("change", () => el.confirm.closest(".card").classList.remove("tune-attention"));
el.add.addEventListener("submit", addFreq);
el.addCancel.addEventListener("click", () => { el.add.hidden = true; });
el.advanced.addEventListener("change", render);
el.export.addEventListener("click", exportFile);
el.import.addEventListener("change", () => {
	const f = el.import.files[0];
	el.import.value = "";
	if (f) importFile(f);
});
el.clear.addEventListener("click", clearAll);
for (const row of el.batt.querySelectorAll("[data-batt]")) {
	const sel = +row.dataset.batt, input = row.querySelector(".tune-batt-in");
	row.querySelector("form").addEventListener("submit", (e) => {
		e.preventDefault();
		battSubmit(sel, parseVolts(input.value));
		if (sel === BATT.GAIN) input.value = "";
		else input.blur();
	});
	row.querySelector(".tune-batt-now")?.addEventListener("click", () => { if (S.batt?.mv) battSubmit(sel, S.batt.mv / 1000); });
	row.querySelector(".tune-reset").addEventListener("click", () => {
		setBatt(sel, 0);
		battHint(sel, t("tools.tune.batt.applied"), "ok");
	});
	input.addEventListener("input", () => battHint(sel));
}
el.bands.addEventListener("keydown", (e) => {
	const n = txBands().length;
	if (!n || (e.key !== "ArrowRight" && e.key !== "ArrowLeft")) return;
	e.preventDefault();
	selectBand((S.band + (e.key === "ArrowRight" ? 1 : n - 1)) % n);
});
let stripW = 0;
new ResizeObserver(() => {
	const w = Math.round(el.strip.clientWidth);
	if (w && w !== stripW && S.conn === "on") { stripW = w; renderFreqs(); }
}).observe(el.strip);
if (serialSupported() && serialApi().addEventListener) {
	// A port coming back is looked at at once; one going away is the radio lost, without waiting for a timeout
	serialApi().addEventListener("connect", () => { if (S.conn === "lost") lookAgain(); });
	serialApi().addEventListener("disconnect", (e) => { if (S.conn === "on" && e.target === S.stream?.port) lost(); });
}
window.addEventListener("beforeunload", (e) => {
	if (S.conn !== "idle" && dirty()) e.preventDefault();
});
window.addEventListener("pagehide", () => { if (S.radio) quietly(S.radio.keyDown()); });
if (!serialSupported()) status(t("flash.err.no_serial"), "bad");
render();
