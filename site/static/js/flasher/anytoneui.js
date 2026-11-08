// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The pages of the AnyTone update mode backend (backends.js id "anytone-update", protocol in anytone.js): install
// RadioFoundry, and put the original firmware back from the user's vendor firmware set. The radio is identified as the
// manufacturer's updater does it (its model answer), mapped to a [hardware] variant; a variant the user picked is checked
// against it, and a variant without a RadioFoundry image is refused for the install (its original firmware may still go
// back). docs/dev/web-flasher.md "Radio access backends".
//
//   install   radio, build, at_channels (optional: the original firmware's codeplug, read in its PC mode and converted),
//             connect, at_boot (the DMR baseband's boot block from the user's stock file), write, done (the channels
//             written through RadioFoundry's CPS once it runs: app.js bringChannels)
//   restore   at_files (the .CDD, .CDI and .spi), connect, restore (reached from the help task, the radio known)

import { h, t, notice, button, panel, progressBar, logBox } from "./dom.js";
import { register } from "./backends.js";
import { serialSupported, SerialStream } from "./link.js";
import { putBlob, getBlob } from "./codecsave.js";
import { radioPicture, holdKeys } from "./radiotop.js";
import { errorNotice } from "../report.js";
import * as diag from "../diag.js";
import { AnytoneUpdate, AnytoneProgram, AnytoneError, BAUD, judge, variantOf, variantLabel, stockSet, sortSet, checkFits, findBootBlock,
	installImage, splitImage, framesNeeded } from "./anytone.js";
import { FakeAnytone, stockAnytone, SAMPLE_ANYTONE } from "./fakeanytone.js";
import { confirmVariant, portFilters } from "./detect.js";
import { detectControl } from "./detectui.js";
import { verdict, writable } from "./variant.js";
import { PROBES as VARIANT_PROBES } from "./anytonedetect.js";

export const BACKEND = "anytone-update";
export const INSTALL = ["radio", "build", "at_channels", "connect", "at_boot", "write", "done"];
export const RECOVER = ["build", "connect", "at_boot", "write", "done"];
export const RESTORE = ["at_files", "connect", "restore"];

let ctx = null;		// from app.js: {S, next, render, fb, flashStarted, flashFinished, flashMark, errorText}
const S = () => ctx.S;
const board = () => ctx.fb().boards[S().radio.flash.board];
const blobId = () => `anytone-boot-${S().radio.flash.board}`;
const errText = (e) => (ctx.errorText ? ctx.errorText(e) : String(e?.message || e));
const fill = (el, ...parts) => el.replaceChildren(...parts.filter(Boolean));
// the update mode's key steps beside the radio's picture, with the keys to hold marked (radiotop.js)
function withPicture(radio, steps) {
	const pic = radioPicture(radio, { keys: holdKeys(radio) });
	return pic ? h("div", { class: "radio-grid" }, steps, pic) : steps;
}

export function init(context) {
	ctx = context;
}

// This backend's state for the visit: the port, what the radio answered, the boot block, the vendor set
function session() {
	return (S().at ||= { stream: null, radio: null, ident: null, verdict: null, block: null, blockFrom: null, set: null, channels: null,
		log: logBox() });
}

export async function release() {
	const a = S()?.at;
	if (!a?.stream) return;
	try { await a.stream.close(); } catch (e) { /* gone */ }
	a.stream = null;
	a.ident = null;
}

// The task this connection is for, and what it must pass (anytone.js judge)
const task = () => (S().task === "restore" ? "restore" : "install");
function verdictNow() {
	const a = session();
	if (!a.ident) return null;
	return judge({ board: board(), radio: S().radio, model: a.ident.model, task: task(), chosen: S().atChosen || null,
		spiModels: a.set?.models || null });
}

// The radios a practice run simulates: the first model of a variant with an image (or any, for a restore)
function fakeModel() {
	const r = S().radio, models = board().anytone_models || {};
	const code = (r.variants || []).find((v) => v.runs?.board === r.flash.board && models[v.code])?.code || Object.keys(models)[0];
	return models[code]?.[0] || "RADIO";
}

const simRadio = () => (S().fakeAnytone ||= new FakeAnytone({ model: fakeModel(), memory: stockAnytone(SAMPLE_ANYTONE), pairs: true }));

async function openPort() {
	const a = session();
	if (a.stream && !a.stream.lost) return a.stream;
	if (S().sim) return (a.stream = simRadio().stream);
	if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
	// the update mode's USB ids are the vendor's and not known here: any port, the user picks the radio's
	const port = await navigator.serial.requestPort();
	a.stream = await SerialStream.open(port, BAUD);
	return a.stream;
}

// For the variant probes (detect.js): the port asked for once, opened again at each probe's rate
function probeOpener() {
	let port = null, every = false;
	const open = async (baud) => {
		if (S().sim) return simRadio().stream;
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		port ||= await navigator.serial.requestPort(every ? {} : { filters: portFilters(S().radio) });
		return SerialStream.open(port, baud || BAUD);
	};
	// the update mode's USB id isn't known (hardware-d578uv.md 1), so the filtered chooser may not list a radio in it
	open.showAll = () => { every = true; port = null; };
	return open;
}

// ---------------------------------------------------------------------------------------------------------------
// connect: update mode, hello, identify, the variant

// The variants as a choice, with the Auto-detect button (detector: detectControl's parts, or null) beside them
function variantChooser(onPick, detector) {
	const r = S().radio;
	if (!r.variants?.length) return null;
	const now = S().atChosen || null;
	return h("div", {}, h("p", {}, h("b", {}, t("flash.at.variant_title")), " ", r.variant_check || ""),
		h("div", { class: "choice-grid" }, r.variants.map((v) => h("button", { class: "choice", type: "button", "aria-pressed": String(now === v.code),
			onclick: () => { S().atChosen = v.code; onPick(); } }, h("strong", {}, `${v.code}: ${v.name}`))), detector?.go),
		detector?.hint, detector?.say);
}

// The build step's variant question (backends.js buildGate). The answer is required, picked or detected: a variant with
// no firmware stops the step before the user spends a build on it. The port a probe left in the update mode is closed:
// the steps until connect need the radio in its normal mode
export function buildGate(redraw) {
	const r = S().radio;
	const open = probeOpener();
	const detector = detectControl({ radio: r, board: board(), fb: ctx.fb(), ids: VARIANT_PROBES, open, showAllPorts: open.showAll, showBlocked: false,
		log: (l) => session().log.add(l), onFound: async (f) => {
			S().atChosen = f.code;
			S().variant = { version: f.version, code: f.code, via: f.via };
			if (f.keep) try { await f.stream.close(); } catch (e) { /* gone */ }
			redraw();
		} });
	// a pick left over from another radio doesn't count
	const known = () => !!r.variants?.some((v) => v.code === S().atChosen);
	const judged = () => (known() ? verdict(r, S().atChosen, r.flash.board, S().buildOptions || {}) : null);
	const refusal = (vd) => {
		const what = `${vd.code} (${vd.variant.name})`;
		return vd.variant.blocked ? t("flash.detect.blocked", { why: vd.variant.blocked })
			: vd.kind === "other_board" ? t("flash.variant.other_board", { variant: what, radio: (S().data?.radios || []).find((x) => x.flash.board === vd.board)?.name || vd.board })
			: vd.kind === "options" ? t("flash.variant.options", { variant: what, options: Object.entries(vd.need).map(([n, v]) => `${n} ${v}`).join(", ") })
			: t("flash.variant.unsupported", { variant: what, radio: r.name });
	};
	const refused = () => { const vd = judged(); return !!vd && !writable(vd); };
	return { refused, need: () => (known() || !r.variants?.length ? null : t("flash.at.variant")),
		view: () => h("div", {}, variantChooser(() => { S().variant = null; redraw(); }, detector),
			refused() ? notice(refusal(judged()), "bad") : null) };
}

export function connectView() {
	const a = session();
	const box = h("div", { class: "flasher" });
	const status = h("div");
	// made once, so what it found stays shown when the step is drawn again
	const open = probeOpener();
	const detector = detectControl({ radio: S().radio, board: board(), fb: ctx.fb(), ids: VARIANT_PROBES, open, showAllPorts: open.showAll,
		log: (l) => a.log.add(l), onFound: (f) => adopt(f),
		nextHint: (f) => (f.keep ? null : "flash.detect.then_update") });
	const draw = () => {
		const vd = verdictNow();
		a.verdict = vd;
		const r = S().radio;
		fill(box, panel(t("flash.at.connect_title"),
			h("p", {}, t("flash.at.connect_lead")),
			withPicture(r, h("ol", { class: "steps" }, r.flash.bootloader.map((x) => h("li", {}, x)))),
			variantChooser(() => draw(), a.ident ? null : detector),
			a.ident ? h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.at.model")), h("dd", {}, `${a.ident.model} (${t("flash.at.hw")} ${a.ident.hw}, ${a.ident.version})`),
				h("dt", {}, t("flash.at.variant")), h("dd", {}, vd?.code ? variantLabel(r, vd.code) : t("flash.at.variant_unknown"))) : null,
			vd && !vd.ok ? notice(t(`flash.at.refuse_${vd.why}`, { why: vd.text }), "bad") : null,
			vd?.ok ? notice(t(task() === "install" ? "flash.at.ok_install" : "flash.at.ok_restore"), "ok") : null,
			status, a.log.el));
		S().forward(vd?.ok ? button(t("flash.at.continue"), () => ctx.next(), { primary: true })
			: button(t(a.ident ? "flash.at.connect_again" : "flash.at.connect"), connect, { primary: true }));
	};
	// A probe named the variant: it becomes the pick; one that left the update mode open is the connection
	async function adopt(found) {
		S().atChosen = found.code;
		S().variant = { version: found.version, code: found.code, via: found.via };
		if (found.keep && found.radio) {
			await release();
			Object.assign(a, { stream: found.stream, radio: found.radio, ident: found.ident });
			diag.note("anytone identify", { model: found.ident.model, hw: found.ident.hw, version: found.ident.version, variant: found.code });
		}
		draw();
	}
	async function connect() {
		fill(status, notice(t("flash.at.connecting"), "info"));
		try {
			await release();
			const stream = await openPort();
			const u = new AnytoneUpdate(stream, { log: (l) => a.log.add(l) });
			await u.hello();
			a.ident = await u.identify();
			a.radio = u;
			diag.note("anytone identify", { model: a.ident.model, hw: a.ident.hw, version: a.ident.version, variant: variantOf(board(), a.ident.model) });
			S().variant = { version: a.ident.model, code: variantOf(board(), a.ident.model), via: "update-mode" };
			fill(status);
		} catch (e) {
			a.ident = null;
			await release();
			fill(status, errorNotice(e instanceof AnytoneError ? t("flash.at.no_update_mode", { why: e.message }) : errText(e)));
		}
		draw();
	}
	draw();
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// at_channels: the original firmware's codeplug, read in its PC mode before the install (migrate.js readAnytone), so
// the channels can be written once RadioFoundry runs. RadioFoundry keeps its own codeplug elsewhere in the radio's
// memory and can't read the original's (docs/spec/codeplug-stock-anytone.md)

export function channelsView() {
	const a = session();
	const box = h("div", { class: "flasher" });
	const bar = progressBar();
	const status = h("div");
	const draw = () => {
		const c = a.channels;
		fill(box, panel(t("flash.at.ch_title"), h("p", {}, t("flash.at.ch_lead")),
			h("ol", { class: "steps" }, h("li", {}, t("flash.at.ch_step_on")), h("li", {}, t("flash.at.ch_step_connect"))),
			c ? notice(t("flash.at.ch_have", { counts: t("flash.channels.counts", c.counts) }), "ok") : null,
			c?.notes.length ? h("ul", { class: "small" }, c.notes.map((x) => h("li", {}, t(`editor.qdmr.note.${x.code}`, { n: x.n })))) : null,
			bar.el, status, a.log.el));
		S().forward([button(t(c ? "flash.at.continue" : "flash.at.ch_skip"), () => ctx.next(), { quiet: !c, primary: !!c }),
			c ? null : button(t("flash.at.ch_read"), read, { primary: true })].filter(Boolean));
	};
	async function read() {
		S().forward(null);
		fill(status, notice(t("flash.at.ch_reading"), "info"));
		let p = null;
		try {
			const m = await import("./migrate.js");
			await release();
			const stream = await openPort();
			p = new AnytoneProgram(stream, { log: (l) => a.log.add(l) });
			await p.hello();
			const tables = await m.readAnytone((addr, n) => p.read(addr, n), (d, n) => bar.set(d, n, t("flash.channels.reading")));
			a.channels = m.convertAnytone(tables);
			diag.note("anytone channels", { ...a.channels.counts, notes: a.channels.notes.map((x) => `${x.code}:${x.n}`).join(" ") });
			fill(status, a.channels.counts.channels ? null : notice(t("flash.channels.none"), "info"));
			if (!a.channels.counts.channels) a.channels = null;
		} catch (e) {
			diag.error("anytone channels", e);
			a.channels = null;
			fill(status, errorNotice(e instanceof AnytoneError ? t("flash.at.ch_failed", { why: e.message }) : errText(e)));
		}
		// the radio restarts on END; it is turned off for the update mode next anyway
		try { await p?.end(); } catch (e) { /* already gone */ }
		await release();
		draw();
	}
	draw();
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// install: the boot block, the write, the end

export function bootView() {
	const a = session();
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const fbd = ctx.fb();
	async function take(name, data) {
		const blk = await findBootBlock(data, fbd.modem_boot);
		if (!blk) return fill(status, notice(t("flash.at.boot_none", { name }), "bad"));
		a.block = blk;
		a.blockFrom = name;
		await putBlob(blobId(), blk);
		draw();
	}
	const draw = () => {
		const picker = h("input", { type: "file", accept: ".cdd,.CDD,.bin" });
		picker.addEventListener("change", async () => {
			const f = picker.files?.[0];
			if (f) await take(f.name, new Uint8Array(await f.arrayBuffer()));
		});
		fill(box, panel(t("flash.at.boot_title"), h("p", {}, t("flash.at.boot_lead", { files: board().codec.sources.join(", ") })),
			a.block ? notice(t("flash.at.boot_have", { name: a.blockFrom }), "ok") : null,
			h("label", { class: "file-pick" }, h("span", {}, t(a.block ? "flash.at.boot_other" : "flash.at.boot_pick")), picker),
			h("p", { class: "small muted" }, t("flash.at.boot_private")), status));
		S().forward(button(t("flash.at.continue"), () => ctx.next(), { primary: true, disabled: !a.block }));
	};
	if (!a.block) {
		getBlob(blobId()).then((b) => {
			if (b && !a.block) { a.block = b; a.blockFrom = t("flash.at.boot_saved"); draw(); }
		});
	}
	draw();
	return box;
}

// The write itself, for the install and the restore: what is written, Install/Stop, progress, the outcome
function writePanel({ title, rows, addr, data, report, okText, onDone }) {
	const a = session();
	const box = h("div", { class: "flasher" });
	const bar = progressBar();
	const status = h("div");
	let abort = null;
	const stop = () => abort?.abort();
	async function go() {
		const vd = verdictNow();
		if (!vd?.ok || !a.radio) return fill(status, notice(t("flash.at.reconnect"), "bad"));
		abort = new AbortController();
		S().forward(button(t("flash.at.stop"), stop, { danger: true }));
		fill(status, notice(t("flash.at.writing", { n: framesNeeded(data.length) }), "info"));
		try {
			// mandatory, just before the first frame: the radio asked once more on this session (detect.js)
			// the install also needs the variant that runs this image; a restore's set was checked against the model at connect
			await confirmVariant({ id: "anytone-update", session: a.radio, radio: S().radio, board: board(), fb: ctx.fb(),
				expect: vd.code, imageBoard: report.task === "install" ? board().id : null, log: (l) => a.log.add(l) });
		} catch (e) {
			diag.note("anytone confirm", { error: errText(e) });
			fill(status, errorNotice(t("flash.at.confirm_failed", { why: errText(e) })));
			a.radio = null;
			await release();
			return S().forward(button(t("flash.at.connect_again_go"), () => {
				const list = task() === "restore" ? RESTORE : S().recovering ? RECOVER : INSTALL;
				S().step = list.indexOf("connect");
				ctx.render();
			}, { primary: true }));
		}
		const started = ctx.flashStarted?.({ ...report, variant: report.variant || vd.code || "" });
		try {
			let first = true;
			await a.radio.write(addr, data, { signal: abort.signal, onFrame: (i, n) => {
				if (first) { first = false; ctx.flashMark?.(started, "first_byte"); }
				if (i === n || i % 64 === 0) bar.set(i, n, t("flash.at.frames", { i, n }));
			} });
			ctx.flashMark?.(started, "last_byte");
			await ctx.flashFinished?.(started);
			a.radio = null;
			await release();
			fill(status, notice(okText, "ok"));
			S().forward(button(t("flash.at.continue"), onDone, { primary: true }));
		} catch (e) {
			await ctx.flashFinished?.(started, e);
			fill(status, errorNotice(t("flash.at.write_failed", { why: errText(e) })));
			S().forward(button(t("flash.at.write_again"), go, { primary: true }));
		}
	}
	fill(box, panel(title, h("dl", { class: "kv-list" }, rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])),
		h("p", {}, t("flash.at.write_lead")), bar.el, status, a.log.el));
	S().forward(button(t("flash.at.write_go"), go, { primary: true }));
	return box;
}

export function writeView() {
	const a = session();
	const fw = S().firmware;
	let data;
	try {
		data = installImage(board(), fw.image, a.block);
		checkFits(board(), board().app_base, data);
	} catch (e) {
		return panel(t("flash.c7.write.title"), errorNotice(errText(e)));
	}
	return writePanel({ title: t("flash.c7.write.title"), addr: board().app_base, data,
		rows: [[t("flash.c7.write.firmware"), `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`],
			[t("flash.at.boot_row"), a.blockFrom || ""],
			[t("flash.at.variant"), variantLabel(S().radio, a.verdict?.code)]],
		report: { task: "install", source: fw.origin || "", radio: S().radio.flash.board, firmware: String(fw.manifest?.version || ""),
			features: fw.manifest?.features || "", build: fw.build || null },
		okText: t("flash.at.install_done"), onDone: () => ctx.next() });
}

export function doneView() {
	const a = session();
	S().forward(null);
	const status = h("div");
	const finish = () => button(t("flash.nav.finish"), () => ctx.reset(), { primary: true });
	// the channels read before the install: written through RadioFoundry's CPS once it has started
	if (a.channels && ctx.bringChannels) {
		S().forward(button(t("flash.at.ch_write"), async () => {
			S().forward(null);
			await release();
			await ctx.bringChannels(status, { finish, converted: a.channels });
		}, { primary: true }));
	}
	return h("div", { class: "flasher" }, panel(t("flash.at.done_title"), h("p", {}, t("flash.at.done_text")),
		a.channels ? h("p", {}, t("flash.at.ch_after", { counts: t("flash.channels.counts", a.channels.counts) })) : null,
		h("p", { class: "small muted" }, t("flash.at.done_dark")), status));
}

// ---------------------------------------------------------------------------------------------------------------
// restore: the vendor set, then the write

export function filesView() {
	const a = session();
	const box = h("div", { class: "flasher" });
	const status = h("div");
	const picker = h("input", { type: "file", multiple: true, accept: ".cdd,.cdi,.spi,.CDD,.CDI,.SPI" });
	picker.addEventListener("change", async () => {
		try {
			const files = await Promise.all([...(picker.files || [])].map(async (f) => ({ name: f.name, data: new Uint8Array(await f.arrayBuffer()) })));
			const set = sortSet(files);
			const st = stockSet(set);
			checkFits(board(), st.addr, st.image);
			a.set = { ...st, names: set.names };
			diag.note("anytone set", { models: st.models, size: st.image.length });
			draw();
		} catch (e) {
			a.set = null;
			fill(status, notice(t("flash.at.files_bad", { why: errText(e) }), "bad"));
			draw(false);
		}
	});
	const draw = (clear = true) => {
		if (clear) fill(status);
		fill(box, panel(t("flash.at.files_title"), h("p", {}, t("flash.at.files_lead")),
			h("label", { class: "file-pick" }, h("span", {}, t("flash.at.files_pick")), picker),
			a.set ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.at.files_for")), h("dd", {}, a.set.models.join(", ")),
				h("dt", {}, t("flash.at.files_size")), h("dd", {}, `${Math.round(a.set.image.length / 1024)} KB`)) : null, status));
		S().forward(button(t("flash.at.continue"), () => ctx.next(), { primary: true, disabled: !a.set }));
	};
	draw();
	return box;
}

export function restoreView() {
	const a = session();
	if (!a.set) return panel(t("flash.at.files_title"), notice(t("flash.at.files_missing"), "bad"));
	return writePanel({ title: t("flash.at.restore_title"), addr: a.set.addr, data: a.set.image,
		rows: [[t("flash.at.files_for"), a.set.models.join(", ")], [t("flash.at.variant"), variantLabel(S().radio, a.verdict?.code)]],
		report: { task: "restore", source: "stock", radio: S().radio.flash.board, firmware: "stock" },
		okText: t("flash.at.restore_done"), onDone: () => { S().forward(null); ctx.reset?.(); } });
}

// ---------------------------------------------------------------------------------------------------------------

// A merged image file (tools/modem_boot.py output) keeps its boot block for the boot step
async function ownImage(fbd, boardId, data) {
	const { image, block } = await splitImage(fbd.boards[boardId], data, fbd.modem_boot);
	if (block) Object.assign(session(), { block, blockFrom: t("flash.at.boot_from_image") });
	return image;
}

const LABELS = { at_channels: "at_channels", at_boot: "at_boot", at_files: "at_files", done: "at_done" };
register({
	id: BACKEND,
	// install where the board's models are known; nothing is read back, so no backup
	available: (radio, fbd, tk) => tk === "install" && !!Object.keys(fbd.boards[radio.flash.board]?.anytone_models || {}).length,
	steps: (tk, { recovering }) => (tk === "install" ? (recovering ? RECOVER : INSTALL) : tk === "restore" ? RESTORE : null),
	stepLabel: (step) => LABELS[step] || step,
	views: { at_channels: channelsView, connect: connectView, at_boot: bootView, write: writeView, done: doneView, at_files: filesView, restore: restoreView },
	recovery: { first: "fix", stock: "restore", reach: "flash.help.at_reach", stockText: "flash.help.fix.stock_at" },
	ownImage,
	release,
	buildGate,
});
