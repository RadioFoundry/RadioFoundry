// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's steps for the GD-77, which installs through its own update mode over USB HID (gd77.js, gd77codec.js,
// docs/spec/update-gd77.md). Backend "gd77-hid":
//
//   update_file   the user's own stock update file (.sgl): its header is kept, its codec is placed into the ODR image,
//                 and the image is written into a copy of it (as odrflash's flash --board gd77 --stock FILE does)
//   write         update mode (both side keys held at power-on), then the file through WebHID
//   done          what to check on the radio (ODR on the GD-77 has no USB link yet, so the page can't ask it)
//
// The loader below flash 0x4000 is never written, so update mode always remains: a failed or interrupted install is
// repeated from the start. Backup and restore are not offered. Not yet tried on a radio.

import { h, t, notice, button, panel, progressBar, logBox, filePicker } from "./dom.js";
import { startsWith } from "./bytes.js";
import { register } from "./backends.js";
import { UpdateFile, Updater, HidLink, hidSupported } from "./gd77.js";
import { readDonor, updateFile, ownImage } from "./gd77codec.js";
import { FakeHid, loaderFor } from "./fakegd77.js";

let ctx = null;			// from app.js: {S, next, render, reset, fb, helpLink, flashStarted, flashFinished, errorText}
const S = () => ctx.S;
const G = () => ctx.fb().gd77;

export function init(context) {
	ctx = context;
}

function session() {
	return (S().gd77 ||= { donor: null, out: null, error: null, noVoice: false, original: false, log: logBox() });
}

const kb = (n) => Math.ceil(n / 1024);
const simulated = () => !!S().sim?.gd77Donor;

// Keep the computer awake and warn before the page is closed while the radio is written
let wake = null;
const leaving = (e) => { e.preventDefault(); e.returnValue = ""; };
async function busy(on) {
	if (on) {
		window.addEventListener("beforeunload", leaving);
		try { wake = await navigator.wakeLock?.request("screen"); } catch (e) { /* not offered */ }
	} else {
		window.removeEventListener("beforeunload", leaving);
		try { await wake?.release(); } catch (e) { /* gone */ }
		wake = null;
	}
}

// The page's words for what the update file, the merge and the loader refuse (the codes of gd77.js and gd77codec.js)
function message(e) {
	const key = {
		not_sgl: "flash.gd77.err.not_sgl", version: "flash.gd77.err.not_sgl", damaged: "flash.gd77.err.damaged",
		sum: "flash.gd77.err.damaged", not_donor: "flash.gd77.err.damaged", too_large: "flash.gd77.err.too_large",
		tail: "flash.gd77.err.too_large", app_too_large: "flash.gd77.err.app_too_large", window: "flash.gd77.err.unsupported",
		refused: "flash.gd77.err.refused", no_update_mode: "flash.gd77.err.no_update_mode", no_answer: "flash.gd77.err.no_answer",
		no_device: "flash.gd77.err.no_device", no_hid: "flash.gd77.no_hid",
	}[e?.code];
	if (!key) return ctx.errorText(e);
	ctx.errorText(e);	// logged with the step
	return t(key, { detail: e.message });
}

// The update file for what this run installs: ODR with the codec, ODR without it (the user's choice for a stock build
// the page doesn't know), or the stock file unchanged (back to the original firmware)
async function build() {
	const g = session();
	g.out = null;
	g.error = null;
	if (!g.donor) return;
	try {
		if (g.original) g.out = { file: g.donor.file.data, image: null, profile: null };
		else if (g.donor.spec || g.noVoice) g.out = await updateFile(G(), g.donor, S().firmware.image, { codec: g.noVoice ? "keep" : "auto" });
	} catch (e) {
		g.error = message(e);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// update_file: the user's own stock update file

function fileView() {
	const g = session();
	const wrap = h("div", { class: "flasher" });
	const take = async (f) => {
		g.donor = null;
		g.noVoice = false;
		try {
			g.donor = { name: f.name, ...(await readDonor(G(), f.data)) };
			await build();
		} catch (e) {
			g.out = null;
			g.error = message(e);
		}
		draw();
	};
	const draw = () => {
		const d = g.donor;
		const versions = Object.values(G().specs).map((s) => s.stock?.name || s.profile).sort().join(", ");
		const picker = simulated() ? button(t("flash.sim.use_stock"), () => take({ name: "practice-update-file.sgl", data: S().sim.gd77Donor }))
			: filePicker(t("flash.gd77.file.pick"), ".sgl,.SGL", take);
		const rows = d ? [
			[t("flash.gd77.file.name"), d.name],
			[t("flash.gd77.file.radio"), d.file.label()],
			g.original ? null : [t("flash.gd77.file.codec"), d.spec ? t("flash.gd77.file.codec_found", { build: d.spec.stock?.name || d.spec.profile })
				: t("flash.gd77.file.codec_unknown")],
			g.out?.image ? [t("flash.gd77.file.size"), t("flash.gd77.file.size_of", { used: kb(g.out.image.length), room: kb(d.file.room) })] : null,
		].filter(Boolean) : [];
		const unknown = d && !d.spec && !g.original;
		wrap.replaceChildren(panel(t(g.original ? "flash.gd77.file.title_original" : "flash.gd77.file.title"),
			notice(t("flash.gd77.unverified"), "warn"),
			h("p", {}, t(g.original ? "flash.gd77.file.lead_original" : "flash.gd77.file.lead")),
			g.original ? null : h("p", { class: "small muted" }, t("flash.gd77.file.versions", { versions })),
			picker,
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			unknown && !g.noVoice ? notice(t("flash.gd77.file.unsupported", { versions }), "bad") : null,
			unknown && g.noVoice ? notice(t("flash.gd77.file.no_voice"), "info") : null,
			g.error ? notice(g.error, "bad") : null));
		const noVoice = unknown && !g.error ? button(t(g.noVoice ? "flash.gd77.file.with_voice" : "flash.gd77.file.without_voice"), async () => {
			g.noVoice = !g.noVoice;
			await build();
			draw();
		}, { quiet: true }) : null;
		S().forward([noVoice, button(t("flash.nav.to_write"), ctx.next, { primary: true, disabled: !g.out })]);
	};
	// coming back after another build was chosen: the file is made again for it
	if (g.donor) build().then(draw);
	draw();
	return wrap;
}

// ---------------------------------------------------------------------------------------------------------------
// write: update mode, then the file

function writeView() {
	const g = session();
	const noHid = !simulated() && !hidSupported();
	const prog = progressBar();
	const status = h("div");
	prog.el.hidden = true;
	const what = g.original ? t("flash.gd77.write.what_original", { name: g.donor.name })
		: g.out.profile ? t("flash.gd77.write.what_voice", { build: g.donor.spec.stock?.name || g.out.profile })
		: t("flash.gd77.write.what_no_voice");
	const go = button(t("flash.gd77.write.go"), () => run(), { primary: true, disabled: noHid });
	const box = h("div", { class: "flasher" }, panel(t("flash.gd77.write.title"),
		noHid ? notice(t("flash.gd77.no_hid"), "bad") : null,
		notice(t("flash.gd77.unverified"), "warn"),
		h("dl", { class: "kv-list" }, h("dt", {}, t("flash.gd77.write.what")), h("dd", {}, what)),
		h("p", {}, t("flash.gd77.write.lead")),
		h("ol", { class: "steps" }, S().radio.flash.bootloader.map((x) => h("li", {}, x)), h("li", {}, t("flash.gd77.write.step_go"))),
		h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.gd77.write.careful"))), h("p", {}, t("flash.gd77.write.careful_text"))),
		prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.gd77.log")), g.log.el)));
	S().forward(go);

	async function run() {
		go.disabled = true;
		status.replaceChildren(h("p", { class: "muted" }, t("flash.gd77.write.choose")));
		let link = null, started = null;
		try {
			link = await HidLink.request(simulated() ? new FakeHid(loaderFor(g.out.file)) : undefined);
			g.log.add(`radio    ${link.dev.productName || "update mode"} (${link.dev.vendorId?.toString(16)}:${link.dev.productId?.toString(16)})`);
			const file = new UpdateFile(g.out.file);
			g.log.add(`file     ${file.label()}, ${file.length} bytes`);
			await busy(true);
			const fw = S().firmware;
			started = ctx.flashStarted({ task: g.original ? "restore" : "install", source: g.original ? "stock" : fw?.origin || "",
				radio: S().radio.flash.board, firmware: g.original ? "stock" : String(fw?.manifest.version || "") });
			status.replaceChildren(h("p", { class: "muted" }, t("flash.gd77.write.running")));
			prog.el.hidden = false;
			let last = -1;
			await new Updater(link).flash(file, { progress: (done, total) => {
				const pct = Math.floor((100 * done) / total);
				if (pct === last) return;
				last = pct;
				prog.set(done, total, t("flash.gd77.write.writing"));
				if (pct % 10 === 0) g.log.add(`write    ${pct}%`);
			} });
			ctx.flashFinished(started);
			status.replaceChildren(notice(t("flash.gd77.write.done"), "ok"));
			S().forward(button(t("flash.nav.to_done"), ctx.next, { primary: true }));
		} catch (e) {
			if (started) ctx.flashFinished(started, e);
			const text = message(e);
			g.log.add(`error    ${e?.message || e}`);
			prog.el.hidden = true;
			status.replaceChildren(notice(text, "bad"), notice(t("flash.gd77.write.retry_safe"), "info"), ctx.helpLink());
			go.disabled = false;
			S().forward(button(t("flash.gd77.write.again"), () => run(), { primary: true }));
		} finally {
			await link?.close();
			await busy(false);
		}
	}
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// done: what to check on the radio

function doneView() {
	const g = session();
	const original = g.original;
	S().forward(button(t("flash.nav.finish"), () => ctx.reset(), { primary: true }));
	return h("div", { class: "flasher" }, panel(t(original ? "flash.gd77.done.title_original" : "flash.gd77.done.title"),
		h("p", {}, t(original ? "flash.gd77.done.lead_original" : "flash.gd77.done.lead")),
		original ? null : h("ul", { class: "steps" }, h("li", {}, t("flash.gd77.done.check_screen")),
			h("li", {}, t(g.out?.profile ? "flash.gd77.done.check_voice" : "flash.gd77.done.no_voice"))),
		h("p", { class: "small muted" }, t("flash.gd77.done.no_link")),
		notice(t("flash.gd77.done.report"), "info"),
		h("p", { class: "small" }, t("flash.gd77.done.not_starting"))));
}

// ---------------------------------------------------------------------------------------------------------------
// the backend (backends.js)

const INSTALL = ["radio", "build", "update_file", "write", "done"];
const RECOVER = ["build", "update_file", "write", "done"];
const ORIGINAL = ["update_file", "write", "done"];
export const isGd77 = (radio, fb) => radio?.flash.backend === "gd77-hid" && !!fb.gd77?.boards.includes(radio.flash.board);

register({
	id: "gd77-hid",
	// install only: the loader can't read the radio back, so there is no backup, and so nothing to restore
	available: (radio, fb, task) => task === "install" && isGd77(radio, fb),
	steps: (task, { recovering }) => (task !== "install" ? null : recovering && ctx?.S.gd77?.original ? ORIGINAL : recovering ? RECOVER : INSTALL),
	stepLabel: (step) => (step === "done" ? "gd77_done" : step),
	views: { update_file: fileView, write: writeView, done: doneView },
	// update mode is in the loader, which the page never writes: reinstalling, or putting the original update file back,
	// always works the same way
	recovery: { first: "fix", stock: "file", reach: "flash.help.gd77_reach", stockText: "flash.help.fix.stock_text",
		choices: ["again"], againText: "flash.help.fix.gd77_again_text", alias: { codec: "again", stock: "original" },
		extra: () => [{ id: "original", title: "flash.help.fix.gd77_original", text: "flash.help.fix.gd77_original_text",
			prepare: () => { session().original = true; } }] },
	ownImage: async (fb, board, data) => {
		if (startsWith(data, "SGL!")) throw new Error(t("flash.gd77.err.sgl_as_image"));
		return ownImage(fb.gd77, data);
	},
	release: async () => { if (ctx?.S.gd77) ctx.S.gd77 = null; },
});
