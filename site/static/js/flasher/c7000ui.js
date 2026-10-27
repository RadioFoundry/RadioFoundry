// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flasher's steps for the HR_C7000 radios, which install through the C7 stub over the
// programming cable instead of a bootloader (stub.js, c7000.js):
//
//   connect   choose the cable; the page finds what answers: a stub left running, RadioFoundry (which hands over to the stub, no
//             buttons), or nothing, and then asks for the radio to be switched off and on to meet its boot ROM
//   check     back up the program memory (and the data memory), find out what the radio runs, decide the codec
//   write     write only the sectors that differ, each read back by CRC; the first sector last
//   done      restart the radio and ask RadioFoundry on it for its version and codec
//
// Redundancy, in order: the boot ROM is in the chip and the bootloader is never written, so the radio can always be
// loaded again; before a first install the whole program memory is read, kept in this browser and saved as a
// download; the codec slot is kept in this browser before anything is written; every sector is checked by CRC after
// it is written, and a second pass checks them all; a failed or interrupted install is resumed by running it again
// (only what still differs is written).

import { h, t, notice, button, panel, progressBar, logBox, download, filePicker, localTime, partNames } from "./dom.js";
import { hex, sha256, sleep } from "./bytes.js";
import { SerialStream, Link, probe, serialSupported } from "./link.js";
import { Stub, stubImages, StubError, DONE_LED, MEM, ROM_REPEAT } from "./stub.js";
import * as c7 from "./c7000.js";
import { openVendorFile, describe as describeVendorFile, analysisCandidate } from "./vendorfile.js";
import { pickProblem } from "./explain.js";
import { upgradeSlot } from "./c7000v1.js";
import { radioTop } from "./radiotop.js";
import { putBlob, getBlob } from "./codecsave.js";
import { makeBundle, describeFirmware, bundleName } from "./backup.js";
import { register } from "./backends.js";
import { makeStockRoutes } from "./iapui.js";
import { Usmart, stockVersion, stockName } from "./usmart.js";
import { Iap, checkInstallImage } from "./iap.js";
import { BfLoader } from "./bfloader.js";
import { errorNotice, reportLine, reportButton } from "../report.js";
import * as diag from "../diag.js";
import { fromSerialInfo, isCh340 } from "./adapter.js";
import * as crumbs from "./trail.js";

// USB-serial chips found in programming cables (tools/serial_loader.py USB_SERIAL), for the port chooser
const CABLES = [0x0403, 0x1A86, 0x10C4, 0x067B].map((usbVendorId) => ({ usbVendorId }));
const CH340 = 0x1A86;
const ROM_WAIT = 180000;		// how long the page listens for the boot ROM per try
const BOOT_MS = 9000;			// a soft reset to RadioFoundry answering its debug link (bench 2026-10-15: 8-9 s)

let ctx = null;				// from app.js: {S, next, render, fb, flashStarted, flashFinished, flashMark, errorText, analysisOffer,
					// problemNotice, failureNotice, stubJson}
const S = () => ctx.S;
const C = () => ctx.fb().c7000;
const boardId = () => S().radio.flash.board;
const store = (what) => `c7000-${what}-${boardId()}`;

// What a board's radios.json flash data adds (iapui.js): stock_console, the stub started through the stock firmware's
// debug console (usmart.js); update_mode, the radio's own factory update mode, by protocol: the way in when nothing
// else answers or the boot ROM load fails
let routes = null;
const flash = () => S().radio.flash;
// The radio's data memory, from its board profile: the SPI NOR data flash (q128), or an SPI NAND (nand) on the same bus.
// Where the factory calibration sits is the profile's too (board.toml [flash.stub] calibration): a page of the data
// flash's stock codeplug area ({area, page_id}), or two 4 KB copies, in the program memory and the NAND ({progmem,
// nand, magic})
const dataMem = () => (C().boards[boardId()].data_flash === false ? "nand" : "q128");
const calLayout = () => C().boards[boardId()].calibration || null;
const hasCal = () => !!calLayout();
const CONSOLES = { usmart: Usmart };	// flash.stock_console: the console's protocol
const stockRadio = () => !!CONSOLES[flash().stock_console];
// flash.update_mode: iap, a vendor IAP loader (iap.js) that a radio without a complete application stays in at every
// power-on (stuck), so the page looks for it when nothing else answers; baofeng, the Baofeng updater (bfloader.js),
// entered only with the keys held, which erases the whole application area. Neither loader is ever written.
const LOADERS = {
	iap: { stuck: true, make: (c) => new Iap(c.stream, { log: (l) => c.log.add(l), firstStage: flash().first_stage,
		protect: C().boards[boardId()].protect }) },
	baofeng: { stuck: false, make: (c) => new BfLoader(c.stream, { log: (l) => c.log.add(l), id: C().boards[boardId()].bootloader_id,
		appMax: C().app_end - C().app_start, appStart: C().app_start, protect: C().boards[boardId()].protect }) },
};
const updateMode = () => !!LOADERS[flash().update_mode];
const stuckMode = () => !!LOADERS[flash().update_mode]?.stuck;
const iap = (c) => LOADERS[flash().update_mode].make(c);
const progSize = () => c7.totalSize(C(), boardId());
const viaIap = () => session().how === "iap";
// What the radio's screen shows while the stub runs: RadioFoundry's loader screen, whatever the original firmware last drew, or
// nothing after a boot ROM load
const screenNow = () => ({ rfoundry: "loader", usmart: "any" })[session().how] || "off";

export function init(context) {
	ctx = context;
	routes = makeStockRoutes({ S, ctx, C, boardId, session, busy, loader: iap, fill: (el, ...p) => fill(el, ...p), stubImage: async () => (await stubs()).image,
		cableBanner: () => cableBanner() });
}

// The factory update mode route of a radio (radios.json flash.update_mode), or null: its protocol, and what the install
// needs for the codec, since nothing can be read from the radio that way: "file", the radio's stock firmware file (a
// profile of the board names one), else "backup", a program memory backup of the radio (or a slot this browser kept).
// fb: the site's own data, not a practice radio's (whose made-up profile has no file)
export function factoryRoute(radio, fb) {
	const kind = LOADERS[radio?.flash.update_mode];
	if (!kind) return null;
	const file = (fb.c7000?.profiles || []).some((p) => p.board === radio.flash.board && p.stock_file);
	return { protocol: radio.flash.update_mode, stuck: kind.stuck, needs: file ? "file" : "backup" };
}

// Radios the page flashes this way
export const isC7000 = (radio, fb) => radio?.flash.backend === "c7000-stub" && !!fb.c7000?.boards[radio.flash.board];

// The universal stub image, loaded once (data/c7000-stub.json)
let images = null;
async function stubs() {
	if (!images) images = stubImages(await ctx.stubJson());
	return images;
}

// Keep the computer awake and warn before the page is closed while the radio is being read or written
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

function session() {
	return (S().c7 ||= { stream: null, stub: null, how: null, rfoundry: null, progmem: null, q128: null, slot: null, analysis: null,
		decision: null, fileCodec: null, backupSaved: false, rate: null, log: logBox() });
}

export async function dropC7() {
	const c = S().c7;
	if (!c?.stream) return;
	c.stopSizing = true;
	c.size = null;	// the next connection may be another radio
	if (!S().fakeC7) try { await c.stream.close(); } catch (e) { /* gone */ }
	c.stream = null;
	c.stub = null;
}

async function openCable(any = false) {
	const c = session();
	if (c.stream) return c.stream;
	if (S().fakeC7) return (c.stream = S().fakeC7.stream);
	if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
	const port = await navigator.serial.requestPort(any ? {} : { filters: CABLES });
	c.portInfo = port.getInfo();
	ctx.useAdapter?.(fromSerialInfo(c.portInfo));
	c.stream = await SerialStream.open(port);
	return c.stream;
}

// A CH340 cable (adapter.js): the warning on every step that works through it
export function cableBanner(a = S().adapter) {
	return isCh340(a) ? h("div", { class: "notice action cable-warning" }, h("p", {}, h("b", {}, t("flash.cable.ch340_title"))),
		h("p", {}, t("flash.cable.ch340_text"))) : null;
}

// replaceChildren() would print a null part as "null"
const fill = (el, ...parts) => el.replaceChildren(...parts.filter(Boolean));

// the chips a programming cable has: the boot ROM errors name the cable by it (never "radio USB")
const USB_SERIAL = new Set(["FTDI", "CH340", "CP210x", "PL2303"]);

const mkStub = () => {
	const c = session();
	c.stub = new Stub(c.stream, { log: (l) => c.log.add(l), cable: USB_SERIAL.has(S().adapter?.chip) ? S().adapter.chip : "" });
	c.stub.size.progmem = progSize();
	return c.stub;
};

// ---------------------------------------------------------------------------------------------------------------
// connect: the cable, then whatever answers; ends with the stub running and the radio identified

// onUpdateMode: what the task does with a radio reached through the vendor update mode (null: the task can't use it)
function connectPanel(onReady, { onUpdateMode = null } = {}) {
	const c = session();
	const radio = S().radio;
	const top = radioTop(radio, { led: "off", screen: "any" });
	const body = h("div");
	const box = panel(t("flash.c7.connect.title", { name: radio.name }), h("div", { class: "c7-grid" }, body, top.el));
	let abort = null;
	// The user left this panel (Start over, Back) while a wait or a load still runs: when it ends, it must not draw
	// into the step that replaced it. Start over drops the session (S().forward goes with it); Back takes the panel off
	// the page.
	let attached = false;
	const gone = () => {
		if (box.isConnected) attached = true;
		const left = S().c7 !== c || typeof S().forward !== "function" || (attached && !box.isConnected);
		if (left) abort?.abort();
		return left;
	};
	// each state shows its parts and, through act(), its one action in the nav bar
	const show = (state, ...parts) => {
		if (gone()) return;
		top.set(state);
		fill(body, cableBanner(), ...parts);
		S().forward(null);
	};
	const act = (...all) => {
		if (gone()) return;
		const btns = all.filter(Boolean);
		S().forward(btns.length > 1 ? btns : btns[0] || null);
	};

	// err: an error's text (errorText: reported) or {text} for a message that is no error of the page's
	const cableStep = (err = null) => {
		show({ led: "off", screen: "any", note: t("flash.c7.visual.cable") },
			h("ol", { class: "steps c7-big" },
				h("li", {}, t("flash.c7.connect.step_cable")),
				h("li", {}, t("flash.c7.connect.step_choose"))),
			!err ? null : typeof err === "string" ? errorNotice(err) : notice(err.text, "bad"),
			h("p", { class: "small muted" }, t("flash.c7.connect.no_cable"), " ",
				h("a", { href: "#", onclick: (e) => { e.preventDefault(); start(true); } }, t("flash.c7.connect.all_ports"))),
			h("details", { class: "small" }, h("summary", {}, t("flash.c7.connect.which_port")), h("p", {}, t("flash.c7.connect.which_port_text"))));
		act(button(t("flash.c7.connect.choose"), () => start(false), { primary: true }));
	};

	const start = async (any) => {
		try {
			await openCable(any);
		} catch (e) {
			if (e?.name === "NotFoundError") return cableStep();
			return cableStep(ctx.errorText(e));
		}
		// Troubleshooting asked for update mode: nothing else is tried
		if (c.forceIap && toIap) {
			c.forceIap = false;
			// a radio already in update mode printed its banner at power-on: remember that, so the install doesn't wait for it.
			// A loader entered only with the keys held is listened for in the install's own step
			c.iapReady = stuckMode() && !!(await iap(c).probe());
			return toIap();
		}
		await find();
	};

	// a stub from an earlier run, then RadioFoundry, then the boot ROM
	const find = async () => {
		show({ led: "off", screen: "any", note: t("flash.c7.visual.cable") }, h("p", { class: "muted c7-big" }, t("flash.c7.connect.looking")));
		const stub = mkStub();
		if (await stub.attach()) {
			c.how = "attach";
			return ready(t("flash.c7.connect.found_stub"));
		}
		const link = new Link(c.stream, { timeout: 1200 });
		const p = await probe(link);
		if (p) {
			c.rfoundry = p;
			if (!S().board.debuglink_names.includes(p.board))
				return cableStep({ text: t("flash.err.wrong_board", { board: p.board, name: S().board.name }) });
			const offer = await Stub.rfoundryOffer(link);
			if (offer) return fromRFoundry(offer);
			return restartRFoundry();
		}
		if (stockRadio() || stuckMode()) return stockOrNothing();
		return powerCycle();
	};

	// A radio with a stock console or an update mode that is not RadioFoundry: the original firmware answers its debug console
	// (the stub goes in through it), or nothing answers (update mode, or the first stage's power-on window for a RadioFoundry
	// that does not answer)
	const toIap = onUpdateMode && updateMode() && (() => {
		c.how = "iap";
		onUpdateMode();
	});
	const stockOrNothing = async () => {
		const Console = CONSOLES[flash().stock_console];
		if (Console && await new Console(c.stream, { log: (l) => c.log.add(l) }).probe()) {
			return routes.stockFound({ show, act, onLoaded: fromStock, onAgain: () => find(), onUpdateMode: toIap });
		}
		// the loader's update mode answers a read-only probe: a radio whose application won't start stays in it
		c.iapReady = false;
		const um = stuckMode() && await iap(c).probe();
		if (um) {
			c.iapReady = true;
			return routes.alreadyUpdateMode({ show, act, onUpdateMode: toIap, onAgain: () => find() }, um.version);
		}
		routes.nothing({ show, act, onAgain: () => find(), onPowerOn: () => powerCycle(), onUpdateMode: toIap });
	};

	// The console started the stub: meet it as RadioFoundry's hand-over does
	const fromStock = async () => {
		const stub = mkStub();
		try {
			await sleep(300);
			await stub.drain(300);
			await stub.helloWithin(3000, t("flash.usmart.no_answer"));
		} catch (e) {
			return routes.stubSilent({ show, act, onAgain: () => find(), onUpdateMode: toIap }, ctx.errorText(e));
		}
		c.how = "usmart";
		await ready(t("flash.usmart.found_stub"));
	};

	const fromRFoundry = async (offer) => {
		const prog = progressBar();
		show({ led: "off", screen: "rfoundry" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.rfoundry_found", { git: c.rfoundry.git }))),
			h("p", {}, t("flash.c7.connect.rfoundry_loading")), prog.el);
		try {
			await mkStub().enterFromRFoundry((await stubs()).image, (d, n) => prog.set(d, n, t("flash.c7.connect.loader")));
			c.how = "rfoundry";
			await ready(t("flash.c7.connect.found_rfoundry_stub"));
		} catch (e) {
			show({ led: "off", screen: "rfoundry" }, errorNotice(t("flash.c7.connect.rfoundry_failed", { error: ctx.errorText(e) })));
			act(button(t("flash.c7.connect.power_instead"), () => powerCycle(), { primary: true }));
		}
	};

	// RadioFoundry without the stub hand-over: its CPS reboot (C 0, C 6 1) is a soft reset, and a soft reset runs the boot ROM,
	// so the page can meet the ROM without a power cycle. Falls back to the power cycle if the ROM doesn't show.
	const restartRFoundry = async () => {
		show({ led: "off", screen: "rfoundry" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.rfoundry_found", { git: c.rfoundry.git }))),
			h("p", {}, t("flash.c7.connect.rfoundry_restart")));
		const stub = mkStub();
		const waiting = stub.waitForRom(5000);
		await c.stream.write(new Uint8Array([0x43, 0x00]));
		await sleep(100);
		await c.stream.write(new Uint8Array([0x43, 0x06, 0x01]));
		try {
			await waiting;
			await load(stub);
		} catch (e) {
			powerCycle(t("flash.c7.connect.rfoundry_restart_failed"));
		}
	};

	// off: only the off switch (the step that waits for Radio Is Off)
	const simPower = ({ off = false } = {}) => S().fakeC7 ? h("div", { class: "actions" },
		button(t("flash.sim.c7_off"), () => S().fakeC7.powerOff()), off ? null : button(t("flash.sim.c7_on"), () => S().fakeC7.powerOn(), { primary: true })) : null;

	// Wait for the boot ROM while the user switches the radio off and on. slow: the slow cable mode, the radio already off
	// (slowCable below)
	const powerCycle = async (why = null, { slow = false } = {}) => {
		abort?.abort();
		const ac = (abort = new AbortController());
		const t0 = Date.now();
		const hints = h("div");
		const clock = h("span", { class: "muted" });
		const cancel = button(t("flash.c7.connect.cancel"), () => { ac.abort(); clearInterval(tick); cableStep(); }, { quiet: true });
		const tick = setInterval(() => {
			if (ac.signal.aborted) return clearInterval(tick);
			const s = Math.round((Date.now() - t0) / 1000);
			clock.textContent = t("flash.c7.connect.listening", { s });
			if (s === 25) {
				hints.replaceChildren(powerHints(), slow ? null : slowWay());
				// nothing heard yet: the slow cable mode is offered next to Cancel
				if (!slow) act(cancel, slowButton());
			}
		}, 1000);
		show({ led: "off", screen: "off", knob: slow ? "on" : "cycle" },
			why ? notice(why, "info") : null,
			slow ? h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.slow.title"))) : null,
			h("ol", { class: "steps c7-big" },
				slow ? null : h("li", {}, t("flash.c7.connect.step_off")),
				h("li", {}, t(slow ? "flash.c7.slow.step_on" : "flash.c7.connect.step_on"))),
			h("p", {}, t("flash.c7.connect.no_keys")), simPower(), h("p", { class: "small" }, clock), hints);
		act(cancel);
		const stub = mkStub();
		try {
			const { answered } = await stub.waitForRom(ROM_WAIT, { signal: ac.signal, repeat: slow ? ROM_REPEAT : 0 });
			clearInterval(tick);
			if (gone()) return;
			await load(stub, answered);
		} catch (e) {
			clearInterval(tick);
			if (ac.signal.aborted && e.message === "stopped") return;
			const what = e?.code === "rom-booted" ? "flash.c7.connect.rom_booted" : slow ? "flash.c7.slow.no_rom" : "flash.c7.connect.no_rom";
			romFailed(slow, notice(t(what), "bad"), reportLine());
		}
	};

	// Slow cable mode (stub.js waitForRom repeat): the boot ROM waits only about 48 ms for the answer to its greeting, and
	// a slow cable or USB hub can take longer than that, so the page sends the answer every ROM_REPEAT ms while the
	// radio is switched on. It starts only once the user says the radio is off: the firmware of a radio still on would
	// get it too (RadioFoundry ignores it; other firmware might not)
	const slowCable = () => {
		abort?.abort();
		show({ led: "off", screen: "off" },
			h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.slow.title"))),
			h("p", {}, t("flash.c7.slow.intro")),
			h("ol", { class: "steps c7-big" }, h("li", {}, t("flash.c7.slow.step_off"))),
			notice(t("flash.c7.slow.why_off"), "info"), simPower({ off: true }));
		act(button(t("flash.c7.connect.cancel"), () => cableStep(), { quiet: true }),
			button(t("flash.c7.slow.is_off"), () => powerCycle(null, { slow: true }), { primary: true }));
	};
	const slowWay = () => h("div", { class: "notice action" }, h("p", {}, h("b", {}, t("flash.c7.slow.offer_title"))), h("p", {}, t("flash.c7.slow.intro")));
	const slowButton = () => button(t("flash.c7.slow.button"), slowCable, { primary: true });

	// The boot ROM load failed or never started. Next comes the slow cable mode; once that failed too, the radio's
	// factory update mode, or the slow cable mode again on a radio without one. slow: this was the slow cable mode
	const romFailed = (slow, ...parts) => {
		show({ led: "off", screen: "off" }, ...parts, powerHints(), slow ? factoryWay() : slowWay(), ctx.manualKitLine?.());
		if (!slow) return act(button(t("flash.c7.connect.try_again"), () => powerCycle(), { quiet: true }), slowButton());
		const factory = factoryButton({ primary: true });
		const again = button(t("flash.c7.slow.again"), slowCable, { primary: !factory, quiet: !!factory });
		// [alternative, main]: the factory update mode is the main action once both cable ways failed
		act(again, factory);
	};

	// When the boot ROM load fails or never starts: the radio's own update mode, which needs no load over the cable.
	// Nothing is read from the radio that way, so the install says what it takes the codec from
	const factoryWay = () => {
		const route = toIap && factoryRoute(S().radio, S().data.fb);
		if (!route) return null;
		const text = S().task === "restore" ? "flash.factory.offer_restore" : `flash.factory.offer_${route.needs}`;
		return h("div", { class: "notice action" }, h("p", {}, h("b", {}, t("flash.factory.offer_title"))), h("p", {}, t(text)));
	};
	const factoryButton = (opts = {}) => toIap && button(t("flash.iap.none.update_mode"), toIap, opts);

	const powerHints = () => {
		const ch340 = c.portInfo?.usbVendorId === CH340;
		return h("div", { class: "notice info" }, h("p", {}, h("b", {}, t("flash.c7.connect.hints_title"))),
			h("ul", {}, h("li", {}, t("flash.c7.connect.hint_order")), h("li", {}, t("flash.c7.connect.hint_plug")),
				h("li", {}, t("flash.c7.connect.hint_battery")), ch340 ? h("li", {}, t("flash.c7.connect.hint_ch340")) : null));
	};

	// answered: the slow cable mode's answer reached the boot ROM already
	const load = async (stub, answered = false) => {
		const prog = progressBar();
		show({ led: "off", screen: "off" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.rom_found"))), prog.el);
		try {
			// one image for every radio, at the boot ROM's address (a first stage takes it there too)
			const st = await stubs();
			await stub.loadThroughRom(st.image, st.load, (d, n) => prog.set(d, n, t("flash.c7.connect.loader")), { answered });
			c.how = answered ? "rom-slow" : "rom";
			await ready(t("flash.c7.connect.found_rom"));
		} catch (e) {
			romFailed(answered, errorNotice(t("flash.c7.connect.load_failed", { error: ctx.errorText(e) })));
		}
	};

	// The stub runs: the LED, both chips, which radio this is
	const ready = async (how) => {
		const st = c.stub;
		// a wait or a load from a panel the user left ends here (show() draws nothing then, and the session may have
		// dropped the stub)
		if (gone() || !st) return;
		const screen = screenNow();
		show({ led: "wait", screen }, h("p", { class: "c7-big" }, h("b", {}, how)), h("p", { class: "muted" }, t("flash.c7.connect.checking")));
		try {
			const led = C().led;
			// the radio's profile, checked before any write, and its protected regions, which every write checks
			await st.configure({ ...C().boards[boardId()].stub_board, protect: C().boards[boardId()].protect || [] }, led);
			await st.setupLed(led.red, led.green);
			c.jedec = { progmem: await st.checkId("progmem"), [dataMem()]: await st.checkId(dataMem()) };
			// RadioFoundry named the radio already; otherwise the bootloader's ID string does (48 KB, a few seconds). A radio
			// whose loader has no ID string is the one the page was told
			c.model = c.rfoundry || !C().boards[boardId()].bootloader_id ? boardId()
				: c7.bootloaderModel(C(), c.progmem || (await st.dump("progmem", 0, C().app_start)));
			if (c.model && c.model !== boardId()) {
				const other = Object.values(S().data.radios).find((r) => r.flash.board === c.model);
				show({ led: "wait", screen }, notice(t("flash.c7.connect.wrong_radio", { name: S().radio.name, other: other?.name || c.model }), "bad"));
				return act(leaveLoaderButton());
			}
			show({ led: "wait", screen }, h("p", { class: "c7-big" }, h("b", {}, how)),
				h("dl", { class: "kv-list" },
					h("dt", {}, t("flash.c7.radio")), h("dd", {}, c.model ? S().radio.name : t("flash.c7.radio_unconfirmed"))),
				h("p", {}, t({ rfoundry: "flash.c7.connect.led_rfoundry", usmart: "flash.usmart.led" }[c.how] || "flash.c7.connect.led_rom")));
			await sleep(S().fakeC7 ? 400 : 1500);
			onReady();
		} catch (e) {
			show({ led: "wait", screen }, errorNotice(ctx.errorText(e)));
			act(button(t("flash.c7.connect.try_again"), () => find(), { primary: true }));
		}
	};

	if (c.stub && c.stream) {
		// already connected (back from a later step): check the stub still answers
		quiet(c).then(() => c.stub.attach()).then((ok) => (ok ? ready(t("flash.c7.connect.still")) : cableStep()), () => cableStep());
	} else cableStep();
	return box;
}

// Leave loader mode: the chip restarts into whatever firmware is in flash
function leaveLoaderButton(label = t("flash.c7.leave")) {
	return button(label, async () => {
		try { await S().c7.stub.resetChip(); } catch (e) { /* the radio may be gone already */ }
		await dropC7();
		ctx.render();
	}, { primary: true });
}

export function connectView() {
	return connectPanel(() => ctx.next(), { onUpdateMode: () => {
		session().analysis = { firmware: "unknown", slot: null, codec: null };
		ctx.next();
	} });
}

// ---------------------------------------------------------------------------------------------------------------
// check: back up, find out what is installed, decide the codec

// The factory calibration, from what was read already where it can be, else read by itself. c.calCopies: the bundle
// parts it gives; c.calFound: whether a copy is the calibration. A data flash radio's is the page the stock codeplug area
// names (c.calArea keeps that area for a backup without the whole data memory); a NAND radio's are the two sectors its
// profile names, each a copy when it starts with the profile's magic.
async function readCalibration(c, prog) {
	const progress = (d, n) => prog.set(d, n, t("flash.c7.check.cal"));
	const lay = calLayout();
	if (!lay) {
		c.calFound = false;
		c.calCopies = [];
		return;
	}
	if (lay.area) {
		c.calArea = c.q128 ? c.q128.subarray(0, lay.area) : await c.stub.dump("q128", 0, lay.area, { progress });
		const at = c7.findCalibration(c.calArea, lay);
		c.calFound = at >= 0;
		c.calCopies = c.calFound ? [{ file: "calibration.bin", what: "c7000-calibration", addr: at, data: c.calArea.slice(at, at + 0x1000) }] : [];
		return;
	}
	const sector = async (mem, addr, whole) => (whole ? whole.slice(addr, addr + 0x1000) : await c.stub.dump(mem, addr, addr + 0x1000, { progress }));
	const inNand = await c7.findNandCalibration((a) => sector("nand", a, c.q128), lay);
	const copies = [
		{ where: "nand", file: "calibration.bin", what: "c7000-calibration-nand", ...inNand },
		{ where: "progmem", file: "calibration-program-memory.bin", what: "c7000-calibration-progmem", addr: C().progmem_base + lay.progmem,
			data: await sector("progmem", lay.progmem, c.progmem) }];
	const { found, missing, keep } = c7.calibrationCopies(copies, lay.magic);
	c.calFound = found;
	c.calMissing = missing;
	c.calCopies = keep.map(({ where, ...p }) => p);
}

// The notice for a calibration found in one copy only, or null
const calOneCopy = (c) => (c.calMissing ? notice(t("flash.c7.backup.cal_one_copy", { where: t(`flash.c7.cal_where_${c.calMissing}`) }), "info") : null);

// The backup choices, shared by the install's first step and the backup task: the program memory, the factory
// calibration and the whole data memory, each with its size from the board profile. The data memory holds the
// calibration (a NAND radio's main copy), so choosing that one includes it; a program memory that holds a calibration
// copy says so. Each says how long it takes; refresh() says it again once the radio has counted its blank sectors
// (sizeUp).
function backupOptions(want, { firstInstall = false, text = (k) => t(`flash.c7.backup.${k}_text`) } = {}) {
	const box = {}, times = {};
	// restore writes back the program memory only (restoreView): without it the backup can't restore the radio's firmware
	const noProgmem = notice(t("flash.c7.backup.no_progmem"), "info");
	const showNote = () => { noProgmem.hidden = !!want.progmem; };
	const opt = (k, title, why, disabled = false) => h("label", { class: "toggle" },
		box[k] = h("input", { type: "checkbox", checked: want[k], disabled, onchange: (e) => {
			want[k] = e.target.checked;
			if (k === "q128" && box.cal) { box.cal.checked = want.cal || want.q128; box.cal.disabled = want.q128; }
			showNote();
		} }),
		h("span", {}, h("b", {}, title), h("span", { class: "why" }, why, " ", times[k] = h("span", { "data-time": k }))), h("span"));
	const mb = (n) => `${n / 0x100000} MB`;
	const calSpan = calLayout()?.area || 0x2000;	// the codeplug area, or the two copies
	const spans = { progmem: ["progmem", progSize()], cal: [dataMem(), calSpan], q128: [dataMem(), dataSize()] };
	const refresh = () => { for (const [k, el] of Object.entries(times)) el.textContent = readTime(...spans[k]); };
	const calInProgmem = calLayout()?.progmem !== undefined;
	const el = h("div", {},
		calInProgmem ? opt("progmem", t("flash.c7.check.opt_progmem_whole", { size: mb(progSize()) }), t("flash.c7.check.opt_progmem_whole_text"), firstInstall)
			: opt("progmem", t("flash.c7.check.opt_progmem", { size: mb(progSize()) }), text("progmem"), firstInstall),
		hasCal() ? opt("cal", t("flash.c7.check.opt_cal"), t(calInProgmem ? "flash.c7.check.opt_cal_copies_text" : "flash.c7.check.opt_cal_text"), want.q128) : null,
		opt("q128", t("flash.c7.check.opt_q128", { size: mb(dataSize()) }), text("q128")), noProgmem);
	if (box.cal) box.cal.checked = want.cal || want.q128;
	showNote();
	refresh();
	el.refresh = refresh;
	return el;
}

// What the install's first step reads unless the user changes it: on a first install the program memory (required) and
// the calibration, on a RadioFoundry update only what it needs (the codec slot) and the calibration. All channels and
// settings only when asked: the whole data memory at 115200 baud can take many minutes.
export const backupChoices = (firstInstall, cal) => ({ progmem: firstInstall, cal, q128: false });

const dataSize = () => session().stub?.size?.[dataMem()] || MEM[dataMem()].size;	// a whole data memory read (Stub.dump)

// How long reading bytes of a memory takes (c7.readSeconds): from the radio's own count of its blank sectors once
// there is one, the cable's rate measured on this radio once something was read, else the board's stated rate or the
// worst case (every sector used)
const estimateFor = (mem, bytes) => c7.readSeconds(bytes, { sizing: session().size?.[mem], rate: session().rate,
	plainRate: mem === "progmem" ? flash().read_rate : null });
const secondsFor = (mem, bytes) => estimateFor(mem, bytes).seconds;

// A range where packing makes a minute or more of difference: how much of a memory is used is counted, how well it
// packs is not (a full contacts list may take three times as long as the short end)
function readTime(mem, bytes) {
	const { seconds: s, least, upTo } = estimateFor(mem, bytes);
	if (upTo) return t("flash.c7.time_upto_min", { n: Math.max(1, Math.ceil(s / 60)) });
	const lo = Math.max(1, Math.round((least ?? s) / 60)), hi = Math.round(s / 60);
	if (s - (least ?? s) >= 60 && lo < hi) return t("flash.c7.time_range_min", { lo, hi });
	return s < 60 ? t("flash.c7.time_s", { n: Math.max(5, Math.round(s / 5) * 5) }) : t("flash.c7.time_min", { n: Math.round(s / 60) });
}

// Count the blank sectors of the memories the choices read, once per connection, so their times are this radio's
// (a few seconds; capped by c7.blankCount's deadline). Nothing else may use the stub meanwhile: quiet() ends it first.
function sizeUp(c) {
	if (!c.stub || viaIap()) return Promise.resolve();
	c.stopSizing = false;
	return (c.sizing ||= (async () => {
		c.size ||= {};
		const stop = () => c.stopSizing;
		try {
			c.size.progmem ||= await c7.blankCount(c.stub, "progmem", 0, progSize(), { stop });
			if (!stop()) c.size[dataMem()] ||= await c7.blankCount(c.stub, dataMem(), 0, dataSize(), { stop });
		} catch (e) {
			c.log.add(`counting blank sectors: ${e.message}`);	// the estimates stay the fixed ones
		} finally {
			c.sizing = null;
		}
	})());
}

// Before anything else talks to the stub: the count stops after the window it is on
async function quiet(c) {
	c.stopSizing = true;
	await c.sizing;
}

export function checkView() {
	const c = session();
	const out = h("div", { class: "flasher" });
	const forward = S().forward;
	// through the factory update mode nothing of the stub runs: the radio is off until the install's own step
	const top = radioTop(S().radio, viaIap() ? { led: "off", screen: "any" } : { led: "wait", screen: screenNow() });
	const firstInstall = !c.rfoundry;
	const want = (S().c7want ||= backupChoices(firstInstall, hasCal()));
	if (viaIap() && !c.analysis) c.analysis = { firmware: "unknown", slot: null, codec: null };
	const items = {};
	const item = (key, title) => (items[key] = h("li", { "data-state": "" }, h("span", {}, h("b", {}, title), h("span", { class: "small muted" }))));
	const say = (key, state, text = "") => {
		items[key].dataset.state = state;
		items[key].querySelector(".small").textContent = text;
	};
	const checks = h("ul", { class: "c7-checks" }, item("progmem", t("flash.c7.check.progmem")), item("cal", t("flash.c7.check.cal")),
		item("q128", t("flash.c7.check.q128")), item("what", t("flash.c7.check.what")), item("codec", t("flash.c7.check.codec")), item("saved", t("flash.c7.check.saved")));
	const prog = progressBar();
	const status = h("div");
	const options = backupOptions(want, { firstInstall,
		text: (k) => t(k === "q128" ? "flash.c7.check.opt_q128_text" : firstInstall ? "flash.c7.check.opt_progmem_first" : "flash.c7.check.opt_progmem_text") });
	const run = button(t("flash.c7.check.run"), () => go(), { primary: true });
	// A first install may take the codec from the stock file instead of the program memory read: before the reading
	// starts, or by stopping it. The firmware in the radio is then not backed up; the stock file restores it.
	const canSkip = firstInstall && !viaIap() && !!c.stub;
	let reading = null;	// the program memory read's AbortController, while it runs
	const skipTitle = h("b");
	const skip = canSkip ? h("div", { class: "notice action" }, h("p", {}, skipTitle),
		h("p", {}, t("flash.c7.check.skip_text")), button(t("flash.c7.check.skip_button"), () => go({ skipFirmware: true }))) : null;
	// the time the way out saves: the program memory read, less the codec area it still reads
	const refreshSkip = () => {
		const lay = C().boards[boardId()];
		const saved = secondsFor("progmem", progSize()) - secondsFor("progmem", lay.slot[1] - lay.slot[0]);
		skipTitle.textContent = saved < 90 ? t("flash.c7.check.skip_title_s", { n: Math.max(10, Math.round(saved / 10) * 10) })
			: t("flash.c7.check.skip_title", { n: Math.round(saved / 60) });
	};
	if (skip) refreshSkip();
	const stopRead = canSkip ? button(t("flash.c7.check.skip_stop"), () => reading?.abort()) : null;
	if (stopRead) stopRead.hidden = true;
	out.append(panel(t("flash.c7.check.title"), h("div", { class: "c7-grid" },
		h("div", {}, cableBanner(), h("p", {}, t(viaIap() ? "flash.iap.check_lead" : firstInstall ? "flash.c7.check.lead_first" : "flash.c7.check.lead_update")),
			firstInstall && !viaIap() && !S().radio.flash.stock_download ? h("div", { class: "notice action" }, h("p", {}, h("b", {}, t("flash.c7.check.only_copy"))), h("p", {}, t("flash.c7.check.only_copy_text"))) : null,
			skip, options,
			checks, prog.el, stopRead, status), top.el)));
	// before the reading the choices say what happens; once it starts the progress list takes their place
	checks.hidden = prog.el.hidden = true;
	const started = () => {
		checks.hidden = prog.el.hidden = false;
		options.hidden = true;
		if (skip) skip.hidden = true;
	};
	forward(run);
	// this radio's own times for the choices, counted while the user reads them
	if (!c.analysis) sizeUp(c).then(() => { options.refresh(); if (skip) refreshSkip(); });

	async function go({ skipFirmware = false } = {}) {
		run.disabled = true;
		diag.note(...crumbs.c7Check(want, { skipFirmware, firstInstall, how: c.how || null, rate: c.rate ? Math.round(c.rate) : null }));
		started();
		await busy(true);
		top.set({ led: "busy", screen: screenNow() });
		const st = c.stub;
		const lay = C().boards[boardId()];
		try {
			await quiet(c);
			let t0 = performance.now();
			if (want.progmem && !skipFirmware) {
				say("progmem", "run", t("flash.c7.check.reading"));
				reading = canSkip ? new AbortController() : null;
				if (stopRead) stopRead.hidden = false;
				try {
					c.progmem = await st.dump("progmem", 0, progSize(), { signal: reading?.signal, progress: (d, n, s) => {
						prog.set(d, n, t("flash.c7.check.progmem"));
						c.rate = s.wire / Math.max(0.5, (performance.now() - t0) / 1000);
					} });
				} catch (e) {
					if (!reading?.signal.aborted) throw e;
					skipFirmware = true;	// stopped by the user: on to the stock file
					diag.note("firmware read stopped for the stock file");
					c.progmem = null;
					await st.drain();
				} finally {
					reading = null;
					if (stopRead) stopRead.hidden = true;
				}
			}
			if (c.progmem) {
				c.slot = c.progmem.subarray(lay.slot[0], lay.slot[1]);
				await ctx.noteReadout(c.progmem);
				say("progmem", "ok", t("flash.c7.check.progmem_done", { sha: (await sha256(c.progmem)).slice(0, 12) }));
			} else {
				say("progmem", "run", t("flash.c7.check.reading_slot"));
				c.slot = await st.dump("progmem", lay.slot[0], lay.slot[1], { progress: (d, n) => prog.set(d, n, t("flash.c7.check.slot")) });
				say("progmem", "skip", t(skipFirmware ? "flash.c7.check.progmem_stock" : "flash.c7.check.progmem_skipped"));
			}
			if (want.q128) {
				say("q128", "run", t("flash.c7.check.reading"));
				t0 = performance.now();
				c.q128 = await st.dump(dataMem(), 0, dataSize(), { progress: (d, n) => prog.set(d, n, t("flash.c7.check.q128")) });
				say("q128", "ok", t("flash.c7.check.q128_done", { sha: (await sha256(c.q128)).slice(0, 12) }));
			} else say("q128", "skip", t("flash.c7.check.q128_skipped"));
			if (want.cal || want.q128) {
				say("cal", "run", t("flash.c7.check.reading"));
				await readCalibration(c, prog);
				say("cal", c.calFound ? "ok" : "bad", !c.calFound ? t("flash.c7.check.cal_missing")
					: c.calMissing ? t("flash.c7.check.cal_one_copy", { where: t(`flash.c7.cal_where_${c.calMissing}`) }) : t("flash.c7.check.cal_done"));
			} else say("cal", "skip", t("flash.c7.check.q128_skipped"));
			prog.set(1, 1, t("flash.c7.check.done"));
			top.set({ led: "wait", screen: screenNow() });

			// what runs on the radio, and its codec
			const slotInfo = await c7.parseSlot(C(), boardId(), c.slot);
			// without the program memory: RadioFoundry (an update), or a first install whose codec comes from the stock file
			c.analysis = c.progmem ? await c7.analyze(c.progmem, C(), boardId())
				: { firmware: c.rfoundry ? "rfoundry" : "unknown", notRead: !c.rfoundry, slot: slotInfo, codec: null };
			const fw = c.analysis.firmware;
			say("what", c.analysis.notRead ? "skip" : fw === "unknown" ? "bad" : "ok", c.rfoundry ? t("flash.c7.check.is_rfoundry", { git: c.rfoundry.git }) : fwName(c));
			if (slotInfo) {
				c.slotBytes = c.slot.slice(0, slotInfo.length);
				await putBlob(store("slot"), c.slotBytes);
			}
			// redundancy: before anything is written, this browser keeps the program memory and the codec
			if (c.progmem) {
				const kept = await putBlob(store(firstInstall ? "progmem-before-rfoundry" : "progmem"), c.progmem);
				if (c.analysis.codec) await putBlob(store("slot"), c7.slotBytes(C(), boardId(), c.analysis.codec));
				await saveBundle();
				say("saved", "ok", t(kept ? "flash.c7.check.saved_both" : "flash.c7.check.saved_download", { name: c.backupName }));
			} else if (c.q128 || (c.analysis.notRead && c.calCopies)) {
				// what was read is saved even without the program memory: the data memory chosen, or the calibration
				await saveBundle();
				say("saved", "ok", t("flash.c7.check.saved_as", { name: c.backupName }));
			} else say("saved", c.slotBytes ? "ok" : "skip", c.slotBytes ? t("flash.c7.check.saved_slot") : "");
			await decideView();
		} catch (e) {
			say("progmem", items.progmem.dataset.state === "run" ? "bad" : items.progmem.dataset.state);
			fill(status, errorNotice(t("flash.c7.check.failed", { error: ctx.errorText(e) })), retryHelp());
			run.disabled = false;
		} finally {
			await busy(false);
		}
	}

	// The codec: kept, carried over from the stock firmware, taken from this browser or a file, or (if asked) none
	async function decideView() {
		const saved = await getBlob(store("slot"));
		const slotInFlash = c.slotBytes && (await c7.parseSlot(C(), boardId(), c.slotBytes)) ? c.slotBytes : null;
		// a version 1 slot from before the codec moved, on the radio or saved here, rebuilt (c7000v1.js, temporary)
		const movedSlot = slotInFlash ? null : (await upgradeSlot(C(), boardId(), c.slot)) || (saved ? await upgradeSlot(C(), boardId(), saved) : null);
		c.decision = await c7.decide(C(), boardId(), { slotInFlash, movedSlot, savedSlot: saved, flashCodec: c.analysis.codec, fileCodec: c.fileCodec,
			allowNone: S().c7none });
		const d = c.decision;
		diag.note(...crumbs.c7Decision(d, c.analysis, c.fileCodec));
		const text = !d ? t("flash.c7.codec.missing") : d.mode === "keep" ? t("flash.c7.codec.keep")
			: d.mode === "none" ? t("flash.c7.codec.none") : t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
		// a first install that skipped the program memory read expects the codec from the stock file: not a problem yet
		const waiting = !d && c.analysis.notRead;
		say("codec", d ? (d.mode === "none" ? "bad" : "ok") : waiting ? "skip" : "bad", waiting ? t("flash.c7.codec.from_stock_next") : text);
		// any vendor file: a .bin, a readout or the vendor's zip, opened as on every radio's page (vendorfile.js)
		// a file that gives no codec is explained (explain.js), with what each file in the pick is; what was picked goes into
		// error reports (S.vendorPick)
		const pick = filePicker(t("flash.c7.codec.pick"), ".bin,.zip", async (f) => {
			let opened = null;
			try {
				S().vendorPick = { picked: { name: f.name, size: f.data.length, sha256: await sha256(f.data) }, opened: null, problem: null };
				opened = await openVendorFile(f.name, f.data, ctx.fb(), boardId());
				S().vendorPick.opened = opened;
				// the file for a problem report and the trail, as on the other radios' codec step
				S().picked = { name: f.name, data: f.data };
				diag.note(...crumbs.filePicked(S().vendorPick.picked, opened.best || {}));
				const got = opened.best?.match;
				if (got?.kind !== "codec") {
					const lines = describeVendorFile(opened, ctx.fb(), boardId());
					fill(status, ctx.problemNotice(pickProblem(opened, boardId()), { offer: analysisCandidate(opened, ctx.fb(), boardId()) }),
						h("details", { class: "small" }, h("summary", {}, t("flash.codec.stock_report")), h("ul", {}, lines.map((l) => h("li", {}, l)))));
					return;
				}
				c.fileCodec = got.codec;
				S().vendorPick.decision = { mode: "write", source: "file" };
				await decideView();
			} catch (e) {
				// a codec the page identified but can't put in the slot (no map, a check failing): the file is offered
				c.fileCodec = null;
				const used = opened?.best;
				fill(status, ctx.failureNotice(e, used ? { name: used.path.split("/").pop(), data: used.data } : null));
			}
		});
		fill(status, 
			d ? null : h("div", { class: "notice action" }, h("strong", {}, t("flash.c7.codec.action")), h("p", {}, codecActionText()), pick,
				h("p", { class: "small" }, h("a", { href: "#", onclick: async (e) => { e.preventDefault(); S().c7none = true; await decideView(); } }, t("flash.c7.codec.without")))),
			d && d.mode === "none" ? notice(t("flash.c7.codec.none_risk"), "bad") : null,
			c.progmem ? h("p", { class: "small" }, h("a", { href: "#", onclick: (e) => { e.preventDefault(); saveBundle(); } }, t("flash.c7.check.download_again"))) : null,
			h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
		forward(button(t("flash.nav.to_write"), () => ctx.next(), { primary: true, disabled: !d }));
	}

	// Why no codec was found, and what to select instead. A stock radio whose codec is a build without a recipe is
	// named, so the message says that the backup is fine and only the codec needs a file
	function codecActionText() {
		if (viaIap()) return t(`flash.factory.codec_${factoryRoute(S().radio, S().data.fb).needs}`);
		if (c.analysis?.notRead) return t("flash.c7.codec.action_stock");
		if (!stockRadio()) return t("flash.c7.codec.action_text");
		const file = { model: S().radio.model, codec_version: flash().codec_version || "" };
		if (!c.analysis?.otherCodec) return t("flash.usmart.codec.action_text", file);
		// named as flash.stock_versions name it (iapui.js too); the image's own string carries another batch code
		const v = c.progmem && stockVersion(c.progmem);
		return t("flash.usmart.codec.action_other", { ...file, version: v ? stockName(flash(), v.slice(-3)) : t("flash.usmart.codec.version_unknown") });
	}

	// Coming back to this step: what was read is still here
	if (c.analysis) {
		say("progmem", c.progmem ? "ok" : "skip");
		say("q128", c.q128 ? "ok" : "skip");
		say("cal", c.calCopies ? (c.calFound ? "ok" : "bad") : "skip");
		say("what", "ok", c.rfoundry ? t("flash.c7.check.is_rfoundry", { git: c.rfoundry.git }) : fwName(c));
		if (c.backupName) say("saved", "ok", t("flash.c7.check.saved_as", { name: c.backupName }));
		run.disabled = true;
		started();
		prog.el.hidden = true;
		decideView();
	}
	return out;
}

async function saveBundle() {
	const c = session();
	const parts = [];
	if (c.progmem) parts.push({ file: "program-memory.bin", what: "c7000-progmem", addr: C().progmem_base, data: c.progmem, complete: true });
	if (c.q128) parts.push(dataMem() === "nand" ? { file: "nand.bin", what: "c7000-nand", addr: 0, data: c.q128, complete: true }
		: { file: "data-memory.bin", what: "c7000-data", addr: 0, data: c.q128, complete: true });
	else if (c.calArea) parts.push({ file: "original-channels.bin", what: "c7000-data-start", addr: 0, data: c.calArea, complete: true });
	for (const p of c.calCopies || []) parts.push({ ...p, complete: true });
	if (!parts.length) return;
	const fwNow = c.rfoundry ? { kind: "rfoundry", git: c.rfoundry.git, codec: c.rfoundry.codec, features: c.rfoundry.features } : { kind: c.analysis?.firmware === "stock" ? "stock" : "unknown",
		version: c.analysis?.codec ? `with the ${String(c.analysis.codec.profile.tag).padStart(3, "0")} codec` : null };
	const meta = { radio: { board: S().radio.id, name: S().radio.name, model: S().radio.model, chip: null, jedec: c.jedec }, firmware_now: fwNow, contents: parts.map((p) => p.what).join("+"),
		notes: [t(dataMem() === "nand" ? "flash.c7.bundle_note_nand" : "flash.c7.bundle_note", { progmem: `${progSize() / 0x100000} MB`, data: `${dataSize() / 0x100000} MB` })] };
	const bytes = await makeBundle(meta, parts, { git: S().data.site.git, url: location.origin });
	c.backupName = bundleName(meta);
	download(c.backupName, bytes, "application/zip");
	S().lastBackup = { name: c.backupName, data: bytes };
	c.backupSaved = true;
}

// What the check step calls the firmware found. A radio read through its debug console may run a stock version this
// page has no codec recipe for, and one reached only through update mode was not read at all.
function fwName(c) {
	if (c.analysis?.notRead) return t("flash.c7.check.is_not_read");
	if (c.how === "iap" && !c.progmem) return t("flash.iap.check_is");
	if (stockRadio() && c.analysis.firmware === "unknown") return t("flash.usmart.check_is_other");
	return t(`flash.c7.check.is_${c.analysis.firmware}`);
}

function retryHelp() {
	return h("div", { class: "notice info" }, h("p", {}, t("flash.c7.retry_safe")),
		h("p", { class: "small" }, t("flash.c7.retry_how")));
}

// ---------------------------------------------------------------------------------------------------------------
// write

// The program memory as a grid of 4 KB sectors: the bootloader (never written), unchanged, to write, written
function sectorMap(c) {
	const n = c.progmem_size / c.sector;
	const cells = Array.from({ length: n }, (_, i) => h("span", { "data-s": i * c.sector < c.app_start ? "boot" : "" }));
	const slot0 = c.boards[boardId()].slot[0] / c.sector;
	for (let i = slot0; i < n; i++) cells[i].classList.add("slot");
	const legend = (s, label) => h("li", {}, h("span", { class: "c7-map" }, h("span", { "data-s": s })), label);
	return {
		el: h("div", {}, h("div", { class: "c7-map", role: "img", "aria-label": t("flash.c7.write.map_label") }, cells),
			h("ul", { class: "c7-legend" }, legend("boot", t("flash.c7.write.map_boot")), legend("same", t("flash.c7.write.map_same")),
				legend("todo", t("flash.c7.write.map_todo")), legend("written", t("flash.c7.write.map_written")),
				h("li", {}, t("flash.c7.write.map_slot", { addr: hex(C().progmem_base + c.boards[boardId()].slot[0]) })))),
		set(addr, state) { cells[addr / c.sector].dataset.s = state; },
	};
}

export function writeView() {
	if (viaIap()) return routes.iapWriteView();
	const c = session();
	const fw = S().firmware;
	const d = c.decision;
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() });
	const map = sectorMap(C());
	const prog = progressBar();
	const status = h("div");
	let start, data;
	try {
		// an image that starts with a first stage: the application alone would be written over it
		if (flash().first_stage) checkInstallImage(fw.image, flash().first_stage);
		[start, data] = c7.target(C(), boardId(), fw.image, d.mode === "write" ? d.slot : null);
	} catch (e) {
		return panel(t("flash.write.title"), errorNotice(ctx.errorText(e)));
	}
	const what = d.mode === "keep" ? t("flash.c7.codec.keep") : d.mode === "none" ? t("flash.c7.codec.none") : t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
	const go = button(t("flash.c7.write.go"), () => write(), { primary: true });
	const box = panel(t("flash.c7.write.title"), h("div", { class: "c7-grid" },
		h("div", {}, cableBanner(),
			h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.c7.write.firmware")), h("dd", {}, `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`),
				h("dt", {}, t("flash.c7.write.codec")), h("dd", {}, what)),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t("flash.c7.write.careful_text"))),
			prog.el, status),
		top.el), map.el, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
	S().forward(go);

	async function write() {
		go.disabled = true;
		fill(status, );
		await quiet(c);	// a size count still running (backupView) ends before the write uses the stub
		await busy(true);
		top.set({ led: "busy", screen: screenNow() });
		const started = ctx.flashStarted({ task: "install", source: fw.origin || "", radio: boardId(), firmware: String(fw.manifest.version || ""),
			variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null });
		let todo = 0, done = 0;
		try {
			if (!(await c.stub.attach())) throw new StubError(t("flash.c7.write.lost"));
			const res = await c.stub.writeProgram(data, start, { appStart: C().app_start, appEnd: C().app_end, onSector: (a, st) => {
				map.set(a, st);
				if (st === "todo") todo++;
				if (st === "written") { done++; ctx.flashMark(started, "first_byte"); }
				prog.set(done, todo, t("flash.c7.write.writing", { done, todo }));
			} });
			ctx.flashMark(started, "last_byte");
			prog.set(1, 1, t("flash.c7.write.verified"));
			c.log.add(`written ${res.written} sectors, ${res.same} unchanged, all verified by CRC`);
			ctx.flashFinished(started);
			c.result = res;
			await c.stub.ledSequence(DONE_LED);
			top.set({ led: "done", screen: screenNow() });
			fill(status, notice(t("flash.c7.write.ok", { written: res.written, same: res.same }), "ok"));
			await sleep(1200);
			await c.stub.resetChip();
			c.resetAt = Date.now();
			await busy(false);
			ctx.next();
		} catch (e) {
			ctx.flashFinished(started, e);
			await busy(false);
			top.set({ led: "wait", screen: "off" });
			fill(status, errorNotice(t("flash.c7.write.failed", { error: ctx.errorText(e) })), retryHelp(), ctx.helpLink());
			S().forward(button(t("flash.c7.write.again"), async () => {
					if (!(await c.stub.attach())) {
						await dropC7();
						S().step--;	// back to connect: the radio has to be found again
						S().step--;
						ctx.render();
						return;
					}
					write();
				}, { primary: true }));
			go.disabled = false;
		}
	}
	return box;
}

// ---------------------------------------------------------------------------------------------------------------
// done: the radio restarts; RadioFoundry on it says what it runs

export function doneView() {
	const c = session();
	const top = radioTop(S().radio, { led: "off", screen: "off" });
	const status = h("div", {}, h("p", { class: "muted c7-big" }, t("flash.c7.done.restarting")));
	const quiet = h("p", { class: "small muted" }, t("report.something_wrong"), " ", reportButton());
	const box = panel(t("flash.done.title"), h("div", { class: "c7-grid" }, h("div", {}, h("p", {}, t("flash.c7.done.lead")), status, quiet), top.el));
	let tries = 0;
	// The user left this step (Start over, Back) while a check still runs: it must not draw into the step that replaced
	// it (as connectPanel's gone())
	let attached = false;
	const left = () => {
		if (box.isConnected) attached = true;
		return S().c7 !== c || typeof S().forward !== "function" || (attached && !box.isConnected);
	};
	const check = async () => {
		if (left()) return;
		S().forward(null);
		top.set({ led: "off", screen: "off" });
		fill(status, h("p", { class: "muted c7-big" }, t("flash.c7.done.restarting")));
		// After the stub's reset RadioFoundry boots, but its host link often stays silent until a power cycle (DM-32UV every
		// time, DM-1801 Pro most times; dm32-bringup.md open items). So the first check waits only a little past the
		// boot and then asks for the power cycle; a check after one allows for the boot it starts. After the Baofeng
		// updater's end of transfer the radio starts RadioFoundry by itself with its link up (DM-32UV, hardware-dm32.md
		// 13.2), so the first check there waits as long, and the power cycle is only the fallback.
		await sleep(Math.max(0, (c.resetAt || 0) + (S().fakeC7 ? 1500 : BOOT_MS) - Date.now()));
		const restartsItself = viaIap() && flash().update_mode === "baofeng";
		const until = Date.now() + (tries++ || restartsItself ? 20000 : 6000);
		let p = null;
		while (!p && Date.now() < until) {
			if (!c.stream) break;
			p = await probe(new Link(c.stream, { timeout: 1000 }));
		}
		if (left()) return;
		if (!p) {
			diag.note("verify: no answer", { tries });
			top.set({ led: "off", screen: "rfoundry", knob: "cycle" });
			fill(status, notice(t("flash.c7.done.no_answer"), tries > 1 ? "bad" : ""),
				S().fakeC7 ? h("div", { class: "actions" }, button(t("flash.sim.c7_off"), () => S().fakeC7.powerOff()),
					button(t("flash.sim.c7_on"), () => S().fakeC7.powerOn())) : null,
				tries > 1 ? ctx.helpLink() : null, tries > 1 ? reportLine() : null);
			return S().forward(button(t("flash.c7.done.check"), check, { primary: true }));
		}
		const wantCodec = c.decision?.mode !== "none";
		const good = !wantCodec || (p.codec && !["absent", "none"].includes(p.codec));
		diag.note(...crumbs.verify(p, good));
		S().reportResult = good ? "problem" : "failed";
		top.set({ led: "off", screen: "rfoundry" });
		fill(status, 
			h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git)),
			notice(good ? t("flash.c7.done.ready") : t("flash.c7.done.no_codec"), good ? "ok" : "bad"), good ? null : ctx.helpLink(), good ? null : reportLine(),
			// a backup taken from a radio that ran RadioFoundry holds that RadioFoundry build, not the original firmware
			c.backupSaved ? h("p", { class: "small muted" }, t(c.rfoundry ? "flash.c7.done.backup_kept_rfoundry" : "flash.c7.done.backup_kept")) : null,
			// not asked of the radio: any STUB command parks RadioFoundry's codec until the next restart (platform stub_region, core/stubentry)
			h("p", { class: "small muted" }, t(hasStubEntry() ? "flash.c7.done.next_time" : "flash.c7.done.next_time_power")));
		S().forward(button(t("flash.nav.finish"), () => ctx.reset(), { primary: true }));
		await dropC7();
	};
	check();
	return box;
}

// Whether the installed build hands over to the loader by itself (CONFIG_STUB_ENTRY), from its manifest or selection
function hasStubEntry() {
	const f = S().firmware;
	const list = String(f?.manifest?.features || "").split(/[ ,]+/);
	return list.includes("STUB_ENTRY") || !!S().selection?.on.has("STUB_ENTRY");
}

// ---------------------------------------------------------------------------------------------------------------
// backup task: both memories, any firmware, then back to normal

export function backupView() {
	const c = session();
	const out = h("div", { class: "flasher" });
	if (!c.stub) {
		out.append(connectPanel(() => ctx.render()));
		return out;
	}
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() });
	const prog = progressBar();
	const status = h("div");
	const want = { progmem: true, cal: hasCal(), q128: true };
	const options = backupOptions(want);
	const go = button(t("flash.backup.run"), async () => {
		go.disabled = true;
		await busy(true);
		top.set({ led: "busy", screen: screenNow() });
		S().readout = null;	// this use's own readout, not an earlier one's
		await quiet(c);
		const started = ctx.flashStarted({ task: "backup", source: Object.keys(want).filter((k) => want[k]).join("+"), radio: boardId(), firmware: c.rfoundry?.git || "", features: c.rfoundry?.features || "" });
		try {
			if (want.progmem) c.progmem = await c.stub.dump("progmem", 0, progSize(), { progress: (d, n) => prog.set(d, n, t("flash.c7.check.progmem")) });
			if (want.progmem) await ctx.noteReadout(c.progmem);
			if (want.q128) c.q128 = await c.stub.dump(dataMem(), 0, dataSize(), { progress: (d, n) => prog.set(d, n, t("flash.c7.check.q128")) });
			if (want.cal || want.q128) await readCalibration(c, prog);
			if (c.progmem) c.analysis = await c7.analyze(c.progmem, C(), boardId());
			await saveBundle();
			ctx.flashFinished(started);
			await c.stub.ledSequence(DONE_LED);
			top.set({ led: "done", screen: screenNow() });
			fill(status, notice(t("flash.c7.backup.done", { name: c.backupName }), "ok"),
				c.calCopies && !c.calFound ? notice(t("flash.c7.backup.cal_missing"), "bad") : calOneCopy(c));
			S().forward(leaveLoaderButton(t("flash.c7.backup.leave")));
		} catch (e) {
			ctx.flashFinished(started, e);
			fill(status, errorNotice(ctx.errorText(e)), retryHelp());
			go.disabled = false;
		} finally {
			await busy(false);
		}
	}, { primary: true });
	out.append(panel(t("flash.backup.task_title", { name: S().radio.name }), h("div", { class: "c7-grid" },
		h("div", {}, cableBanner(), h("p", {}, t("flash.c7.backup.lead")), options, prog.el, status), top.el)));
	S().forward(go);
	sizeUp(c).then(() => options.refresh());
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// restore task: the program memory from a backup (the application area and the codec slot; never the bootloader)

export function hasC7Backup(bundle) {
	return !!bundle?.parts.get("c7000-progmem");
}

// The radio's original firmware file (one the page identified for it: vendorfile.js kind "codec"), or a program
// memory backup, as a restore bundle: the restore puts it back as it puts back a backup's program memory, so a radio
// returns to its original firmware without the backup from before the first installation (c7.progmemFromFile)
export function vendorBundle(radio, file, c) {
	const name = file.path.split("/").pop();
	// only a file a profile of this radio names: a C-SKY codec signature alone tells no radio
	if (file.match.kind !== "codec" || file.match.board !== radio.flash.board)
		throw new c7.CodecError(`${name} is not identified as the ${radio.name}'s original firmware`, "unmapped");
	const data = c7.progmemFromFile(c, radio.flash.board, file.data, file.match.codec?.profile?.stock_header);
	return { meta: { radio: { board: radio.id, name: radio.name }, created: new Date().toISOString(), vendor_file: name,
		firmware_now: { kind: "stock", version: file.match.known || null } },
	parts: new Map([["c7000-progmem", { what: "c7000-progmem", file: name, complete: true, addr: 0, data }]]) };
}

export function restoreView() {
	const c = session();
	const out = h("div", { class: "flasher" });
	const meta = S().bundle.meta;
	// what the file holds, before and while connecting
	const vendor = meta.vendor_file || null;	// the original firmware from its file (vendorBundle), not a backup
	out.append(panel(t("flash.restore.what"), h("dl", { class: "kv-list" },
		h("dt", {}, t("flash.now.board")), h("dd", {}, meta.radio.name),
		vendor ? [h("dt", {}, t("flash.restore.vendor_file")), h("dd", {}, vendor)] : [h("dt", {}, t("flash.restore.taken")), h("dd", {}, localTime(meta.created))],
		h("dt", {}, t("flash.restore.fw_now")), h("dd", {}, describeFirmware(meta.firmware_now)),
		h("dt", {}, t("flash.restore.parts")), h("dd", {}, partNames(S().bundle.parts)))));
	// what the page writes back is the program memory: a backup of the calibration or the data memory alone is kept,
	// not written (restore.data_note)
	const pm = S().bundle.parts.get("c7000-progmem")?.data;
	if (!pm) {
		out.append(notice(t("flash.c7.restore.no_progmem"), "bad"));
		S().forward(null);
		return out;
	}
	// a radio that answers nothing else gets the backup through the vendor update mode
	if (c.how === "iap" && c.stream) {
		const after = h("div");
		out.append(routes.iapRestoreView(pm, vendor, async () => {
			await dropC7();
			c.how = null;
			fill(after, notice(t("flash.iap.restored_text"), "ok"));
			S().forward(button(t("flash.nav.finish"), () => ctx.reset(), { primary: true }));
		}), after);
		return out;
	}
	if (!c.stub) {
		out.append(connectPanel(() => ctx.render(), { onUpdateMode: () => ctx.render() }));
		return out;
	}
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() });
	const map = sectorMap(C());
	const prog = progressBar();
	const status = h("div");
	const go = button(t("flash.c7.restore.go"), async () => {
		go.disabled = true;
		await quiet(c);	// a size count still running (backupView) ends before the restore uses the stub
		await busy(true);
		top.set({ led: "busy", screen: "off" });
		const started = ctx.flashStarted({ task: "restore", source: vendor ? "stock" : "c7000-progmem", radio: boardId(), firmware: S().bundle.meta.firmware_now?.kind || "" });
		try {
			const model = c7.bootloaderModel(C(), pm);
			if (model && model !== boardId()) throw new Error(t("flash.c7.restore.other_radio"));
			const [start, data] = c7.restoreTarget(C(), pm);
			let todo = 0, done = 0;
			// carve: a protected sector keeps what the radio has, whatever the backup holds
			const res = await c.stub.writeProgram(data, start, { appStart: C().app_start, appEnd: C().app_end, carve: true, onSector: (a, st) => {
				map.set(a, st === "kept" ? "boot" : st);
				if (st === "todo") todo++;
				if (st === "written") done++;
				prog.set(done, todo, t("flash.c7.write.writing", { done, todo }));
			} });
			prog.set(1, 1, t("flash.c7.write.verified"));
			ctx.flashFinished(started);
			await c.stub.ledSequence(DONE_LED);
			top.set({ led: "done", screen: "off" });
			await sleep(1200);
			await c.stub.resetChip();
			await dropC7();
			fill(status, notice(t("flash.c7.restore.done", { written: res.written, same: res.same }), "ok"),
				S().bundle.parts.get("c7000-data") ? notice(t("flash.c7.restore.data_note"), "info") : null);
		} catch (e) {
			ctx.flashFinished(started, e);
			fill(status, errorNotice(ctx.errorText(e)), retryHelp());
			go.disabled = false;
		} finally {
			await busy(false);
		}
	}, { primary: true });
	out.append(panel(t("flash.c7.restore.title"), h("div", { class: "c7-grid" },
		h("div", {}, cableBanner(), h("p", {}, vendor ? t("flash.c7.restore.lead_vendor", { file: vendor }) : t("flash.c7.restore.lead")), prog.el, status), top.el), map.el));
	S().forward(go);
	return out;
}

// A merged image (application + codec slot, tools/hrc7000_codec.py output) split into the two, so the page can treat
// the slot like any other codec source. Returns {image, slot} (slot null when the file has none).
export async function splitOwnImage(c, board, data) {
	const off = c.boards[board].slot[0] - c.app_start;
	if (data.length <= off) return { image: data, slot: null };
	const slot = data.subarray(off);
	const info = await c7.parseSlot(c, board, slot);
	let end = off;
	while (end > 0 && data[end - 1] === 0xFF) end--;
	return { image: data.slice(0, Math.ceil(end / 4) * 4), slot: info ? slot.slice(0, info.length) : null };
}

// The backend (backends.js): connect and check take the place of "now" and "codec"; a reinstall from the help task
// skips the radio step. Stock firmware comes back through the restore task: from a backup, or from the radio's
// original firmware file (vendorBundle).
const INSTALL = ["radio", "build", "connect", "check", "write", "done"], RECOVER = ["build", "connect", "check", "write", "done"];
register({
	id: "c7000-stub",
	available: (radio, fb) => isC7000(radio, fb),		// install and backup, stock firmware or RadioFoundry
	steps: (task, { recovering }) => (task === "install" ? (recovering ? RECOVER : INSTALL) : null),
	stepLabel: (step, task) => (task === "install" && step === "done" ? "c7_done" : step),
	views: { connect: connectView, check: checkView, write: writeView, done: doneView, backup: backupView, restore: restoreView },
	recovery: { first: "fix", stock: "restore", reach: "flash.help.c7_reach", stockText: "flash.help.fix.stock_c7",
		// the factory update mode: a reinstall that needs nothing to answer on the cable but the radio's own loader. stuck:
		// a radio without a complete application stays in it (white screen, red LED), a symptom of its own
		extra: (radio, fb) => {
			const route = factoryRoute(radio, fb);
			return route ? [{ id: "updatemode", title: "flash.help.fix.iap", stuck: route.stuck,
				text: route.stuck ? "flash.help.fix.iap_text" : `flash.help.fix.factory_text_${route.needs}`, prepare: () => { session().forceIap = true; } }] : [];
		} },
	ownsBundle: hasC7Backup,
	vendorRestore: (radio, file, fb) => vendorBundle(radio, file, fb.c7000),
	ownImage: async (fb, board, data) => {
		const { image, slot } = await splitOwnImage(fb.c7000, board, data);
		if (slot) session().fileCodec = slot;
		return image;
	},
	release: dropC7,
});
