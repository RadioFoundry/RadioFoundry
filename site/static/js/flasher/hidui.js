// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's steps for the radios of the "sgl-hid" backend. Two routes: the radio's own update mode over USB HID
// (hidboot.js, k22fcodec.js, docs/spec/update-gd77.md), which every radio has, and, on a radio that already runs ODR
// built with CONFIG_STUB_ENTRY, the cable route: ODR hands over to the K22F stub on its debug link, which writes only
// the changed flash sectors (k22stub.js, docs/spec/stub-k22f.md).
//
//   connect       the radio running ODR answers on its debug link and offers stub entry: the cable route; otherwise
//                 (or when the user chooses it) update mode
//   update_file   update mode: the user's own stock update file (.sgl): its header is kept, its codec is placed into the
//                 ODR image, and the image is written into a copy of it (as odrflash's flash --board BOARD --stock FILE
//                 does). The cable route: the file is optional; with it the codec is merged as above, without it only
//                 the application is written and the codec in the radio stays
//   write         update mode (the board's keys held at power-on), then the file through WebHID; or the stub: CONFIG,
//                 changed sectors in the interlock's order, CRC verify, restart
//   done          what to check on the radio
//
// The loader below flash 0x4000 is never written, so update mode always remains: a failed or interrupted install is
// repeated from the start there. Backup and restore are not offered. Neither route has been tried on a radio.

import { h, t, notice, button, panel, progressBar, logBox, filePicker, download, ROOT } from "./dom.js";
import { hex, startsWith } from "./bytes.js";
import { register } from "./backends.js";
import { UpdateFile, Updater, HidLink, hidSupported } from "./hidboot.js";
import { readDonor, updateFile, ownImage, merge, pack } from "./k22fcodec.js";
import { FakeHid, loaderFor } from "./fakehid.js";
import { Link, SerialStream, ODR_USB, probe, serialSupported } from "./link.js";
import { Stub } from "./stub.js";
import { f4Image } from "./f4stub.js";
import { k22Board, k22Stub, k22Install } from "./k22stub.js";

let ctx = null;			// from app.js: {S, next, render, reset, fb, helpLink, flashStarted, flashFinished, errorText}
const S = () => ctx.S;
const G = () => ctx.fb().hid;

export function init(context) {
	ctx = context;
}

function session() {
	// route: "cable" (the stub on a radio running ODR) or "update" (update mode); link: ODR's debug link while held
	return (S().hid ||= { donor: null, out: null, error: null, noVoice: false, original: false, log: logBox(), route: null, link: null, result: null });
}

const kb = (n) => Math.ceil(n / 1024);
const simulated = () => !!S().sim?.hidDonor;
const simOdr = () => S().sim?.hidOdr || null;
// the cable route only on a plain install: the help task's reinstall and the original firmware go through update mode
const cable = () => session().route === "cable" && !session().original && !S().recovering;

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

// The page's words for what the update file, the merge and the loader refuse (the codes of hidboot.js and k22fcodec.js)
function message(e) {
	const key = {
		not_sgl: "flash.hid.err.not_sgl", version: "flash.hid.err.not_sgl", damaged: "flash.hid.err.damaged",
		sum: "flash.hid.err.damaged", not_donor: "flash.hid.err.damaged", too_large: "flash.hid.err.too_large",
		tail: "flash.hid.err.too_large", app_too_large: "flash.hid.err.app_too_large", window: "flash.hid.err.unsupported",
		refused: "flash.hid.err.refused", no_update_mode: "flash.hid.err.no_update_mode", no_answer: "flash.hid.err.no_answer",
		no_device: "flash.hid.err.no_device", no_hid: "flash.hid.no_hid",
	}[e?.code];
	if (!key) return ctx.errorText(e);
	ctx.errorText(e);	// logged with the step
	return t(key, { detail: e.message });
}

// The update file for what this run installs: ODR with the codec, ODR without it (the user's choice for a stock build
// the page doesn't know), or the stock file unchanged (back to the original firmware). The cable route needs no update
// file: ODR merged with the codec of the user's file, or without a file ODR alone (the radio's codec stays).
async function build() {
	const g = session();
	g.out = null;
	g.error = null;
	const app = S().firmware?.image;
	try {
		if (cable()) {
			if (!g.donor) g.out = { image: app, profile: null, keep: true };
			else if (g.donor.spec) g.out = { image: merge(G(), app, await pack(G(), g.donor.flat, g.donor.spec)), profile: g.donor.spec.profile };
			return;
		}
		if (!g.donor) return;
		if (g.original) g.out = { file: g.donor.file.data, image: null, profile: null };
		else if (g.donor.spec || g.noVoice) g.out = await updateFile(G(), g.donor, app, { codec: g.noVoice ? "keep" : "auto" });
	} catch (e) {
		g.error = message(e);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// connect: a radio running ODR with stub entry takes the cable route, any other one update mode

async function closeLink() {
	const g = session();
	try { await g.link?.s.close?.(); } catch (e) { /* gone */ }
	g.link = null;
}

// The radio's ODR port: the simulated one, one this page was allowed before, or the one the user picks
async function openLink() {
	const g = session();
	if (g.link) return g.link;
	let stream;
	if (simulated() && !simOdr()) throw new Error(t("flash.hid.connect.no_answer"));	// the practice radio is in update mode
	if (simOdr()) stream = simOdr().serialStream();
	else {
		const granted = await odrPorts();
		stream = granted.length ? await SerialStream.open(granted[0]) : await SerialStream.request();
	}
	return (g.link = new Link(stream));
}

const odrPorts = async () => (await navigator.serial.getPorts()).filter((p) => {
	const i = p.getInfo();
	return i.usbVendorId === ODR_USB.usbVendorId && i.usbProductId === ODR_USB.usbProductId;
});

// The stub's port after it enumerated again: the same USB IDs and serial number, so the browser lists it as granted
async function reopen() {
	if (simOdr()) return simOdr().serialStream();
	const ports = await odrPorts();
	if (!ports.length) throw new Error("not back yet");
	return SerialStream.open(ports[ports.length - 1]);
}

function connectView() {
	const g = session();
	const status = h("div");
	const noSerial = !simulated() && !serialSupported();
	const useUpdate = button(t("flash.hid.connect.update"), async () => {
		await closeLink();
		g.route = "update";
		await build();
		ctx.next();
	}, { quiet: true });
	const onward = () => S().forward([useUpdate, button(t("flash.nav.to_update_file"), ctx.next, { primary: true })]);
	const go = button(t("flash.hid.connect.go"), async () => {
		go.disabled = true;
		status.replaceChildren(h("p", { class: "muted" }, t("flash.hid.connect.checking")));
		g.route = null;
		try {
			const link = await openLink();
			const p = await probe(link);
			if (!p) throw new Error(t("flash.hid.connect.no_answer"));
			g.log.add(`radio    ${p.board} ${p.git}, codec ${p.codec}`);
			const board = k22Board(G().stub, p.board);
			if (board !== S().radio.flash.board) throw new Error(t("flash.hid.connect.other_radio", { board: p.board }));
			const rows = h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git));
			if (!(await Stub.odrOffer(link))) {
				await closeLink();
				g.route = "update";
				await build();
				status.replaceChildren(rows, notice(t("flash.hid.connect.no_cable"), "info"));
				S().forward(button(t("flash.nav.to_update_file"), ctx.next, { primary: true }));
				return;
			}
			g.route = "cable";
			await build();
			status.replaceChildren(rows, notice(t("flash.hid.connect.ready"), "ok"));
			onward();
		} catch (e) {
			await closeLink();
			status.replaceChildren(notice(ctx.errorText(e), "bad"), h("p", {}, t("flash.hid.connect.failed")));
			go.disabled = false;
			S().forward([useUpdate, go]);
		}
	}, { primary: true, disabled: noSerial });
	const box = h("div", { class: "flasher" }, panel(t("flash.hid.connect.title"),
		noSerial ? notice(t("flash.hid.connect.no_serial"), "info") : null,
		h("p", {}, t("flash.hid.connect.lead")),
		h("p", { class: "small muted" }, t("flash.hid.connect.original")),
		status, h("details", { class: "small" }, h("summary", {}, t("flash.hid.log")), g.log.el)));
	if (g.route === "cable" && g.link) {
		status.replaceChildren(notice(t("flash.hid.connect.ready"), "ok"));
		onward();
	} else S().forward([useUpdate, go]);
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// update_file: the user's own stock update file (for the cable route, only for the codec)

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
		const d = g.donor, via = cable();
		const versions = Object.values(G().specs).map((s) => s.stock?.name || s.profile).sort().join(", ");
		const picker = simulated() ? button(t("flash.sim.use_stock"), () => take({ name: "practice-update-file.sgl", data: S().sim.hidDonor }))
			: filePicker(t("flash.hid.file.pick"), ".sgl,.SGL", take);
		const rows = d ? [
			[t("flash.hid.file.name"), d.name],
			[t("flash.hid.file.radio"), d.file.label()],
			g.original ? null : [t("flash.hid.file.codec"), d.spec ? t("flash.hid.file.codec_found", { build: d.spec.stock?.name || d.spec.profile })
				: t("flash.hid.file.codec_unknown")],
			g.out?.image && !via ? [t("flash.hid.file.size"), t("flash.hid.file.size_of", { used: kb(g.out.image.length), room: kb(d.file.room) })] : null,
		].filter(Boolean) : [];
		const unknown = d && !d.spec && !g.original;
		wrap.replaceChildren(panel(t(g.original ? "flash.hid.file.title_original" : via ? "flash.hid.file.title_cable" : "flash.hid.file.title"),
			notice(t(via ? "flash.hid.cable.unverified" : "flash.hid.unverified"), "warn"),
			h("p", {}, t(g.original ? "flash.hid.file.lead_original" : via ? "flash.hid.file.lead_cable" : "flash.hid.file.lead")),
			g.original ? null : h("p", { class: "small muted" }, t("flash.hid.file.versions", { versions })),
			picker,
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			unknown && via ? notice(t("flash.hid.file.unsupported_cable", { versions }), "bad") : null,
			unknown && !via && !g.noVoice ? notice(t("flash.hid.file.unsupported", { versions }), "bad") : null,
			unknown && !via && g.noVoice ? notice(t("flash.hid.file.no_voice"), "info") : null,
			via && !d ? notice(t("flash.hid.file.keep"), "info") : null,
			g.error ? notice(g.error, "bad") : null));
		// the cable route: without a file (or with one the page can't take the codec from) the radio's codec stays
		const keep = via && d ? button(t("flash.hid.file.keep_button"), async () => {
			g.donor = null;
			await build();
			draw();
		}, { quiet: true }) : null;
		const noVoice = !via && unknown && !g.error ? button(t(g.noVoice ? "flash.hid.file.with_voice" : "flash.hid.file.without_voice"), async () => {
			g.noVoice = !g.noVoice;
			await build();
			draw();
		}, { quiet: true }) : null;
		S().forward([keep || noVoice, button(t("flash.nav.to_write"), ctx.next, { primary: true, disabled: !g.out })]);
	};
	// coming back after another build was chosen: the file is made again for it
	if (g.donor || cable()) build().then(draw);
	draw();
	return wrap;
}

// ---------------------------------------------------------------------------------------------------------------
// write over the cable: ODR hands over to the K22F stub, which writes the changed sectors and restarts the radio

let k22Json = null;
const stubJson = async () => (k22Json ||= await (await fetch(`${ROOT}data/k22-stub.json`)).json());

function cableView() {
	const g = session();
	const prog = progressBar();
	const status = h("div");
	prog.el.hidden = true;
	const fw = S().firmware;
	const what = g.out.profile ? t("flash.hid.write.what_voice", { build: g.donor.spec.stock?.name || g.out.profile }) : t("flash.hid.cable.what_keep");
	const go = button(t("flash.hid.cable.go"), () => run(), { primary: true });
	// back to the update file step, now for update mode (the update file the radio's loader needs)
	const viaUpdate = button(t("flash.hid.cable.use_update"), async () => {
		try { await stub?.s.close?.(); } catch (e) { /* gone */ }
		await closeLink();
		g.route = "update";
		await build();
		S().step = INSTALL.indexOf("update_file");
		ctx.render();
	}, { quiet: true });
	const box = h("div", { class: "flasher" }, panel(t("flash.hid.cable.title"),
		notice(t("flash.hid.cable.unverified"), "warn"),
		h("dl", { class: "kv-list" }, h("dt", {}, t("flash.hid.write.what")), h("dd", {}, what)),
		h("p", {}, h("b", {}, t("flash.hid.cable.lead"))),
		h("p", { class: "small muted" }, t("flash.hid.cable.safety")),
		prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.hid.log")), g.log.el)));
	S().forward(go);
	let stub = null;

	async function run() {
		go.disabled = true;
		status.replaceChildren();
		let started = null;
		try {
			await busy(true);
			started = ctx.flashStarted({ task: "install", source: fw?.origin || "", radio: S().radio.flash.board, firmware: String(fw?.manifest.version || "") });
			prog.el.hidden = false;
			if (!stub) {
				const link = await openLink();
				const json = await stubJson();
				stub = k22Stub(link.s, G().stub, S().radio.flash.board, { log: g.log.add, reopen });
				g.link = null;		// the port belongs to the loader from here
				await stub.enterFromOdr(f4Image(json), (d, n) => prog.set(d, n, t("flash.hid.cable.sending")), { entry: json.run });
			}
			prog.set(0, 1, t("flash.hid.cable.reading"));
			const r = await k22Install(stub, g.out.image, {
				// the sector the application ends in also holds the codec's first bytes: a copy first, written back unchanged
				onKept: async (sec) => {
					const file = `${S().radio.flash.board}_sector${sec.index}_${hex(sec.addr)}.bin`;
					if (!simulated()) download(file, sec.kept);
					g.log.add(t("flash.hid.cable.kept", { file }));
				},
				progress: (done, todo) => prog.set(done, todo, t("flash.hid.cable.writing", { done, todo })) });
			prog.set(1, 1, t("flash.hid.cable.verified"));
			g.result = t(r.written ? "flash.hid.cable.ok" : "flash.hid.cable.same", r);
			g.log.add(g.result);
			await stub.resetChip();
			try { await stub.s.close?.(); } catch (e) { /* gone */ }
			stub = null;
			ctx.flashFinished(started);
			status.replaceChildren(notice(g.result, "ok"));
			S().forward(button(t("flash.nav.to_done"), ctx.next, { primary: true }));
		} catch (e) {
			if (started) ctx.flashFinished(started, e);
			g.log.add(`error    ${e?.message || e}`);
			prog.el.hidden = true;
			go.disabled = false;
			if (!stub?.jumped) {
				// ODR did not hand over: nothing changed on the radio
				stub = null;
				await closeLink();
				status.replaceChildren(notice(t("flash.hid.cable.enter_failed", { error: ctx.errorText(e) }), "bad"));
				S().forward([viaUpdate, go]);
			} else if (!stub.info) {
				// the loader came up on a port this page can't open without asking: one click picks it
				const pick = button(t("flash.hid.cable.repick_button"), async () => {
					try {
						stub.s = await (simOdr() ? reopen() : SerialStream.request());
						await stub.helloWithin(2000, t("flash.hid.cable.lost"));
						stub.attached();
						await run();
					} catch (err) {
						status.replaceChildren(notice(t("flash.hid.cable.failed", { error: ctx.errorText(err) }), "bad"), h("p", {}, t("flash.hid.cable.after_fail")), ctx.helpLink());
						S().forward(viaUpdate);
					}
				}, { primary: true });
				status.replaceChildren(notice(t("flash.hid.cable.repick"), "action"));
				S().forward([viaUpdate, pick]);
			} else {
				const again = button(t("flash.hid.write.again"), async () => {
					again.disabled = true;
					if (await stub.attach()) return run();
					status.replaceChildren(notice(t("flash.hid.cable.failed", { error: t("flash.hid.cable.lost") }), "bad"), h("p", {}, t("flash.hid.cable.after_fail")), ctx.helpLink());
					S().forward(viaUpdate);
				}, { primary: true });
				status.replaceChildren(notice(t("flash.hid.cable.failed", { error: ctx.errorText(e) }), "bad"), h("p", {}, t("flash.hid.cable.after_fail")), ctx.helpLink());
				S().forward([viaUpdate, again]);
			}
		} finally {
			await busy(false);
		}
	}
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// write in update mode: the file through WebHID

function writeView() {
	if (cable()) return cableView();
	const g = session();
	const noHid = !simulated() && !hidSupported();
	const prog = progressBar();
	const status = h("div");
	prog.el.hidden = true;
	const what = g.original ? t("flash.hid.write.what_original", { name: g.donor.name })
		: g.out.profile ? t("flash.hid.write.what_voice", { build: g.donor.spec.stock?.name || g.out.profile })
		: t("flash.hid.write.what_no_voice");
	const go = button(t("flash.hid.write.go"), () => run(), { primary: true, disabled: noHid });
	const box = h("div", { class: "flasher" }, panel(t("flash.hid.write.title"),
		noHid ? notice(t("flash.hid.no_hid"), "bad") : null,
		notice(t("flash.hid.unverified"), "warn"),
		h("dl", { class: "kv-list" }, h("dt", {}, t("flash.hid.write.what")), h("dd", {}, what)),
		h("p", {}, t("flash.hid.write.lead")),
		h("ol", { class: "steps" }, S().radio.flash.bootloader.map((x) => h("li", {}, x)), h("li", {}, t("flash.hid.write.step_go"))),
		h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.hid.write.careful"))), h("p", {}, t("flash.hid.write.careful_text"))),
		prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.hid.log")), g.log.el)));
	S().forward(go);

	async function run() {
		go.disabled = true;
		status.replaceChildren(h("p", { class: "muted" }, t("flash.hid.write.choose")));
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
			status.replaceChildren(h("p", { class: "muted" }, t("flash.hid.write.running")));
			prog.el.hidden = false;
			let last = -1;
			await new Updater(link).flash(file, { progress: (done, total) => {
				const pct = Math.floor((100 * done) / total);
				if (pct === last) return;
				last = pct;
				prog.set(done, total, t("flash.hid.write.writing"));
				if (pct % 10 === 0) g.log.add(`write    ${pct}%`);
			} });
			ctx.flashFinished(started);
			status.replaceChildren(notice(t("flash.hid.write.done"), "ok"));
			S().forward(button(t("flash.nav.to_done"), ctx.next, { primary: true }));
		} catch (e) {
			if (started) ctx.flashFinished(started, e);
			const text = message(e);
			g.log.add(`error    ${e?.message || e}`);
			prog.el.hidden = true;
			status.replaceChildren(notice(text, "bad"), notice(t("flash.hid.write.retry_safe"), "info"), ctx.helpLink());
			go.disabled = false;
			S().forward(button(t("flash.hid.write.again"), () => run(), { primary: true }));
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
	const voice = g.out?.profile ? "flash.hid.done.check_voice" : g.out?.keep ? "flash.hid.done.check_voice_kept" : "flash.hid.done.no_voice";
	return h("div", { class: "flasher" }, panel(t(original ? "flash.hid.done.title_original" : "flash.hid.done.title"),
		cable() && g.result ? notice(g.result, "ok") : null,
		h("p", {}, t(original ? "flash.hid.done.lead_original" : "flash.hid.done.lead")),
		original ? null : h("ul", { class: "steps" }, h("li", {}, t("flash.hid.done.check_screen")), h("li", {}, t(voice))),
		h("p", { class: "small muted" }, t("flash.hid.done.no_link")),
		notice(t("flash.hid.done.report"), "info"),
		h("p", { class: "small" }, t(cable() ? "flash.hid.done.not_starting_cable" : "flash.hid.done.not_starting"))));
}

// ---------------------------------------------------------------------------------------------------------------
// the backend (backends.js)

const INSTALL = ["radio", "build", "connect", "update_file", "write", "done"];
const RECOVER = ["build", "update_file", "write", "done"];
const ORIGINAL = ["update_file", "write", "done"];
export const isHid = (radio, fb) => radio?.flash.backend === "sgl-hid" && !!fb.hid?.boards.includes(radio.flash.board);

register({
	id: "sgl-hid",
	// install only: the loader can't read the radio back, so there is no backup, and so nothing to restore
	available: (radio, fb, task) => task === "install" && isHid(radio, fb),
	steps: (task, { recovering }) => (task !== "install" ? null : recovering && ctx?.S.hid?.original ? ORIGINAL : recovering ? RECOVER : INSTALL),
	stepLabel: (step) => (step === "done" ? "hid_done" : step),
	views: { connect: connectView, update_file: fileView, write: writeView, done: doneView },
	// update mode is in the loader, which the page never writes: reinstalling, or putting the original update file back,
	// always works the same way
	recovery: { first: "fix", stock: "file", reach: "flash.help.hid_reach", stockText: "flash.help.fix.stock_text",
		choices: ["again"], againText: "flash.help.fix.hid_again_text", alias: { codec: "again", stock: "original" },
		extra: () => [{ id: "original", title: "flash.help.fix.hid_original", text: "flash.help.fix.hid_original_text",
			prepare: () => { session().original = true; } }] },
	ownImage: async (fb, board, data) => {
		if (startsWith(data, "SGL!")) throw new Error(t("flash.hid.err.sgl_as_image"));
		return ownImage(fb.hid, data);
	},
	release: async () => {
		if (!ctx?.S.hid) return;
		await closeLink();
		ctx.S.hid = null;
	},
});
