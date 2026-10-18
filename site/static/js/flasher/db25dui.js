// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's steps for a DB25-D PRO that does not run ODR yet (db25d.js, docs/spec/hardware-db25d.md 7). The radio is
// otherwise handled by c7000ui.js (ODR's stub hand-over, the first stage's power-on window). Two things are added:
//
//   read     the radio runs its stock firmware: the whole program flash is read through the stock debug console, kept
//            in this browser as it goes (a stopped read resumes) and saved as a backup file; the codec is found in it
//   install  the vendor update mode takes the install image: first stage, ODR and the codec slot
//
// The stock firmware offers no other way to read the flash, and update mode is the only way to write it. After the
// install ODR runs, the first stage is in place, and every later update goes through the stub (c7000ui.js).

import { h, t, notice, button, panel, progressBar } from "./dom.js";
import { le32, u32le, concat, sleep } from "./bytes.js";
import { putBlob, getBlob } from "./codecsave.js";
import * as c7 from "./c7000.js";
import { Usmart, usmartDump, Iap, trimTail, FLASH_SIZE, DB25DError } from "./db25d.js";

const LISTEN_MS = 180000;	// how long the page waits for update mode

// host: what c7000ui.js hands over ({S, ctx, C, boardId, session, busy, saveBundle, store, fill, retryHelp})
export function makeDb25d(host) {
	const { S, ctx, C, boardId, session, busy, saveBundle, store, fill } = host;
	const key = () => store("usmart");

	// A stopped read: [done words (u32), limit (u32), image]
	async function loadState() {
		const b = await getBlob(key());
		if (!b || b.length < 8 + 0x100000) return null;
		return { done: u32le(b, 0), limit: u32le(b, 4), image: b.subarray(8) };
	}

	// ---- the stock firmware: read the program flash

	// show(...parts) / act(...buttons) are connect's; onReady() goes on to the next step; instead() is the other way in
	async function stockFound({ show, act, onReady, instead }) {
		const c = session();
		const state = await loadState();
		let limit = state?.limit || 0x100000;
		const prog = progressBar();
		const status = h("div");
		const choice = (bytes, title, text) => h("label", { class: "toggle" },
			h("input", { type: "radio", name: "db25d-read", checked: bytes === limit, disabled: !!state, onchange: () => { limit = bytes; } }),
			h("span", {}, h("b", {}, title), h("span", { class: "why" }, text)), h("span"));
		const body = [
			h("p", { class: "c7-big" }, h("b", {}, t("flash.db25d.stock.found"))),
			h("p", {}, t("flash.db25d.stock.lead")),
			choice(0x100000, t("flash.db25d.stock.opt_1m"), t("flash.db25d.stock.opt_1m_text")),
			choice(FLASH_SIZE, t("flash.db25d.stock.opt_2m"), t("flash.db25d.stock.opt_2m_text")),
			state ? notice(t("flash.db25d.stock.resume", { pct: Math.round((100 * state.done) / (state.limit / 4)) }), "info") : null,
			h("p", { class: "small muted" }, t("flash.db25d.stock.note")), prog.el, status];
		prog.el.hidden = true;
		show({ led: "off", screen: "on" }, ...body);
		const ac = new AbortController();
		const go = button(state ? t("flash.db25d.stock.resume_go") : t("flash.db25d.stock.go"), async () => {
			go.disabled = true;
			prog.el.hidden = false;
			fill(status);
			await busy(true);
			act(button(t("flash.db25d.stock.stop"), () => ac.abort(), { quiet: true }));
			let queue = Promise.resolve();
			const t0 = performance.now();
			const w0 = state?.done || 0;
			try {
				const con = new Usmart(c.stream, { log: (l) => c.log.add(l) });
				const image = await usmartDump(con, { limit, state: state && { done: state.done, image: state.image }, signal: ac.signal,
					log: (l) => c.log.add(l),
					onProgress: (d, n) => {
						const rate = (d - w0) / Math.max(1, (performance.now() - t0) / 1000);
						const left = Math.round((n - d) / Math.max(rate, 1) / 60);
						prog.set(d, n, t("flash.db25d.stock.progress", { pct: Math.round((100 * d) / n), min: left }));
					},
					onCheckpoint: (done, img) => { queue = queue.then(() => putBlob(key(), concat(le32(done), le32(limit), img))).catch(() => {}); } });
				await queue;
				c.progmem = image;
				c.slot = image.subarray(C().boards[boardId()].slot[0], C().boards[boardId()].slot[1]);
				c.analysis = await c7.analyze(image, C(), boardId());
				c.how = "stock";
				const slotInfo = await c7.parseSlot(C(), boardId(), c.slot);
				if (slotInfo) c.slotBytes = c.slot.slice(0, slotInfo.length);
				if (c.analysis.codec) await putBlob(store("slot"), c7.slotBytes(C(), boardId(), c.analysis.codec));
				await putBlob(store("progmem-before-odr"), image);
				await putBlob(key(), new Uint8Array(0));	// the read is complete: nothing to resume
				await saveBundle();
				await busy(false);
				onReady();
			} catch (e) {
				await queue;
				await busy(false);
				fill(status, notice(e.message === "stopped" ? t("flash.db25d.stock.stopped") : t("flash.db25d.stock.failed", { error: ctx.errorText(e) }), e.message === "stopped" ? "info" : "bad"));
				go.disabled = false;
				go.textContent = t("flash.db25d.stock.resume_go");
				act(go);
			}
		}, { primary: true });
		act(go);
		if (instead) c.insteadAction = instead;
	}

	// Nothing on the cable answered: the radio is off, in update mode already, or runs ODR with a silent link
	function nothing({ show, act, onUpdateMode, onPowerOn, onAgain }) {
		show({ led: "off", screen: "any" },
			notice(t("flash.db25d.none.lead"), "info"),
			h("ul", {}, h("li", {}, t("flash.db25d.none.on")), h("li", {}, t("flash.db25d.none.cable")), h("li", {}, t("flash.db25d.none.stock"))),
			h("p", { class: "small muted" }, t("flash.db25d.none.odr")));
		act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), button(t("flash.db25d.none.update_mode"), onUpdateMode),
			button(t("flash.db25d.none.power_on"), onPowerOn, { quiet: true }));
	}

	// ---- the install through the vendor update mode

	function iapWriteView() {
		const c = session();
		const fw = S().firmware;
		const d = c.decision;
		const prog = progressBar();
		const status = h("div");
		let image;
		try {
			const [, data] = c7.target(C(), boardId(), fw.image, d.mode === "write" ? d.slot : null);
			image = trimTail(data);
		} catch (e) {
			return panel(t("flash.write.title"), notice(ctx.errorText(e), "bad"));
		}
		const what = d.mode === "keep" ? t("flash.c7.codec.keep") : d.mode === "none" ? t("flash.c7.codec.none")
			: t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
		const steps = S().radio.flash.bootloader || [];
		const ac = { current: null };
		const go = button(t("flash.db25d.iap.go"), () => run(), { primary: true });
		const box = panel(t("flash.c7.write.title"),
			h("dl", { class: "kv-list" }, h("dt", {}, t("flash.c7.write.firmware")), h("dd", {}, `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`),
				h("dt", {}, t("flash.c7.write.codec")), h("dd", {}, what)),
			h("p", {}, t("flash.db25d.iap.lead")),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.db25d.iap.step_go")), ...steps.map((s) => h("li", {}, s))),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t("flash.db25d.iap.careful_text"))),
			prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
		prog.el.hidden = true;
		S().forward(go);

		async function run() {
			go.disabled = true;
			fill(status, h("p", { class: "muted c7-big" }, t("flash.db25d.iap.listening")));
			prog.el.hidden = true;
			const stop = new AbortController();
			ac.current = stop;
			S().forward(button(t("flash.c7.connect.cancel"), () => stop.abort(), { quiet: true }));
			const started = ctx.flashStarted({ task: "install", source: fw.origin || "", radio: boardId(), firmware: String(fw.manifest.version || ""),
				variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null });
			try {
				const iap = new Iap(c.stream, { log: (l) => c.log.add(l) });
				await iap.waitBanner(LISTEN_MS, { signal: stop.signal });
				await busy(true);
				fill(status, h("p", { class: "muted c7-big" }, t("flash.db25d.iap.erasing")));
				prog.el.hidden = false;
				await iap.write(image, { signal: stop.signal,
					onPhase: (p) => fill(status, h("p", { class: "muted c7-big" }, t(p === "erase" ? "flash.db25d.iap.erasing" : "flash.db25d.iap.writing"))),
					onPage: (i, n) => prog.set(i, n, t("flash.db25d.iap.page", { done: i, total: n })) });
				prog.set(1, 1, t("flash.c7.write.verified"));
				ctx.flashFinished(started);
				c.resetAt = Date.now();
				await busy(false);
				fill(status, notice(t("flash.db25d.iap.ok"), "ok"));
				await sleep(800);
				ctx.next();
			} catch (e) {
				ctx.flashFinished(started, e);
				await busy(false);
				const stopped = e.message === "stopped";
				fill(status, notice(stopped ? t("flash.db25d.iap.cancelled") : t("flash.db25d.iap.failed", { error: ctx.errorText(e) }), stopped ? "info" : "bad"),
					stopped ? null : h("div", { class: "notice info" }, h("p", {}, t("flash.db25d.iap.retry_safe")), h("p", { class: "small" }, t("flash.db25d.iap.retry_how"))),
					stopped ? null : ctx.helpLink());
				prog.el.hidden = true;
				go.disabled = false;
				S().forward(button(t("flash.c7.write.again"), () => run(), { primary: true }));
			}
		}
		return box;
	}

	return { stockFound, nothing, iapWriteView, loadState };
}

export { DB25DError };
