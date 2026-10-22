// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's steps for a DB25-D PRO that does not run ODR yet (db25d.js, docs/spec/hardware-db25d.md 7). The radio is
// otherwise handled by c7000ui.js (ODR's stub hand-over, the first stage's power-on window). Two things are added:
//
//   stock    the radio runs its original firmware: the universal stub goes into its RAM through the stock debug console
//            and starts there. From then on the radio is handled as any HR_C7000 radio with the stub running: the
//            backup, the install and a restore all go through the stub (c7000ui.js)
//   update   the vendor update mode (IAP) takes an install image (first stage, ODR and the codec slot) or a backup's
//            application area. The recovery route, for a radio that answers neither the console, ODR nor the first
//            stage's power-on window
//
// Neither ever writes the vendor loader below 0xC000, so update mode always remains.

import { h, t, notice, button, panel, progressBar } from "./dom.js";
import { sleep } from "./bytes.js";
import * as c7 from "./c7000.js";
import { CONSOLES, Usmart, Iap, trimTail, DB25DError, iapSuffix } from "./db25d.js";

const LISTEN_MS = 180000;	// how long the page waits for update mode
const WORD_RATE = 14;		// console frames per second, written or read back (radio, 2026-10-18: 3,470 in 242 s)

// host: what c7000ui.js hands over ({S, ctx, C, boardId, session, busy, fill, stubImage})
export function makeDb25d(host) {
	const { S, ctx, C, boardId, session, busy, fill, stubImage } = host;
	// the update-mode texts that describe the screen: the radio's own form (the GD-88 shows IAP_A or IAP_B)
	const own = (key) => `${key}${iapSuffix(S().radio.flash.via || boardId())}`;

	// ---- the original firmware: the stub through its console

	// show(...parts) / act(...buttons) are connect's. onLoaded() meets the stub the console just started; onUpdateMode()
	// and onAgain() are the other ways on
	async function stockFound({ show, act, onLoaded, onUpdateMode, onAgain }) {
		const c = session();
		// the simulated radio answers at once: no need to wait for the original firmware's log lines
		const con = new Usmart(c.stream, { log: (l) => c.log.add(l), quiet: S().fakeC7 ? 2 : 80, board: boardId() });
		const k = CONSOLES[boardId()] || CONSOLES.db25d;
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.db25d.stock.found"))),
			h("p", { class: "muted" }, t("flash.db25d.stock.checking")));
		const other = button(t("flash.db25d.none.update_mode"), onUpdateMode, { quiet: true });
		let build = null;
		try {
			build = await con.build();
		} catch (e) {
			show({ led: "off", screen: "on" }, notice(t("flash.db25d.stock.failed", { error: ctx.errorText(e) }), "bad"));
			return act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), other);
		}
		if (!build) {
			show({ led: "off", screen: "on" }, notice(t("flash.db25d.stock.unknown_build"), "bad"), h("p", {}, t(`flash.${k.texts}.stock.unknown_how`)));
			return act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), other);
		}
		const image = await stubImage();
		const words = Math.ceil(image.length / 4);
		const prog = progressBar();
		const status = h("div");
		prog.el.hidden = true;
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.db25d.stock.found"))),
			h("dl", { class: "kv-list" }, h("dt", {}, t("flash.db25d.stock.version")), h("dd", {}, `${k.prefix}${build}`)),
			h("p", {}, t("flash.db25d.stock.lead", { min: Math.ceil((2 * words) / WORD_RATE / 60) })),
			h("p", { class: "small muted" }, t("flash.db25d.stock.note")), prog.el, status);
		const ac = new AbortController();
		const go = button(t("flash.db25d.stock.go"), async () => {
			go.disabled = true;
			prog.el.hidden = false;
			fill(status);
			await busy(true);
			act(button(t("flash.db25d.stock.stop"), () => ac.abort(), { quiet: true }));
			const t0 = performance.now();
			let sent = 0;
			try {
				await con.loadStub(image, { signal: ac.signal, onProgress: (phase, d, n) => {
					if (phase === "start") return prog.set(d, n, t("flash.db25d.stock.starting"));
					sent = (phase === "write" ? 0 : n) + d;
					const rate = sent / Math.max(1, (performance.now() - t0) / 1000);
					const left = Math.max(1, Math.round((2 * n - sent) / Math.max(rate, 1) / 60));
					prog.set(sent, 2 * n, t(phase === "write" ? "flash.db25d.stock.writing" : "flash.db25d.stock.verifying", { min: left }));
				} });
				await busy(false);
				await onLoaded();
			} catch (e) {
				await busy(false);
				const stopped = e.message === "stopped";
				// a stop or a failure before the start leaves the original firmware running; one after it may not
				fill(status, notice(stopped ? t("flash.db25d.stock.stopped") : t("flash.db25d.stock.failed", { error: ctx.errorText(e) }), stopped ? "info" : "bad"),
					stopped ? null : h("p", {}, t("flash.db25d.stock.retry_how")));
				act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), other);
			}
		}, { primary: true });
		act(go, other);
	}

	// The console started the stub but it does not answer: the original firmware may have stopped, so the radio needs
	// a restart before anything else is tried
	function stubSilent({ show, act, onAgain, onUpdateMode }, error) {
		show({ led: "off", screen: "any", knob: "cycle" }, notice(t("flash.db25d.stock.no_stub", { error }), "bad"),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.db25d.stock.no_stub_off")), h("li", {}, t("flash.db25d.stock.no_stub_on"))));
		act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), button(t("flash.db25d.none.update_mode"), onUpdateMode, { quiet: true }));
	}

	// Nothing on the cable answered: the radio is off, in update mode already, or runs ODR with a silent link
	function nothing({ show, act, onUpdateMode, onPowerOn, onAgain }) {
		show({ led: "off", screen: "any" },
			notice(t("flash.db25d.none.lead"), "info"),
			h("ul", {}, h("li", {}, t("flash.db25d.none.on")), h("li", {}, t("flash.db25d.none.cable")), h("li", {}, t(own("flash.db25d.none.stock")))),
			h("p", { class: "small muted" }, t("flash.db25d.none.odr")));
		act(button(t("flash.db25d.none.again"), onAgain, { primary: true }), button(t("flash.db25d.none.update_mode"), onUpdateMode),
			button(t("flash.db25d.none.power_on"), onPowerOn, { quiet: true }));
	}

	// The loader answered a read-only probe: the radio is in update mode already (its application is missing or incomplete,
	// so it stays there at every power-on, white screen and red LED). The install goes straight on; no keys are needed.
	function alreadyUpdateMode({ show, act, onUpdateMode, onAgain }, version) {
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.db25d.um.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.db25d.stock.version")), h("dd", {}, version)) : null,
			h("p", {}, t(own("flash.db25d.um.text"))), h("p", { class: "small muted" }, t("flash.db25d.um.safe")));
		act(onUpdateMode ? button(t("flash.db25d.um.go"), onUpdateMode, { primary: true }) : button(t("flash.db25d.none.again"), onAgain, { primary: true }));
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
			done: t("flash.db25d.iap.ok"), onDone: () => ctx.next() });
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
			done: t("flash.db25d.iap.restored"), onDone });
	}

	function iapPanel({ title, rows, image, restore, report, done, onDone }) {
		const c = session();
		const prog = progressBar();
		const status = h("div");
		const steps = S().radio.flash.bootloader || [];
		const go = button(t(restore ? "flash.db25d.iap.restore_go" : "flash.db25d.iap.go"), () => run(), { primary: true });
		const box = panel(title,
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			h("p", {}, t(restore ? "flash.db25d.iap.restore_lead" : "flash.db25d.iap.lead")),
			h("ol", { class: "steps c7-big" }, h("li", {}, t(restore ? "flash.db25d.iap.restore_step_go" : "flash.db25d.iap.step_go")), ...steps.map((x) => h("li", {}, x))),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t(own("flash.db25d.iap.careful_text")))),
			prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
		prog.el.hidden = true;
		S().forward(go);

		async function run() {
			go.disabled = true;
			fill(status, h("p", { class: "muted c7-big" }, t("flash.db25d.iap.listening")));
			prog.el.hidden = true;
			const stop = new AbortController();
			S().forward(button(t("flash.c7.connect.cancel"), () => stop.abort(), { quiet: true }));
			const started = ctx.flashStarted(report);
			try {
				const iap = new Iap(c.stream, { log: (l) => c.log.add(l) });
				// a loader that answered the probe is waiting already: its banner was printed before the page asked
				if (c.iapReady) c.iapReady = false;
				else await iap.waitBanner(LISTEN_MS, { signal: stop.signal });
				await busy(true);
				fill(status, h("p", { class: "muted c7-big" }, t("flash.db25d.iap.erasing")));
				prog.el.hidden = false;
				await iap.write(image, { signal: stop.signal, restore,
					onPhase: (p) => fill(status, h("p", { class: "muted c7-big" }, t(p === "erase" ? "flash.db25d.iap.erasing" : "flash.db25d.iap.writing"))),
					onPage: (i, n) => prog.set(i, n, t("flash.db25d.iap.page", { done: i, total: n })) });
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

	return { stockFound, stubSilent, nothing, alreadyUpdateMode, iapWriteView, iapRestoreView };
}

export { DB25DError };
