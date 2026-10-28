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
import { Usmart, stockName, BUILDS, UNKNOWN_BUILD } from "./usmart.js";
import { trimTail } from "./iap.js";
import { variantCode, verdict, writable, groups } from "./variant.js";
import * as diag from "../diag.js";
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

// The hardware variant the page knows the radio is (S().variant: {version, code, via}), judged against the image and
// build options chosen now: null when it is not known
export function variantVerdict(state, boardId) {
	const v = state.variant;
	return v?.code ? verdict(state.radio, v.code, boardId, state.buildOptions || {}) : null;
}

// A variant in development (variant.js verdict "dev"): its notice, as the pre-release banner shows it; else null
export function variantWarning(vd) {
	return vd?.kind === "dev" ? h("div", { class: "notice prerelease", role: "note" },
		h("p", {}, h("strong", {}, t("flash.variant.dev_title", { variant: `${vd.code} (${vd.variant.name})` })), " ", vd.variant.notice)) : null;
}

// host: what c7000ui.js hands over ({S, ctx, C, boardId, session, busy, loader, fill, stubImage}); loader(session) makes
// the update mode's protocol object (listen, write)
export function makeStockRoutes(host) {
	const { S, ctx, C, boardId, session, busy, loader: makeLoader, fill, stubImage, cableBanner = () => null } = host;
	const protocol = () => S().radio.flash.update_mode;
	const radioName = (id) => S().data?.radios?.find((r) => r.flash.board === id)?.name || id;

	// ---- the hardware variant (board.toml [hardware] variants)

	// Remember what the radio reported and judge it. via: "console" (the stock console's RAM copy), "identify" (the
	// vendor identify reply), "user" (picked from the Version screen). Returns the verdict, or null when the text names
	// no variant
	function seen(version, via, code = variantCode(S().radio, version)) {
		if (!code) return null;
		S().variant = { version, code, via };
		const vd = variantVerdict(S(), boardId());
		diag.note("radio variant", { version: version || null, code, via, verdict: vd.kind });
		return vd;
	}

	// Why the image can't go on this variant; nothing was written
	function variantStop(vd) {
		const what = vd.variant?.name ? `${vd.code} (${vd.variant.name})` : vd.code;
		const text = vd.kind === "other_board" ? t("flash.variant.other_board", { variant: what, radio: radioName(vd.board) })
			: vd.kind === "options" ? t("flash.variant.options", { variant: what, options: Object.entries(vd.need).map(([n, v]) => `${n} ${v}`).join(", ") })
			: t("flash.variant.unsupported", { variant: what, radio: S().radio.name });
		return h("div", {}, notice(text, "bad"), h("p", {}, t("flash.variant.nothing_written")), howToCheck(), reportLine());
	}

	// How to read the variant on the radio, and which ones RadioFoundry runs on
	function howToCheck(open = false) {
		const radio = S().radio;
		if (!radio.variants?.length) return null;
		const g = groups(radio);
		const line = (key, vs) => (vs.length ? h("li", {}, t(key, { codes: vs.map((v) => `${v.code} (${v.name})`).join(", ") })) : null);
		return h("details", { class: "small", open: open || null }, h("summary", {}, t("flash.variant.how_title")),
			h("p", {}, radio.variant_check),
			h("ul", {}, line("flash.variant.list_supported", g.supported), line("flash.variant.list_dev", g.dev), line("flash.variant.list_none", g.none)));
	}

	// Install through update mode on a radio whose variant the page could not read: the user picks it from the Version
	// screen, or says it can't be read (a radio that doesn't start). onChange(ok) tells the caller whether to allow
	// the install
	function variantPicker(onChange) {
		const radio = S().radio;
		const out = h("div");
		const say = h("div");
		const choices = [...radio.variants.map((v) => ({ code: v.code, label: `${v.code}: ${v.name}` })), { code: null, label: t("flash.variant.pick_unknown") }];
		const buttons = choices.map((ch) => h("button", { class: "choice", type: "button", "aria-pressed": "false", onclick: () => pick(ch) },
			h("strong", {}, ch.label)));
		function pick(ch) {
			buttons.forEach((b, i) => b.setAttribute("aria-pressed", String(choices[i] === ch)));
			if (ch.code === null) {
				S().variant = { version: null, code: null, via: "unknown" };
				diag.note("radio variant", { code: null, via: "unknown" });
				fill(say, notice(t("flash.variant.unknown_risk"), "info"));
				return onChange(true);
			}
			const vd = seen(null, "user", ch.code);
			fill(say, writable(vd) ? variantWarning(vd) : variantStop(vd));
			onChange(writable(vd));
		}
		out.append(h("p", {}, h("b", {}, t("flash.variant.pick_title"))), h("p", {}, radio.variant_check),
			h("div", { class: "choice-grid" }, buttons), say);
		onChange(false);
		return out;
	}

	// The original firmware answers, but the stub can't go in through its console (a build whose console load is not
	// proven, or a console that does not answer): no backup of it. Two ways on: put a version the page loads the stub
	// through on the radio first (the user's file, the manufacturer's tool), or install through update mode without a backup
	// tryAnyway: offer the stub load all the same (a build whose console has the known command table, or one the page
	// doesn't know); failed: the error of such a try, which this firmware made likely
	function noBackup({ show, act, onUpdateMode, onAgain }, version, vd, { tryAnyway = null, failed = null } = {}) {
		// the builds whose console takes the stub, by the board's names for them
		const versions = BUILDS.filter((b) => b.stub).map((b) => `${stockName(S().radio.flash, b.name)} (${b.date})`).join(" or ");
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.usmart.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)) : null,
			variantWarning(vd),
			failed ? notice(t("flash.variant.try_failed"), "info") : notice(t(tryAnyway ? "flash.variant.no_backup_known" : "flash.variant.no_backup"), "info"),
			failed ? h("details", { class: "small" }, h("summary", {}, t("flash.problem.details")), h("pre", { class: "log" }, failed)) : null,
			h("ol", { class: "steps" },
				tryAnyway ? h("li", {}, h("b", {}, t("flash.variant.try_title")), " ", t("flash.variant.try")) : null,
				h("li", {}, h("b", {}, t("flash.variant.update_first_title")), " ", t("flash.variant.update_first", { versions })),
				onUpdateMode ? h("li", {}, h("b", {}, t("flash.variant.direct_title")), " ", t("flash.variant.direct")) : null),
			howToCheck(), reportLine());
		act(tryAnyway ? button(t("flash.variant.try_go"), tryAnyway, { primary: true }) : null,
			onUpdateMode ? button(t("flash.variant.direct_go"), onUpdateMode, { primary: !tryAnyway, quiet: !!tryAnyway }) : null,
			button(t("flash.iap.none.again"), onAgain, { primary: !onUpdateMode && !tryAnyway, quiet: !!onUpdateMode || !!tryAnyway }));
	}

	// The console did not answer but the stock application answered the identify (the cable works): judge the variant,
	// then offer the ways on without a backup
	function stockIdentified(parts, version) {
		const vd = seen(version, "identify");
		if (vd && !writable(vd)) {
			parts.show({ led: "off", screen: "on" }, h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)), variantStop(vd));
			return parts.act(button(t("flash.iap.none.again"), parts.onAgain, { primary: true }));
		}
		noBackup(parts, version, vd);
	}

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
		let build = null, version = null;
		try {
			build = await con.build();
			if (build) version = await con.version(build);
		} catch (e) {
			show({ led: "off", screen: "on" }, errorNotice(t("flash.usmart.failed", { error: ctx.errorText(e) })));
			return act(button(t("flash.iap.none.again"), onAgain, { primary: true }), other);
		}
		// a build this page does not know: its version string looked for in RAM (the identify would silence the console
		// until a power cycle, and the backup is still worth a try)
		// a RAM copy that doesn't read as a version (a build whose RAM differs from what the table says): looked for too
		if (version && !variantCode(S().radio, version)) version = null;
		if (!version) {
			try {
				version = await con.findVersion(S().radio.variant_pattern);
			} catch (e) {
				version = null;
			}
		}
		diag.note("stock firmware", { build: build ? `${build.name} ${build.date}` : null, version: version || null, console_stub: build?.stub ? "proven" : "untried" });
		const vd = version ? seen(version, "console") : null;
		if (vd && !writable(vd)) {
			show({ led: "off", screen: "on" }, version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)) : null,
				variantStop(vd));
			return act(button(t("flash.iap.none.again"), onAgain, { primary: true }));
		}
		const parts = { show, act, onLoaded, onUpdateMode, onAgain };
		const shown = version || (build && `${build.name} (${build.date})`);
		if (!build?.stub) return noBackup(parts, shown, vd, { tryAnyway: () => loadOffer(parts, con, shown, vd, true, build || UNKNOWN_BUILD) });
		return loadOffer(parts, con, version || stockName(S().radio.flash, build.name), vd, false, build);
	}

	// The stub load through the console, from the user's go. untried: a build whose load is not proven; a failure is
	// then what this firmware made likely, and the page says so and offers the other ways on. Its backup is offered
	// to the project (session().stockUntried)
	// build: the BUILDS entry (UNKNOWN_BUILD for a build the page doesn't know), whose window and hijack word the load uses
	async function loadOffer({ show, act, onLoaded, onUpdateMode, onAgain }, con, version, vd, untried, build) {
		const other = onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true });
		const image = await stubImage();
		const words = Math.ceil(image.length / 4);
		const prog = progressBar();
		const status = h("div");
		prog.el.hidden = true;
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.usmart.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.usmart.version")), h("dd", {}, version)) : null,
			variantWarning(vd),
			untried ? notice(t("flash.variant.try_note"), "info") : null,
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
				session().stockUntried = untried ? { version: version || null } : null;
				await con.loadStub(image, { signal: ac.signal, untried, build, onProgress: (phase, d, n) => {
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
				if (untried && !stopped) {
					diag.note("untried stock build: stub load failed", { version: version || null, error: ctx.errorText(e) });
					return noBackup({ show, act, onUpdateMode, onAgain }, version, vd, { failed: ctx.errorText(e) });
				}
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
		const untried = session().stockUntried;
		if (untried) diag.note("untried stock build: the stub did not answer", { version: untried.version, error });
		show({ led: "off", screen: "any", knob: "cycle" }, untried ? notice(t("flash.variant.try_silent"), "info") : errorNotice(t("flash.usmart.no_stub", { error })),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.usmart.no_stub_off")), h("li", {}, t("flash.usmart.no_stub_on"))));
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode, { quiet: true }));
	}

	// Nothing on the cable answered: the radio is off, in update mode already, or runs RadioFoundry with a silent link
	function nothing({ show, act, onUpdateMode, onPowerOn, onAgain }) {
		show({ led: "off", screen: "any" },
			notice(t("flash.iap.none.lead"), "info"),
			h("ul", {}, h("li", {}, t("flash.iap.none.on")), h("li", {}, t("flash.iap.none.cable")), h("li", {}, t("flash.iap.none.stock"))),
			h("p", { class: "small muted" }, t("flash.iap.none.rfoundry")), howToCheck());
		act(button(t("flash.iap.none.again"), onAgain, { primary: true }), onUpdateMode && button(t("flash.iap.none.update_mode"), onUpdateMode),
			button(t("flash.iap.none.power_on"), onPowerOn, { quiet: true }));
	}

	// The loader answered a read-only probe: the radio is in update mode already (its application is missing or incomplete,
	// so it stays there at every power-on, white screen and red LED). The install goes straight on; no keys are needed.
	function alreadyUpdateMode({ show, act, onUpdateMode, onAgain }, version) {
		show({ led: "off", screen: "on" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.iap.um.found"))),
			version ? h("dl", { class: "kv-list" }, h("dt", {}, t("flash.iap.um.loader")), h("dd", {}, version)) : null,
			h("p", {}, t("flash.iap.um.text")), h("p", { class: "small muted" }, t("flash.iap.um.safe")),
			S().radio.variants?.length ? h("p", { class: "small" }, t("flash.variant.not_read")) : null, howToCheck());
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
		// the variant: read on the way here (judged again: the build options may have changed since), or picked now.
		// Update mode itself can't tell it
		const vd = variantVerdict(S(), boardId());
		if (vd && !writable(vd)) return panel(t("flash.c7.write.title"), variantStop(vd));
		const gate = vd || S().variant || !S().radio.variants?.length ? null : (go) => variantPicker((ok) => { go.disabled = !ok; });
		// checked again when the write starts: the pick can change while the page listens, and Write Again is not go
		const allowed = () => {
			const now = variantVerdict(S(), boardId());
			return now ? writable(now) : !!S().variant || !S().radio.variants?.length;
		};
		return iapPanel({ title: t("flash.c7.write.title"), image, restore: false, gate, allowed,
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

	// gate(go): an element that decides when go may be pressed (the variant picker); allowed(): whether the write may
	// start, asked again once the radio answers
	function iapPanel({ title, rows, image, restore, report, done, onDone, name, lead = null, gate = null, allowed = () => true }) {
		const c = session();
		const prog = progressBar();
		const status = h("div");
		const steps = S().radio.flash.bootloader || [];
		const go = button(t(restore ? "flash.iap.restore_go" : "flash.iap.go"), () => run(), { primary: true });
		const box = panel(title, cableBanner(),	// a CH340 cable: the warning here too (programming through update mode)
			rows.length ? h("dl", { class: "kv-list" }, ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
			gate ? gate(go) : null,
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
				if (!allowed()) throw new Error(t("flash.variant.nothing_written"));
				writing = true;
				await busy(true);
				fill(status, h("p", { class: "muted c7-big" }, t("flash.iap.erasing")));
				prog.el.hidden = false;
				await loader.write(image, { signal: stop.signal, restore, name,
					onPhase: (p) => fill(status, h("p", { class: "muted c7-big" }, t(p === "erase" ? "flash.iap.erasing" : "flash.iap.writing"))),
					onPage: (i, n) => { if (i > 0) ctx.flashMark(started, "first_byte"); prog.set(i, n, t("flash.iap.page", { done: i, total: n })); } });
				ctx.flashMark(started, "last_byte");
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

	return { stockFound, stockIdentified, stubSilent, nothing, alreadyUpdateMode, iapWriteView, iapRestoreView, variantStop, variantPicker, howToCheck };
}

