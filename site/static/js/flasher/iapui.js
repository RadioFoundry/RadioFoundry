// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flasher's stock-side steps for an HR_C7000 radio that does not run RadioFoundry yet, as its radios.json flash data names
// them (the DB25-D PRO's: docs/spec/hardware-db25d.md 7). The radio is otherwise handled by c7000ui.js (RadioFoundry's stub
// hand-over, the first stage's power-on window). Two things are added:
//
//   stock    flash.stock_console: the radio runs its original firmware, and the universal stub goes into its RAM
//            through the stock debug console (usmart.js) and starts there. From then on the radio is handled as any
//            HR_C7000 radio with the stub running: the backup, the install and a restore all go through the stub
//   update   flash.update_mode: the radio's own factory update mode, entered at power-on, takes an install image
//            (RadioFoundry and the codec slot; through "iap", iap.js, the first stage too) or a backup's application
//            area. "baofeng" (bfloader.js) is the Baofeng updater, which erases the whole application area first. The
//            recovery route, for a radio that answers neither the console, RadioFoundry, the first stage's power-on
//            window nor the boot ROM. Nothing is read from the radio this way, so the codec comes from a file
//
// Neither ever writes the vendor loader, so update mode always remains.

import { h, t, notice, button, panel, progressBar } from "./dom.js";
import { sleep } from "./bytes.js";
import * as c7 from "./c7000.js";
import { Usmart, stockName } from "./usmart.js";
import { trimTail } from "./iap.js";
import { CodecError } from "./c7000.js";
import { errorNotice, reportLine } from "../report.js";
import { radioPicture, holdKeys } from "./radiotop.js";

const LISTEN_MS = 180000;	// how long the page waits for update mode

// What each protocol's write step says: an IAP loader checks the whole image by its checksum and starts it; the
// Baofeng updater acknowledges each block by its CRC and may wait for a power cycle (hardware-dm1801pro.md 6.2)
const TEXTS = {
	iap: { careful: "flash.iap.careful_text", ok: "flash.iap.ok", restored: "flash.iap.restored", written: "flash.c7.write.verified" },
	baofeng: { careful: "flash.factory.careful_restart", ok: "flash.factory.ok", restored: "flash.factory.restored", written: "flash.factory.written" },
};
// a loader that erases the whole application area before it writes: a restore needs only what is not erased already
const ERASES_ALL = new Set(["baofeng"]);

// The bytes the factory update mode writes for an install: the image, 0xFF up to the slot, the slot (c7.target), without
// the erased tail. Nothing was read from the radio, so no slot in it is known to be good, and the Baofeng updater erases
// the slot anyway: a decision to keep it is refused
export function factoryInstallImage(c, boardId, image, decision) {
	if (!decision || decision.mode === "keep")
		throw new CodecError("the codec slot can't be kept through the factory update mode: select a file to take the codec from", "internal");
	return trimTail(c7.target(c, boardId, image, decision.mode === "write" ? decision.slot : null)[1]);
}
const WORD_RATE = 14;		// console frames per second, written or read back (radio, 2026-10-18: 3,470 in 242 s)

// host: what c7000ui.js hands over ({S, ctx, C, boardId, session, busy, loader, fill, stubImage}); loader(session) makes
// the update mode's protocol object (listen, write)
export function makeStockRoutes(host) {
	const { S, ctx, C, boardId, session, busy, loader: makeLoader, fill, stubImage, cableBanner = () => null } = host;
	const protocol = () => S().radio.flash.update_mode;

	// update mode steps beside the radio's picture, the keys to hold marked on it (radiotop.js; app.js does the same)
	const withPicture = (steps) => {
		const pic = radioPicture(S().radio, { keys: holdKeys(S().radio) });
		return pic ? h("div", { class: "radio-grid" }, steps, pic) : steps;
	};

	// ---- the original firmware: the stub through its console

	// show(...parts) / act(...buttons) are connect's. onLoaded() meets the stub the console just started; onUpdateMode()
	// and onAgain() are the other ways on
	async function stockFound({ show, act, onLoaded, onUpdateMode, onAgain }) {
		const c = session();
		// the simulated radio answers at once: no need to wait for the original firmware's log lines
		const con = new Usmart(c.stream, { log: (l) => c.log.add(l), quiet: S().fakeC7 ? 2 : 80 });
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.usmart.found"))),
			h("p", { class: "muted" }, t("flash.usmart.checking")));
		const other = onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true });
		let build = null;
		try {
			build = await con.build();
		} catch (e) {
			show({ led: "off", screen: "on" }, errorNotice(t("flash.usmart.failed", { error: ctx.errorText(e) })));
			return act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
		}
		if (!build) {
			const versions = S().radio.flash.stock_versions.join(" and ");
			show({ led: "off", screen: "on" }, notice(t("flash.usmart.unknown_build"), "bad"), h("p", {}, t("flash.usmart.unknown_how", { versions })), reportLine());
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
				fill(status, (stopped ? notice(t("flash.usmart.stopped"), "info") : errorNotice(t("flash.usmart.failed", { error: ctx.errorText(e) }))),
					stopped ? null : h("p", {}, t("flash.usmart.retry_how")));
				act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
			}
		}, { primary: true });
		act(go, other);
	}

	// The console started the stub but it does not answer: the original firmware may have stopped, so the radio needs
	// a restart before anything else is tried
	function stubSilent({ show, act, onAgain, onUpdateMode }, error) {
		show({ led: "off", screen: "any", knob: "cycle" }, errorNotice(t("flash.usmart.no_stub", { error })),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.usmart.no_stub_off")), h("li", {}, t("flash.usmart.no_stub_on"))));
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true }));
	}

	// Nothing on the cable answered: the radio is off, in update mode already, or runs RadioFoundry with a silent link
	function nothing({ show, act, onUpdateMode, onPowerOn, onAgain }) {
		show({ led: "off", screen: "any" },
			notice(t("flash.iap.none.lead"), "info"),
			h("ul", {}, h("li", {}, t("flash.iap.none.on")), h("li", {}, t("flash.iap.none.cable")), h("li", {}, t("flash.iap.none.stock"))),
			h("p", { class: "small muted" }, t("flash.iap.none.rfoundry")));
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode),
			button(t("flash.iap.none.power_on"), onPowerOn, { quiet: true }));
	}

	// The loader answered a read-only probe: the radio is in update mode already (its application is missing or incomplete,
	// so it stays there at every power-on, white screen and red LED). The install goes straight on; no keys are needed.
	function alreadyUpdateMode({ show, act, onUpdateMode, onAgain }, version) {
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.iap.um.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)) : null,
			h("p", {}, t("flash.iap.um.text")), h("p", { class: "small muted" }, t("flash.iap.um.safe")));
		act(onUpdateMode ? button(t("flash.iap.um.go"), onUpdateMode, { primary: true }) : button(t("flash.iap.none.again"), onAgain, { primary: true }));
	}

	// ---- the vendor update mode: install, or put a backup back

	// The install task's write step when the radio is reached through update mode
	function iapWriteView() {
		const fw = S().firmware;
		const d = session().decision;
		let image;
		try {
			image = factoryInstallImage(C(), boardId(), fw.image, d);
		} catch (e) {
			return panel(t("flash.write.title"), errorNotice(ctx.errorText(e)));
		}
		const what = d.mode === "keep" ? t("flash.c7.codec.keep") : d.mode === "none" ? t("flash.c7.codec.none")
			: t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
		return iapPanel({ title: t("flash.c7.write.title"), image, restore: false,
			rows: [[t("flash.c7.write.firmware"), `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`], [t("flash.c7.write.codec"), what]],
			report: { task: "install", source: fw.origin || "", radio: boardId(), firmware: String(fw.manifest.version || ""),
				variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null },
			done: t(TEXTS[protocol()].ok), name: `rfoundry_${boardId()}.bin`, onDone: () => ctx.next() });
	}

	// The restore task through update mode: the backup's application area and codec slot, exactly (c7.restoreTarget).
	// vendor: the name of the original firmware file the program memory came from (c7000ui.js vendorBundle), or null
	function iapRestoreView(progmem, vendor, onDone) {
		let image;
		try {
			// the loader's ID tells the radio at the cable; a backup names the radio it came from by its bootloader's
			const model = c7.bootloaderModel(C(), progmem);
			if (model && model !== boardId()) throw new CodecError("this backup is of another radio model", "internal");
			image = c7.restoreTarget(C(), progmem)[1];
			if (ERASES_ALL.has(protocol())) image = trimTail(image);
		} catch (e) {
			return panel(t("flash.c7.restore.title"), errorNotice(ctx.errorText(e)));
		}
		return iapPanel({ title: t("flash.c7.restore.title"), image, restore: true, rows: [],
			lead: vendor ? t("flash.iap.restore_lead_vendor", { file: vendor }) : null,
			report: { task: "restore", source: vendor ? "stock" : "c7000-progmem", radio: boardId(), firmware: S().bundle?.meta.firmware_now?.kind || "" },
			done: t(TEXTS[protocol()].restored), name: vendor || "restore.bin", onDone });
	}

	function iapPanel({ title, rows, image, restore, report, done, onDone, name, lead = null }) {
		const c = session();
		const prog = progressBar();
		const status = h("div");
		const steps = S().radio.flash.bootloader || [];
		const go = button(t(restore ? "flash.iap.restore_go" : "flash.iap.go"), () => run(), { primary: true });
		const box = panel(title, cableBanner(),	// a CH340 cable: the warning here too (programming through update mode)
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			h("p", {}, lead || t(restore ? "flash.iap.restore_lead" : "flash.iap.lead")),
			withPicture(h("ol", { class: "steps c7-big" }, h("li", {}, t(restore ? "flash.iap.restore_step_go" : "flash.iap.step_go")), ...steps.map((x) => h("li", {}, x)))),
			simKeys(),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t(TEXTS[protocol()].careful))),
			prog.el, status, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
		prog.el.hidden = true;
		S().forward(go);

		async function run() {
			go.disabled = true;
			fill(status, h("p", { class: "muted c7-big" }, t("flash.iap.listening")));
			prog.el.hidden = true;
			const stop = new AbortController();
			S().forward(button(t("flash.c7.connect.cancel"), () => stop.abort(), { quiet: true }));
			// The user left this step (Back, Start over) while the page listens: stop listening, or a radio switched on
			// with its keys later, for anything else, would be erased by a step no longer on the page. Nothing is drawn
			// then. A write that started runs to its end
			const left = () => S().c7 !== c || !box.isConnected;
			let writing = false;
			const watch = setInterval(() => { if (!writing && left()) stop.abort(); }, 250);
			const started = ctx.flashStarted(report);
			try {
				// A loader that identified itself (bfloader.js ready) answers that once per power-on: the session keeps it
				// for the next try. After a write started, a try needs the radio switched off and into update mode again
				// (flash.iap.retry_how), which a new listen waits for
				const loader = c.loader?.ready && c.loader.s === c.stream ? c.loader : (c.loader = makeLoader(c));
				// a loader that answered the probe is waiting already: its banner was printed before the page asked
				if (c.iapReady) c.iapReady = false;
				else if (!loader.ready) await loader.listen(LISTEN_MS, { signal: stop.signal });
				if (left()) throw new Error("stopped");
				writing = true;
				await busy(true);
				fill(status, h("p", { class: "muted c7-big" }, t("flash.iap.erasing")));
				prog.el.hidden = false;
				await loader.write(image, { signal: stop.signal, restore, name,
					onPhase: (p) => fill(status, h("p", { class: "muted c7-big" }, t(p === "erase" ? "flash.iap.erasing" : "flash.iap.writing"))),
					onPage: (i, n) => prog.set(i, n, t("flash.iap.page", { done: i, total: n })) });
				prog.set(1, 1, t(TEXTS[protocol()].written));
				ctx.flashFinished(started);
				c.resetAt = Date.now();
				await busy(false);
				fill(status, notice(done, "ok"));
				await sleep(800);
				if (!left()) onDone();
			} catch (e) {
				ctx.flashFinished(started, e);
				await busy(false);
				if (left()) return;
				const stopped = e.message === "stopped";
				fill(status, (stopped ? notice(t("flash.iap.cancelled"), "info") : errorNotice(t("flash.iap.failed", { error: ctx.errorText(e) }))),
					stopped ? null : h("div", { class: "notice info" }, h("p", {}, t("flash.iap.retry_safe")), h("p", { class: "small" }, t("flash.iap.retry_how"))),
					stopped ? null : ctx.helpLink());
				prog.el.hidden = true;
				go.disabled = false;
				S().forward(button(t("flash.c7.write.again"), () => run(), { primary: true }));
			} finally {
				clearInterval(watch);
			}
		}
		return box;
	}

	// Practice mode: the simulated radio switched on with its update mode keys held (fakec7000.js), where it has such a loader
	function simKeys() {
		const fake = S().fakeC7;
		if (!fake?.loaderId) return null;
		return h("div", { class: "actions" }, button(t("flash.sim.c7_off"), () => fake.powerOff()),
			button(t("flash.sim.c7_keys_on"), () => fake.powerOn({ keys: true }), { primary: true }));
	}

	return { stockFound, stubSilent, nothing, alreadyUpdateMode, iapWriteView, iapRestoreView };
}

