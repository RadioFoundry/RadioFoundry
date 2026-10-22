// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's stock-side steps for an HR_C7000 radio that does not run ODR yet, as its radios.json flash data names
// them (the DB25-D PRO's: docs/spec/hardware-db25d.md 7). The radio is otherwise handled by c7000ui.js (ODR's stub
// hand-over, the first stage's power-on window). Two things are added:
//
//   stock    flash.stock_console: the radio runs its original firmware, and the universal stub goes into its RAM
//            through the stock debug console (usmart.js) and starts there. From then on the radio is handled as any
//            HR_C7000 radio with the stub running: the backup, the install and a restore all go through the stub
//   update   flash.update_mode "iap": the vendor update mode (iap.js) takes an install image (first stage, ODR and the
//            codec slot) or a backup's application area. The recovery route, for a radio that answers neither the
//            console, ODR nor the first stage's power-on window
//
// Neither ever writes the vendor loader, so update mode always remains.

import { h, t, notice, button, panel, progressBar } from "./dom.js";
import { sleep } from "./bytes.js";
import * as c7 from "./c7000.js";
import { Usmart, stockName } from "./usmart.js";
import { trimTail } from "./iap.js";

const LISTEN_MS = 180000;	// how long the page waits for update mode
const WORD_RATE = 14;		// console frames per second, written or read back (radio, 2026-10-18: 3,470 in 242 s)

// host: what c7000ui.js hands over ({S, ctx, C, boardId, session, busy, iap, fill, stubImage}); iap(session) makes the
// update mode's protocol object
export function makeStockRoutes(host) {
	const { S, ctx, C, boardId, session, busy, iap, fill, stubImage } = host;
	// a text in the radio's own words where en.json has one (<key>_<radio id>, what its update mode shows), else the
	// common one. A second SoC takes its own board's text first, then its radio's
	const own = (key) => {
		const r = S().radio;
		for (const id of [r.id, r.second_of]) {
			const k = `${key}_${id}`;
			if (id && t(k) !== k) return k;
		}
		return key;
	};

	// ---- the original firmware: the stub through its console

	// show(...parts) / act(...buttons) are connect's. onLoaded() meets the stub the console just started; onUpdateMode()
	// and onAgain() are the other ways on
	async function stockFound({ show, act, onLoaded, onUpdateMode, onAgain }) {
		const c = session();
		// the simulated radio answers at once: no need to wait for the original firmware's log lines
		const con = new Usmart(c.stream, { log: (l) => c.log.add(l), quiet: S().fakeC7 ? 2 : 80, versions: S().radio.flash.stock_versions });
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.usmart.found"))),
			h("p", { class: "muted" }, t("flash.usmart.checking")));
		const other = onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true });
		let build = null;
		try {
			build = await con.build();
		} catch (e) {
			show({ led: "off", screen: "on" }, notice(t("flash.usmart.failed", { error: ctx.errorText(e) }), "bad"));
			return act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
		}
		if (!build) {
			const versions = S().radio.flash.stock_versions.join(" and ");
			show({ led: "off", screen: "on" }, notice(t("flash.usmart.unknown_build"), "bad"), h("p", {}, t("flash.usmart.unknown_how", { versions })));
			return act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
		}
		const image = await stubImage();
		const words = Math.ceil(image.length / 4);
		const prog = progressBar();
		const status = h("div");
		prog.el.hidden = true;
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.usmart.found"))),
			h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, stockName(S().radio.flash, build))),
			h("p", {}, t("flash.usmart.lead", { min: Math.ceil((2 * words) / WORD_RATE / 60) })),
			h("p", { class: "small muted" }, t("flash.usmart.note")), prog.el, status);
		const ac = new AbortController();
		const go = button(t("flash.usmart.go"), async () => {
			go.disabled = true;
			prog.el.hidden = false;
			fill(status);
			await busy(true);
			act(button(t("flash.usmart.stop"), () => ac.abort(), { quiet: true }));
			const t0 = performance.now();
			let sent = 0;
			try {
				await con.loadStub(image, { signal: ac.signal, onProgress: (phase, d, n) => {
					if (phase === "start") return prog.set(d, n, t("flash.usmart.starting"));
					sent = (phase === "write" ? 0 : n) + d;
					const rate = sent / Math.max(1, (performance.now() - t0) / 1000);
					const left = Math.max(1, Math.round((2 * n - sent) / Math.max(rate, 1) / 60));
					prog.set(sent, 2 * n, t(phase === "write" ? "flash.usmart.writing" : "flash.usmart.verifying", { min: left }));
				} });
				await busy(false);
				await onLoaded();
			} catch (e) {
				await busy(false);
				const stopped = e.message === "stopped";
				// a stop or a failure before the start leaves the original firmware running; one after it may not
				fill(status, notice(stopped ? t("flash.usmart.stopped") : t("flash.usmart.failed", { error: ctx.errorText(e) }), stopped ? "info" : "bad"),
					stopped ? null : h("p", {}, t("flash.usmart.retry_how")));
				act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
			}
		}, { primary: true });
		act(go, other);
	}

	// The console started the stub but it does not answer: the original firmware may have stopped, so the radio needs
	// a restart before anything else is tried
	function stubSilent({ show, act, onAgain, onUpdateMode }, error) {
		show({ led: "off", screen: "any", knob: "cycle" }, notice(t("flash.usmart.no_stub", { error }), "bad"),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.usmart.no_stub_off")), h("li", {}, t("flash.usmart.no_stub_on"))));
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true }));
	}

	// Nothing on the cable answered: the radio is off, in update mode already, or runs ODR with a silent link
	function nothing({ show, act, onUpdateMode, onPowerOn, onAgain }) {
		show({ led: "off", screen: "any" },
			notice(t("flash.iap.none.lead"), "info"),
			h("ul", {}, h("li", {}, t("flash.iap.none.on")), h("li", {}, t("flash.iap.none.cable")), h("li", {}, t(own("flash.iap.none.stock")))),
			h("p", { class: "small muted" }, t("flash.iap.none.odr")));
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode),
			button(t("flash.iap.none.power_on"), onPowerOn, { quiet: true }));
	}

	// The loader answered a read-only probe: the radio is in update mode already (its application is missing or incomplete,
	// so it stays there at every power-on, white screen and red LED). The install goes straight on; no keys are needed.
	function alreadyUpdateMode({ show, act, onUpdateMode, onAgain }, version) {
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.iap.um.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)) : null,
			h("p", {}, t(own("flash.iap.um.text"))), h("p", { class: "small muted" }, t("flash.iap.um.safe")));
		act(onUpdateMode ? button(t("flash.iap.um.go"), onUpdateMode, { primary: true }) : button(t("flash.iap.none.again"), onAgain, { primary: true }));
	}

	// ---- the vendor update mode: install, or put a backup back

	// The install task's write step when the radio is reached through update mode
	function iapWriteView() {
		const fw = S().firmware;
		const d = session().decision;
		let image;
		try {
			const [, data] = c7.target(C(), boardId(), fw.image, d.mode === "write" ? d.slot : null);
			image = trimTail(data);
		} catch (e) {
			return panel(t("flash.write.title"), notice(ctx.errorText(e), "bad"));
		}
		const what = d.mode === "keep" ? t("flash.c7.codec.keep") : d.mode === "none" ? t("flash.c7.codec.none")
			: t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
		return iapPanel({ title: t("flash.c7.write.title"), image, restore: false,
			rows: [[t("flash.c7.write.firmware"), `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`], [t("flash.c7.write.codec"), what]],
			report: { task: "install", source: fw.origin || "", radio: boardId(), firmware: String(fw.manifest.version || ""),
				variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null },
			done: t("flash.iap.ok"), onDone: () => ctx.next() });
	}

	// The restore task through update mode: the backup's application area and codec slot, exactly (c7.restoreTarget)
	function iapRestoreView(progmem, onDone) {
		let image;
		try {
			image = c7.restoreTarget(C(), progmem)[1];
		} catch (e) {
			return panel(t("flash.c7.restore.title"), notice(ctx.errorText(e), "bad"));
		}
		return iapPanel({ title: t("flash.c7.restore.title"), image, restore: true, rows: [],
			report: { task: "restore", source: "c7000-progmem", radio: boardId(), firmware: S().bundle?.meta.firmware_now?.kind || "" },
			done: t("flash.iap.restored"), onDone });
	}

	function iapPanel({ title, rows, image, restore, report, done, onDone }) {
		const c = session();
		const prog = progressBar();
		const status = h("div");
		const steps = S().radio.flash.bootloader || [];
		const go = button(t(restore ? "flash.iap.restore_go" : "flash.iap.go"), () => run(), { primary: true });
		const box = panel(title,
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			h("p", {}, t(restore ? "flash.iap.restore_lead" : "flash.iap.lead")),
			h("ol", { class: "steps c7-big" }, h("li", {}, t(restore ? "flash.iap.restore_step_go" : "flash.iap.step_go")), ...steps.map((x) => h("li", {}, x))),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t(own("flash.iap.careful_text")))),
			prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
		prog.el.hidden = true;
		S().forward(go);

		async function run() {
			go.disabled = true;
			fill(status, h("p", { class: "muted c7-big" }, t("flash.iap.listening")));
			prog.el.hidden = true;
			const stop = new AbortController();
			S().forward(button(t("flash.c7.connect.cancel"), () => stop.abort(), { quiet: true }));
			const started = ctx.flashStarted(report);
			try {
				const loader = iap(c);
				// a loader that answered the probe is waiting already: its banner was printed before the page asked
				if (c.iapReady) c.iapReady = false;
				else await loader.waitBanner(LISTEN_MS, { signal: stop.signal });
				await busy(true);
				fill(status, h("p", { class: "muted c7-big" }, t("flash.iap.erasing")));
				prog.el.hidden = false;
				await loader.write(image, { signal: stop.signal, restore,
					onPhase: (p) => fill(status, h("p", { class: "muted c7-big" }, t(p === "erase" ? "flash.iap.erasing" : "flash.iap.writing"))),
					onPage: (i, n) => prog.set(i, n, t("flash.iap.page", { done: i, total: n })) });
				prog.set(1, 1, t("flash.c7.write.verified"));
				ctx.flashFinished(started);
				c.resetAt = Date.now();
				await busy(false);
				fill(status, notice(done, "ok"));
				await sleep(800);
				onDone();
			} catch (e) {
				ctx.flashFinished(started, e);
				await busy(false);
				const stopped = e.message === "stopped";
				fill(status, notice(stopped ? t("flash.iap.cancelled") : t("flash.iap.failed", { error: ctx.errorText(e) }), stopped ? "info" : "bad"),
					stopped ? null : h("div", { class: "notice info" }, h("p", {}, t("flash.iap.retry_safe")), h("p", { class: "small" }, t("flash.iap.retry_how"))),
					stopped ? null : ctx.helpLink());
				prog.el.hidden = true;
				go.disabled = false;
				S().forward(button(t("flash.c7.write.again"), () => run(), { primary: true }));
			}
		}
		return box;
	}

	return { stockFound, stubSilent, nothing, alreadyUpdateMode, iapWriteView, iapRestoreView };
}

