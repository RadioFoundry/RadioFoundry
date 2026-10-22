// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flasher's steps for the HR_C7000 radios (DM-32UV, DM-1801 Pro), which install through the C7 stub over the
// programming cable instead of a bootloader (stub.js, c7000.js):
//
//   connect   choose the cable; the page finds what answers: a stub left running, ODR (which hands over to the stub, no
//             buttons), or nothing, and then asks for the radio to be switched off and on to meet its boot ROM
//   check     back up the program memory (and the data memory), find out what the radio runs, decide the codec
//   write     write only the sectors that differ, each read back by CRC; the first sector last
//   done      restart the radio and ask ODR on it for its version and codec
//
// Redundancy, in order: the boot ROM is in the chip and the bootloader is never written, so the radio can always be
// loaded again; before a first install the whole program memory is read, kept in this browser and saved as a
// download; the codec slot is kept in this browser before anything is written; every sector is checked by CRC after
// it is written, and a second pass checks them all; a failed or interrupted install is resumed by running it again
// (only what still differs is written).

import { h, t, notice, button, panel, progressBar, logBox, download, filePicker, localTime, partNames } from "./dom.js";
import { hex, sha256, sleep } from "./bytes.js";
import { SerialStream, Link, probe, serialSupported } from "./link.js";
import { Stub, stubImages, StubError, DONE_LED, MEM, patchUniv, boardLed } from "./stub.js";
import * as c7 from "./c7000.js";
import { upgradeSlot } from "./c7000v1.js";
import { radioTop } from "./radiotop.js";
import { putBlob, getBlob } from "./codecsave.js";
import { makeBundle, describeFirmware, bundleName } from "./backup.js";
import { register } from "./backends.js";
import { makeDb25d } from "./db25dui.js";
import { Usmart, Iap, checkInstallImage, stockVersion } from "./db25d.js";

// USB-serial chips found in programming cables (tools/dm32_flash.py USB_SERIAL), for the port chooser
const CABLES = [0x0403, 0x1A86, 0x10C4, 0x067B].map((usbVendorId) => ({ usbVendorId }));
const CH340 = 0x1A86;
const ROM_WAIT = 180000;		// how long the page listens for the boot ROM per try
const BOOT_MS = 9000;			// a soft reset to ODR answering its debug link (bench 2026-10-15: 8-9 s)

let ctx = null;				// from app.js: {S, next, render, fb, flashStarted, flashFinished, errorText, stubJson, installSecond}
const S = () => ctx.S;
const C = () => ctx.fb().c7000;
const boardId = () => S().radio.flash.board;
const store = (what) => `c7000-${what}-${boardId()}`;

// What the DB25-D adds (db25dui.js): the stub started through the original firmware's debug console, and the vendor
// update mode as the way back when nothing else answers
let db25d = null;
const hasData = () => C().boards[boardId()].data_flash !== false;	// the SPI data flash the stub reads (the DB25-D has none)
const stockRadio = () => !!C().boards[boardId()].stock_console;
// whether the chip the cable reaches has a status LED the stub drives (255, 255: none, the GD-88's main SoC)
// the radio's chip on the cable has no LED: both stub LED pins are "none" (the GD-88's A)
const noLed = (radio, fbd) => !(fbd.c7000?.boards[radio.flash.board]?.stub_board?.leds || [0]).some((p) => p !== 255);
const hasLed = () => (C().boards[boardId()].stub_board?.leds || [0]).some((p) => p !== 255);
// A second SoC with no cable of its own (the GD-88's receiver B): reached through ODR on the first, whose board the
// radio entry names (flash.via)
const bridged = () => !!C().boards[boardId()].stub_board?.bridge;
const answersAs = () => (bridged() ? ctx.fb().boards[S().radio.flash.via] : S().board).debuglink_names;
const BRIDGE_ROM_WAIT = 70000;		// the second SoC's first stage offers for about a minute
const BRIDGE_END_MS = 10000;		// the first SoC ends the bridge this long after the page's last byte, then restarts
const progSize = () => c7.totalSize(C(), boardId());
const viaIap = () => session().how === "iap";
// What the radio's screen shows while the stub runs: ODR's loader screen, whatever the original firmware last drew, or
// nothing after a boot ROM load
const screenNow = () => ({ odr: "loader", usmart: "any" })[session().how] || "off";

export function init(context) {
	ctx = context;
	db25d = makeDb25d({ S, ctx, C, boardId, session, busy, fill: (el, ...p) => fill(el, ...p), stubImage: async () => (await stubs()).image });
}

// Radios the page flashes this way
export const isC7000 = (radio, fb) => radio?.flash.backend === "c7000-stub" && !!fb.c7000?.boards[radio.flash.board];

// The universal stub image, loaded once (data/c7000-stub.json), with this board's pins in its parameter block
let images = null;
async function stubs() {
	if (!images) images = stubImages(await ctx.stubJson());
	return { ...images, image: patchUniv(images.image, C().boards[boardId()].stub_board) };
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
	return (S().c7 ||= { stream: null, stub: null, how: null, odr: null, progmem: null, q128: null, slot: null, analysis: null,
		decision: null, fileCodec: null, backupSaved: false, rate: null, log: logBox() });
}

export async function dropC7() {
	const c = S().c7;
	if (!c?.stream) return;
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
	c.stream = await SerialStream.open(port);
	return c.stream;
}

// replaceChildren() would print a null part as "null"
const fill = (el, ...parts) => el.replaceChildren(...parts.filter(Boolean));

const mkStub = () => {
	const c = session();
	c.stub = new Stub(c.stream, { log: (l) => c.log.add(l) });
	c.stub.size.progmem = progSize();
	return c.stub;
};

// ---------------------------------------------------------------------------------------------------------------
// connect: the cable, then whatever answers; ends with the stub running and the radio identified

// onUpdateMode: what the task does with a DB25-D reached through the vendor update mode (null: the task can't use it)
function connectPanel(onReady, { onUpdateMode = null } = {}) {
	const c = session();
	const radio = S().radio;
	const top = radioTop(radio, { led: "off", screen: "any" }, { hasLed: hasLed() });
	const body = h("div");
	const box = panel(t("flash.c7.connect.title", { name: radio.name }), h("div", { class: "c7-grid" }, body, top.el));
	let abort = null;
	// each state shows its parts and, through act(), its one action in the nav bar
	const show = (state, ...parts) => {
		top.set(state);
		fill(body, ...parts);
		S().forward(null);
	};
	const act = (...all) => {
		const btns = all.filter(Boolean);
		S().forward(btns.length > 1 ? btns : btns[0] || null);
	};

	const cableStep = (err = null) => {
		show({ led: "off", screen: "any", note: t(bridged() ? "flash.gd88.bridge.cable" : "flash.c7.visual.cable") },
			h("ol", { class: "steps c7-big" },
				h("li", {}, t("flash.c7.connect.step_cable")),
				h("li", {}, t("flash.c7.connect.step_choose"))),
			err ? notice(err, "bad") : null,
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
			// a radio already in update mode printed its banner at power-on: remember that, so the install doesn't wait for it
			c.iapReady = !!(await new Iap(c.stream, { log: (l) => c.log.add(l) }).probe());
			return toIap();
		}
		await find();
	};

	// a stub from an earlier run, then ODR, then the boot ROM
	const find = async () => {
		show({ led: "off", screen: "any", note: t(bridged() ? "flash.gd88.bridge.cable" : "flash.c7.visual.cable") }, h("p", { class: "muted c7-big" }, t("flash.c7.connect.looking")));
		const stub = mkStub();
		if (await stub.attach()) {
			c.how = "attach";
			return ready(t("flash.c7.connect.found_stub"));
		}
		const link = new Link(c.stream, { timeout: 1200 });
		const p = await probe(link);
		if (p) {
			c.odr = p;
			if (!answersAs().includes(p.board))
				return cableStep(t("flash.err.wrong_board", { board: p.board, name: S().board.name }));
			if (bridged()) return throughFirst(link);
			const offer = await Stub.odrOffer(link);
			if (offer) return fromOdr(offer);
			return restartOdr();
		}
		if (bridged()) return cableStep(t("flash.gd88.bridge.need_odr"));
		if (stockRadio()) return stockOrNothing();
		return powerCycle();
	};

	// The second SoC: ODR on the first hands the cable over, then the second's first stage takes the stub as the boot
	// ROM would. No power cycle: the first SoC stays on and restarts by itself afterwards
	const throughFirst = async (link) => {
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.odr_found", { git: c.odr.git }))),
			h("p", {}, t("flash.gd88.bridge.asking")));
		const stub = mkStub();
		// what answered is the first chip; what the second one runs is only known from its own flash, so it is read and
		// backed up as on a first install
		c.via = c.odr;
		c.odr = null;
		try {
			await Stub.bridgeFromOdr(link);
			await stub.waitForRom(S().fakeC7 ? 3000 : BRIDGE_ROM_WAIT);
		} catch (e) {
			show({ led: "off", screen: "on" }, notice(t("flash.gd88.bridge.failed", { error: ctx.errorText(e) }), "bad"));
			return act(button(t("flash.c7.connect.try_again"), () => find(), { primary: true }));
		}
		await load(stub, "bridge");
	};

	// A DB25-D on the cable that is not ODR: the original firmware answers its debug console (the stub goes in through
	// it), or nothing answers (update mode, or the first stage's power-on window for an ODR that does not answer)
	const toIap = onUpdateMode && (() => {
		c.how = "iap";
		onUpdateMode();
	});
	const stockOrNothing = async () => {
		if (await new Usmart(c.stream, { log: (l) => c.log.add(l), board: boardId() }).probe()) {
			return db25d.stockFound({ show, act, onLoaded: fromStock, onAgain: () => find(), onUpdateMode: toIap });
		}
		// the loader's update mode answers a read-only probe: a radio whose application won't start stays in it
		c.iapReady = false;
		const um = await new Iap(c.stream, { log: (l) => c.log.add(l) }).probe();
		if (um) {
			c.iapReady = true;
			return db25d.alreadyUpdateMode({ show, act, onUpdateMode: toIap, onAgain: () => find() }, um.version);
		}
		db25d.nothing({ show, act, onAgain: () => find(), onPowerOn: () => powerCycle(), onUpdateMode: toIap });
	};

	// The console started the stub: meet it as ODR's hand-over does
	const fromStock = async () => {
		const stub = mkStub();
		try {
			await sleep(300);
			await stub.drain(300);
			await stub.helloWithin(3000, t("flash.db25d.stock.no_answer"));
		} catch (e) {
			return db25d.stubSilent({ show, act, onAgain: () => find(), onUpdateMode: toIap }, ctx.errorText(e));
		}
		c.how = "usmart";
		await ready(t("flash.db25d.stock.found_stub"));
	};

	const fromOdr = async (offer) => {
		const prog = progressBar();
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.odr_found", { git: c.odr.git }))),
			h("p", {}, t("flash.c7.connect.odr_loading")), prog.el);
		try {
			await mkStub().enterFromOdr((await stubs()).image, (d, n) => prog.set(d, n, t("flash.c7.connect.loader")));
			c.how = "odr";
			await ready(t("flash.c7.connect.found_odr_stub"));
		} catch (e) {
			show({ led: "off", screen: "on" }, notice(t("flash.c7.connect.odr_failed", { error: ctx.errorText(e) }), "bad"));
			act(button(t("flash.c7.connect.power_instead"), () => powerCycle(), { primary: true }));
		}
	};

	// ODR without the stub hand-over: its CPS reboot (C 0, C 6 1) is a soft reset, and a soft reset runs the boot ROM,
	// so the page can meet the ROM without a power cycle. Falls back to the power cycle if the ROM doesn't show.
	const restartOdr = async () => {
		show({ led: "off", screen: "on" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.odr_found", { git: c.odr.git }))),
			h("p", {}, t("flash.c7.connect.odr_restart")));
		const stub = mkStub();
		const waiting = stub.waitForRom(5000);
		await c.stream.write(new Uint8Array([0x43, 0x00]));
		await sleep(100);
		await c.stream.write(new Uint8Array([0x43, 0x06, 0x01]));
		try {
			await waiting;
			await load(stub);
		} catch (e) {
			powerCycle(t("flash.c7.connect.odr_restart_failed"));
		}
	};

	// Wait for the boot ROM while the user switches the radio off and on
	const powerCycle = async (why = null) => {
		abort?.abort();
		const ac = (abort = new AbortController());
		const t0 = Date.now();
		const hints = h("div");
		const clock = h("span", { class: "muted" });
		const tick = setInterval(() => {
			if (ac.signal.aborted) return clearInterval(tick);
			const s = Math.round((Date.now() - t0) / 1000);
			clock.textContent = t("flash.c7.connect.listening", { s });
			if (s === 25) hints.replaceChildren(powerHints());
		}, 1000);
		const sim = S().fakeC7 ? h("div", { class: "actions" },
			button(t("flash.sim.c7_off"), () => S().fakeC7.powerOff()), button(t("flash.sim.c7_on"), () => S().fakeC7.powerOn(), { primary: true })) : null;
		show({ led: "off", screen: "off", knob: "cycle" },
			why ? notice(why, "info") : null,
			h("ol", { class: "steps c7-big" },
				h("li", {}, t("flash.c7.connect.step_off")),
				h("li", {}, t("flash.c7.connect.step_on"))),
			h("p", {}, t(hasLed() ? "flash.c7.connect.no_keys" : "flash.c7.connect.no_keys_noled")), sim, h("p", { class: "small" }, clock), hints);
		act(button(t("flash.c7.connect.cancel"), () => { ac.abort(); clearInterval(tick); cableStep(); }, { quiet: true }));
		const stub = mkStub();
		try {
			await stub.waitForRom(ROM_WAIT, { signal: ac.signal });
			clearInterval(tick);
			await load(stub);
		} catch (e) {
			clearInterval(tick);
			if (ac.signal.aborted && e.message === "stopped") return;
			show({ led: "off", screen: "off" }, notice(t("flash.c7.connect.no_rom"), "bad"), powerHints());
			act(button(t("flash.c7.connect.try_again"), () => powerCycle(), { primary: true }));
		}
	};

	const powerHints = () => {
		const ch340 = c.portInfo?.usbVendorId === CH340;
		return h("div", { class: "notice info" }, h("p", {}, h("b", {}, t("flash.c7.connect.hints_title"))),
			h("ul", {}, h("li", {}, t("flash.c7.connect.hint_order")), h("li", {}, t("flash.c7.connect.hint_plug")),
				h("li", {}, t("flash.c7.connect.hint_battery")), ch340 ? h("li", {}, t("flash.c7.connect.hint_ch340")) : null));
	};

	const load = async (stub, how = "rom") => {
		const prog = progressBar();
		show({ led: "off", screen: "off" }, h("p", { class: "c7-big" }, h("b", {}, t("flash.c7.connect.rom_found"))), prog.el);
		try {
			// one image for every radio, at the boot ROM's address (the DB25-D's first stage takes it there too)
			const st = await stubs();
			await stub.loadThroughRom(st.image, st.load, (d, n) => prog.set(d, n, t("flash.c7.connect.loader")));
			c.how = how;
			await ready(t(how === "bridge" ? "flash.gd88.bridge.found" : "flash.c7.connect.found_rom"));
		} catch (e) {
			show({ led: "off", screen: "off" }, notice(t("flash.c7.connect.load_failed", { error: ctx.errorText(e) }), "bad"));
			act(button(t("flash.c7.connect.try_again"), () => (bridged() ? find() : powerCycle()), { primary: true }));
		}
	};

	// The stub runs: the LED, both chips, which radio this is
	const ready = async (how) => {
		const st = c.stub;
		const screen = screenNow();
		show({ led: "wait", screen }, h("p", { class: "c7-big" }, h("b", {}, how)), h("p", { class: "muted" }, t("flash.c7.connect.checking")));
		try {
			const led = boardLed(C(), boardId());
			await st.configure(C().boards[boardId()].stub_board, led);	// the radio's profile, checked before any write
			await st.setupLed(led.red, led.green);
			c.jedec = { progmem: await st.checkId("progmem"), q128: hasData() ? await st.checkId("q128") : null };
			// ODR named the radio already; otherwise the bootloader's ID string does (48 KB, a few seconds). A radio
			// whose loader has no ID string (the DB25-D) is the one the page was told
			c.model = c.odr || !C().boards[boardId()].bootloader_id ? boardId()
				: c7.bootloaderModel(C(), c.progmem || (await st.dump("progmem", 0, C().app_start)));
			if (c.model && c.model !== boardId()) {
				const other = Object.values(S().data.radios).find((r) => r.flash.board === c.model);
				show({ led: "wait", screen }, notice(t("flash.c7.connect.wrong_radio", { name: S().radio.name, other: other?.name || c.model }), "bad"));
				return act(leaveLoaderButton());
			}
			show({ led: "wait", screen }, h("p", { class: "c7-big" }, h("b", {}, how)),
				h("dl", { class: "kv-list" },
					h("dt", {}, t("flash.c7.radio")), h("dd", {}, c.model ? S().radio.name : t("flash.c7.radio_unconfirmed"))),
				h("p", {}, t(c.how === "usmart" ? "flash.db25d.stock.led"
					: `flash.c7.connect.led_${{ odr: "odr", bridge: "bridge" }[c.how] || "rom"}${hasLed() ? "" : "_noled"}`)));
			await sleep(S().fakeC7 ? 400 : 1500);
			onReady();
		} catch (e) {
			show({ led: "wait", screen }, notice(ctx.errorText(e), "bad"));
			act(button(t("flash.c7.connect.try_again"), () => find(), { primary: true }));
		}
	};

	if (c.stub && c.stream) {
		// already connected (back from a later step): check the stub still answers
		c.stub.attach().then((ok) => (ok ? ready(t("flash.c7.connect.still")) : cableStep()));
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

// The calibration area: from the full data memory read when there is one, else read by itself
async function readCalibration(c, prog) {
	c.calArea = c.q128 ? c.q128.subarray(0, c7.CAL_AREA_END)
		: await c.stub.dump("q128", 0, c7.CAL_AREA_END, { progress: (d, n) => prog.set(d, n, t("flash.c7.check.cal")) });
	c.calAddr = c7.findCalibration(c.calArea);
}

// The backup choices, shared by the install's first step and the backup task. The calibration is part of the data
// memory, so choosing that one includes it.
function backupOptions(want, { firstInstall = false, text = (k) => t(`flash.c7.backup.${k}_text`) } = {}) {
	const box = {};
	const opt = (k, title, why, disabled = false) => h("label", { class: "toggle" },
		box[k] = h("input", { type: "checkbox", checked: want[k], disabled, onchange: (e) => {
			want[k] = e.target.checked;
			if (k === "q128") { box.cal.checked = want.cal || want.q128; box.cal.disabled = want.q128; }
		} }),
		h("span", {}, h("b", {}, title), h("span", { class: "why" }, why)), h("span"));
	if (!hasData()) {
		return h("div", {}, opt("progmem", t("flash.db25d.check.opt_progmem"), [t("flash.db25d.check.opt_progmem_text"), " ", estimate(0x200000, DB25D_READ_RATE)], firstInstall));
	}
	const el = h("div", {},
		opt("progmem", t("flash.c7.check.opt_progmem"), [text("progmem"), " ", estimate(0xA0000)], firstInstall),
		opt("cal", t("flash.c7.check.opt_cal"), [t("flash.c7.check.opt_cal_text"), " ", estimate(0x18000)], want.q128),
		opt("q128", t("flash.c7.check.opt_q128"), [text("q128"), " ", estimate(0x200000)]));
	box.cal.checked = want.cal || want.q128;
	return el;
}

// The DB25-D's 2 MB program memory, read from a radio that runs ODR: about 45 s (radio, 2026-10-17). Blank sectors and
// LZ4 make the memory read faster than the wire's 9 KB/s
const DB25D_READ_RATE = 46000;

function estimate(bytes, fallback = 9000) {
	const rate = session().rate || fallback;	// bytes on the wire per second at 115200 baud, after LZ4 and blank sectors
	const s = bytes / rate;
	return s < 60 ? t("flash.c7.time_s", { n: Math.max(5, Math.round(s / 5) * 5) }) : t("flash.c7.time_min", { n: Math.round(s / 60) });
}

export function checkView() {
	const c = session();
	const out = h("div", { class: "flasher" });
	const forward = S().forward;
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() }, { hasLed: hasLed() });
	const firstInstall = !c.odr;
	// a first install reads everything; an ODR update only what it needs (the codec slot), unless asked
	const want = (S().c7want ||= { progmem: firstInstall, cal: hasData(), q128: firstInstall && hasData() });
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
	out.append(panel(t("flash.c7.check.title"), h("div", { class: "c7-grid" },
		h("div", {}, h("p", {}, t(viaIap() ? "flash.db25d.check.lead_iap" : firstInstall ? "flash.c7.check.lead_first" : "flash.c7.check.lead_update")),
			firstInstall && !viaIap() && !S().radio.flash.stock_download ? h("div", { class: "notice action" }, h("p", {}, h("b", {}, t("flash.c7.check.only_copy"))), h("p", {}, t("flash.c7.check.only_copy_text"))) : null,
			options,
			checks, prog.el, status), top.el)));
	// before the reading the choices say what happens; once it starts the progress list takes their place
	checks.hidden = prog.el.hidden = true;
	const started = () => { checks.hidden = prog.el.hidden = false; options.hidden = true; };
	forward(run);

	async function go() {
		run.disabled = true;
		started();
		await busy(true);
		top.set({ led: "busy", screen: screenNow() });
		const st = c.stub;
		const lay = C().boards[boardId()];
		try {
			let t0 = performance.now();
			if (want.progmem) {
				say("progmem", "run", t("flash.c7.check.reading"));
				c.progmem = await st.dump("progmem", 0, progSize(), { progress: (d, n, s) => {
					prog.set(d, n, t("flash.c7.check.progmem"));
					c.rate = s.wire / Math.max(0.5, (performance.now() - t0) / 1000);
				} });
				c.slot = c.progmem.subarray(lay.slot[0], lay.slot[1]);
				say("progmem", "ok", t("flash.c7.check.progmem_done", { sha: (await sha256(c.progmem)).slice(0, 12) }));
			} else {
				say("progmem", "run", t("flash.c7.check.reading_slot"));
				c.slot = await st.dump("progmem", lay.slot[0], lay.slot[1], { progress: (d, n) => prog.set(d, n, t("flash.c7.check.slot")) });
				say("progmem", "skip", t("flash.c7.check.progmem_skipped"));
			}
			if (want.q128) {
				say("q128", "run", t("flash.c7.check.reading"));
				t0 = performance.now();
				c.q128 = await st.dump("q128", 0, MEM.q128.size, { progress: (d, n) => prog.set(d, n, t("flash.c7.check.q128")) });
				say("q128", "ok", t("flash.c7.check.q128_done", { sha: (await sha256(c.q128)).slice(0, 12) }));
			} else say("q128", "skip", t("flash.c7.check.q128_skipped"));
			if (want.cal || want.q128) {
				say("cal", "run", t("flash.c7.check.reading"));
				await readCalibration(c, prog);
				say("cal", c.calAddr >= 0 ? "ok" : "bad", t(c.calAddr >= 0 ? "flash.c7.check.cal_done" : "flash.c7.check.cal_missing"));
			} else say("cal", "skip", t("flash.c7.check.q128_skipped"));
			prog.set(1, 1, t("flash.c7.check.done"));
			top.set({ led: "wait", screen: screenNow() });

			// what runs on the radio, and its codec
			const slotInfo = await c7.parseSlot(C(), boardId(), c.slot);
			c.analysis = c.progmem ? await c7.analyze(c.progmem, C(), boardId()) : { firmware: "odr", slot: slotInfo, codec: null };
			const fw = c.analysis.firmware;
			say("what", fw === "unknown" ? "bad" : "ok", c.odr ? t("flash.c7.check.is_odr", { git: c.odr.git }) : fw === "odr" ? t("flash.c7.check.is_odr_read") : t(`flash.c7.check.is_${fw}`));
			if (slotInfo) {
				c.slotBytes = c.slot.slice(0, slotInfo.length);
				await putBlob(store("slot"), c.slotBytes);
			}
			// redundancy: before anything is written, this browser keeps the program memory and the codec
			if (c.progmem) {
				const kept = await putBlob(store(firstInstall ? "progmem-before-odr" : "progmem"), c.progmem);
				if (c.analysis.codec) await putBlob(store("slot"), c7.slotBytes(C(), boardId(), c.analysis.codec));
				await saveBundle();
				say("saved", "ok", t(kept ? "flash.c7.check.saved_both" : "flash.c7.check.saved_download", { name: c.backupName }));
			} else say("saved", c.slotBytes ? "ok" : "skip", c.slotBytes ? t("flash.c7.check.saved_slot") : "");
			await decideView();
		} catch (e) {
			say("progmem", items.progmem.dataset.state === "run" ? "bad" : items.progmem.dataset.state);
			fill(status, notice(t("flash.c7.check.failed", { error: ctx.errorText(e) }), "bad"), retryHelp());
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
		const text = !d ? t("flash.c7.codec.missing") : d.mode === "keep" ? t("flash.c7.codec.keep")
			: d.mode === "none" ? t("flash.c7.codec.none") : t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
		say("codec", d ? (d.mode === "none" ? "bad" : "ok") : "bad", text);
		const pick = filePicker(t("flash.c7.codec.pick"), ".bin", async (f) => {
			try {
				c.fileCodec = await c7.codecFromFile(f.data, C(), boardId());
				await decideView();
			} catch (e) {
				fill(status, notice(ctx.errorText(e), "bad"));
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

	// Why no codec was found, and what to select instead. A DB25-D or GD-88 whose codec is a build without a recipe
	// (the DB25-D's 020) is named, so the message says that the backup is fine and only the codec needs a file
	function codecActionText() {
		if (!stockRadio()) return t("flash.c7.codec.action_text");
		const k = CONSOLES[boardId()] || CONSOLES.db25d;
		if (!c.analysis?.otherCodec) return t(`flash.${k.texts}.codec.action_text`);
		// named as the update-mode step names it (db25dui.js); the image's own string carries another batch code
		const v = c.progmem && stockVersion(c.progmem, boardId());
		return t(`flash.${k.texts}.codec.action_other`, { version: v ? `${k.prefix}${v.slice(-3)}` : t("flash.db25d.codec.version_unknown") });
	}

	// Coming back to this step: what was read is still here
	if (c.analysis) {
		say("progmem", c.progmem ? "ok" : "skip");
		say("q128", c.q128 ? "ok" : "skip");
		say("cal", c.calArea ? (c.calAddr >= 0 ? "ok" : "bad") : "skip");
		say("what", "ok", c.odr ? t("flash.c7.check.is_odr", { git: c.odr.git }) : fwName(c));
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
	if (c.q128) parts.push({ file: "data-memory.bin", what: "c7000-data", addr: 0, data: c.q128, complete: true });
	else if (c.calArea) parts.push({ file: "original-channels.bin", what: "c7000-data-start", addr: 0, data: c.calArea, complete: true });
	if (c.calArea && c.calAddr >= 0)
		parts.push({ file: "calibration.bin", what: "c7000-calibration", addr: c.calAddr, data: c.calArea.slice(c.calAddr, c.calAddr + 0x1000), complete: true });
	if (!parts.length) return;
	const fwNow = c.odr ? { kind: "odr", git: c.odr.git, codec: c.odr.codec, features: c.odr.features } : { kind: c.analysis?.firmware === "stock" ? "stock" : "unknown",
		version: c.analysis?.codec ? `with the ${String(c.analysis.codec.profile.tag).padStart(3, "0")} codec` : null };
	const meta = { radio: { board: S().radio.id, name: S().radio.name, model: S().radio.model, chip: null, jedec: c.jedec }, firmware_now: fwNow, contents: parts.map((p) => p.what).join("+"),
		notes: [t(hasData() ? "flash.c7.bundle_note" : "flash.db25d.bundle_note")] };
	const bytes = await makeBundle(meta, parts, { git: S().data.site.git, url: location.origin });
	c.backupName = bundleName(meta);
	download(c.backupName, bytes, "application/zip");
	c.backupSaved = true;
}

// What the check step calls the firmware found. A DB25-D read through its debug console may run a stock version this
// page has no codec recipe for, and one reached only through update mode was not read at all.
function fwName(c) {
	if (c.how === "iap" && !c.progmem) return t("flash.db25d.check.is_update_mode");
	if (stockRadio() && c.analysis.firmware === "unknown") return t("flash.db25d.check.is_stock_other");
	// ODR found in the flash read, with no answer from it to name the build (a radio reached in update mode)
	if (c.analysis.firmware === "odr") return t("flash.c7.check.is_odr_read");
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
	if (viaIap()) return db25d.iapWriteView();
	const c = session();
	const fw = S().firmware;
	const d = c.decision;
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() }, { hasLed: hasLed() });
	const map = sectorMap(C());
	const prog = progressBar();
	const status = h("div");
	let start, data;
	try {
		// the DB25-D's image starts with its first stage at 0xC000; the application alone would be written over it
		if (stockRadio()) checkInstallImage(fw.image);
		[start, data] = c7.target(C(), boardId(), fw.image, d.mode === "write" ? d.slot : null);
	} catch (e) {
		return panel(t("flash.write.title"), notice(ctx.errorText(e), "bad"));
	}
	const what = d.mode === "keep" ? t("flash.c7.codec.keep") : d.mode === "none" ? t("flash.c7.codec.none") : t(`flash.c7.codec.from_${d.source}`, { name: d.name || "", tag: String(d.tag ?? "").padStart(3, "0") });
	const go = button(t("flash.c7.write.go"), () => write(), { primary: true });
	const box = panel(t("flash.c7.write.title"), h("div", { class: "c7-grid" },
		h("div", {},
			h("dl", { class: "kv-list" },
				h("dt", {}, t("flash.c7.write.firmware")), h("dd", {}, `${fw.source} (${Math.round(fw.image.length / 1024)} KB)`),
				h("dt", {}, t("flash.c7.write.codec")), h("dd", {}, what)),
			h("div", { class: "notice" }, h("p", {}, h("b", {}, t("flash.c7.write.careful"))), h("p", {}, t(hasLed() ? "flash.c7.write.careful_text" : "flash.c7.write.careful_text_noled"))),
			prog.el, status),
		top.el), map.el, h("details", { class: "small" }, h("summary", {}, t("flash.c7.log")), c.log.el));
	S().forward(go);

	async function write() {
		go.disabled = true;
		fill(status, );
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
				if (st === "written") done++;
				prog.set(done, todo, t("flash.c7.write.writing", { done, todo }));
			} });
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
			fill(status, notice(t("flash.c7.write.failed", { error: ctx.errorText(e) }), "bad"), retryHelp(), ctx.helpLink());
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
// done: the radio restarts; ODR on it says what it runs

export function doneView() {
	const c = session();
	const top = radioTop(S().radio, { led: "off", screen: "off" }, { hasLed: hasLed() });
	const status = h("div", {}, h("p", { class: "muted c7-big" }, t("flash.c7.done.restarting")));
	const box = panel(t("flash.done.title"), h("div", { class: "c7-grid" }, h("div", {}, h("p", {}, t("flash.c7.done.lead")), status), top.el));
	let tries = 0;
	const check = async () => {
		S().forward(null);
		top.set({ led: "off", screen: "off" });
		fill(status, h("p", { class: "muted c7-big" }, t("flash.c7.done.restarting")));
		// After the stub's reset ODR boots, but its host link often stays silent until a power cycle (DM-32UV every
		// time, DM-1801 Pro most times; dm32-bringup.md open items). So the first check waits only a little past the
		// boot and then asks for the power cycle; a check after one allows for the boot it starts.
		// the second SoC behind the first: the first ends the bridge after a quiet spell and restarts, then reports it
		await sleep(Math.max(0, (c.resetAt || 0) + (S().fakeC7 ? 1500 : BOOT_MS + (bridged() ? BRIDGE_END_MS : 0)) - Date.now()));
		const until = Date.now() + (tries++ ? 20000 : 6000);
		let p = null;
		while (!p && Date.now() < until) {
			if (!c.stream) break;
			p = await probe(new Link(c.stream, { timeout: 1000 }));
		}
		if (!p) {
			top.set({ led: "off", screen: "on", knob: "cycle" });
			fill(status, notice(t("flash.c7.done.no_answer"), tries > 1 ? "bad" : ""),
				S().fakeC7 ? h("div", { class: "actions" }, button(t("flash.sim.c7_off"), () => S().fakeC7.powerOff()),
					button(t("flash.sim.c7_on"), () => S().fakeC7.powerOn())) : null,
				tries > 1 ? ctx.helpLink() : null);
			return S().forward(button(t("flash.c7.done.check"), check, { primary: true }));
		}
		if (bridged()) return secondDone(p);
		const wantCodec = c.decision?.mode !== "none";
		const good = !wantCodec || (p.codec && !["absent", "none"].includes(p.codec));
		top.set({ led: "off", screen: "on" });
		fill(status, 
			h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git)),
			notice(good ? t("flash.c7.done.ready") : t("flash.c7.done.no_codec"), good ? "ok" : "bad"), good ? null : ctx.helpLink(),
			// a backup taken from a radio that ran ODR holds that ODR build, not the original firmware
			c.backupSaved ? h("p", { class: "small muted" }, t(c.odr ? "flash.c7.done.backup_kept_odr" : "flash.c7.done.backup_kept")) : null,
			// not asked of the radio: any STUB command parks ODR's codec until the next restart (debuglink_stub.c)
			h("p", { class: "small muted" }, t(hasStubEntry() ? "flash.c7.done.next_time" : "flash.c7.done.next_time_power")),
			// a radio with a second chip: that one's image goes next, through this one
			S().radio.flash.second_receiver ? notice(t("flash.second.next"), "info") : null);
		S().forward(S().radio.flash.second_receiver
			? [button(t("flash.nav.finish"), () => ctx.reset(), { quiet: true }),
				button(t("flash.second.go"), async () => { await dropC7(); ctx.installSecond(); }, { primary: true })]
			: button(t("flash.nav.finish"), () => ctx.reset(), { primary: true }));
		await dropC7();
	};
	// The second SoC: ODR on the first says whether it answers on the link again
	const secondDone = async (p) => {
		let flags = null;
		for (let i = 0; i < 5 && !(flags & 2); i++) {
			flags = await Stub.secondState(new Link(c.stream, { timeout: 1000 }));
			if (!(flags & 2)) await sleep(S().fakeC7 ? 100 : 1000);
		}
		const good = !!(flags & 2);
		top.set({ led: "off", screen: "on" });
		fill(status, h("dl", { class: "kv-list" }, h("dt", {}, t("flash.now.firmware")), h("dd", {}, p.git)),
			notice(t(good ? "flash.gd88.done.second_up" : "flash.gd88.done.second_down"), good ? "ok" : "bad"), good ? null : ctx.helpLink());
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
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() }, { hasLed: hasLed() });
	const prog = progressBar();
	const status = h("div");
	const want = { progmem: true, cal: hasData(), q128: hasData() };
	const options = backupOptions(want);
	const go = button(t("flash.backup.run"), async () => {
		go.disabled = true;
		await busy(true);
		top.set({ led: "busy", screen: screenNow() });
		const started = ctx.flashStarted({ task: "backup", source: Object.keys(want).filter((k) => want[k]).join("+"), radio: boardId(), firmware: c.odr?.git || "", features: c.odr?.features || "" });
		try {
			if (want.progmem) c.progmem = await c.stub.dump("progmem", 0, progSize(), { progress: (d, n) => prog.set(d, n, t("flash.c7.check.progmem")) });
			if (want.q128) c.q128 = await c.stub.dump("q128", 0, MEM.q128.size, { progress: (d, n) => prog.set(d, n, t("flash.c7.check.q128")) });
			if (want.cal || want.q128) await readCalibration(c, prog);
			if (c.progmem) c.analysis = await c7.analyze(c.progmem, C(), boardId());
			await saveBundle();
			ctx.flashFinished(started);
			await c.stub.ledSequence(DONE_LED);
			top.set({ led: "done", screen: screenNow() });
			fill(status, notice(t("flash.c7.backup.done", { name: c.backupName }), "ok"),
				c.calArea && c.calAddr < 0 ? notice(t("flash.c7.backup.cal_missing"), "bad") : null);
			S().forward(leaveLoaderButton(t("flash.c7.backup.leave")));
		} catch (e) {
			ctx.flashFinished(started, e);
			fill(status, notice(ctx.errorText(e), "bad"), retryHelp());
			go.disabled = false;
		} finally {
			await busy(false);
		}
	}, { primary: true });
	out.append(panel(t("flash.backup.task_title", { name: S().radio.name }), h("div", { class: "c7-grid" },
		h("div", {}, h("p", {}, t("flash.c7.backup.lead")), options, prog.el, status), top.el)));
	S().forward(go);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// restore task: the program memory from a backup (the application area and the codec slot; never the bootloader)

export function hasC7Backup(bundle) {
	return !!bundle?.parts.get("c7000-progmem");
}

export function restoreView() {
	const c = session();
	const out = h("div", { class: "flasher" });
	const meta = S().bundle.meta;
	// what the file holds, before and while connecting
	out.append(panel(t("flash.restore.what"), h("dl", { class: "kv-list" },
		h("dt", {}, t("flash.now.board")), h("dd", {}, meta.radio.name),
		h("dt", {}, t("flash.restore.taken")), h("dd", {}, localTime(meta.created)),
		h("dt", {}, t("flash.restore.fw_now")), h("dd", {}, describeFirmware(meta.firmware_now)),
		h("dt", {}, t("flash.restore.parts")), h("dd", {}, partNames(S().bundle.parts)))));
	const pm = S().bundle.parts.get("c7000-progmem").data;
	// a DB25-D that answers nothing else gets the backup through the vendor update mode
	if (c.how === "iap" && c.stream) {
		const after = h("div");
		out.append(db25d.iapRestoreView(pm, async () => {
			await dropC7();
			c.how = null;
			fill(after, notice(t("flash.db25d.iap.restored_text"), "ok"));
			S().forward(button(t("flash.nav.finish"), () => ctx.reset(), { primary: true }));
		}), after);
		return out;
	}
	if (!c.stub) {
		out.append(connectPanel(() => ctx.render(), { onUpdateMode: () => ctx.render() }));
		return out;
	}
	const top = radioTop(S().radio, { led: "wait", screen: screenNow() }, { hasLed: hasLed() });
	const map = sectorMap(C());
	const prog = progressBar();
	const status = h("div");
	const go = button(t("flash.c7.restore.go"), async () => {
		go.disabled = true;
		await busy(true);
		top.set({ led: "busy", screen: "off" });
		const started = ctx.flashStarted({ task: "restore", source: "c7000-progmem", radio: boardId(), firmware: S().bundle.meta.firmware_now?.kind || "" });
		try {
			const model = c7.bootloaderModel(C(), pm);
			if (model && model !== boardId()) throw new Error(t("flash.c7.restore.other_radio"));
			const [start, data] = c7.restoreTarget(C(), pm);
			let todo = 0, done = 0;
			const res = await c.stub.writeProgram(data, start, { appStart: C().app_start, appEnd: C().app_end, onSector: (a, st) => {
				map.set(a, st);
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
			fill(status, notice(ctx.errorText(e), "bad"), retryHelp());
			go.disabled = false;
		} finally {
			await busy(false);
		}
	}, { primary: true });
	out.append(panel(t("flash.c7.restore.title"), h("div", { class: "c7-grid" },
		h("div", {}, h("p", {}, t("flash.c7.restore.lead")), prog.el, status), top.el), map.el));
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
// skips the radio step. Stock firmware comes back from a backup, never from a stock file.
const INSTALL = ["radio", "build", "connect", "check", "write", "done"], RECOVER = ["build", "connect", "check", "write", "done"];
register({
	id: "c7000-stub",
	available: (radio, fb) => isC7000(radio, fb),		// install and backup, stock firmware or ODR
	steps: (task, { recovering }) => (task === "install" ? (recovering ? RECOVER : INSTALL) : null),
	stepLabel: (step, task) => (task === "install" && step === "done" ? "c7_done" : step),
	views: { connect: connectView, check: checkView, write: writeView, done: doneView, backup: backupView, restore: restoreView },
	recovery: { first: "fix", stock: "restore", reach: "flash.help.c7_reach", stockText: "flash.help.fix.stock_c7",
		// the DB25-D's vendor update mode: a reinstall that needs nothing to answer on the cable
		extra: (radio, fbd) => (fbd.c7000?.boards[radio.flash.board]?.stock_console
			? [{ id: "updatemode", title: "flash.help.fix.iap", text: `flash.help.fix.iap_text${noLed(radio, fbd) ? "_noled" : ""}`,
				prepare: () => { session().forceIap = true; } }] : []),
		// the chip on the cable has no LED (the GD-88's A): the symptom texts that name one have a _noled form
		noLed },
	ownsBundle: hasC7Backup,
	ownImage: async (fb, board, data) => {
		const { image, slot } = await splitOwnImage(fb.c7000, board, data);
		if (slot) session().fileCodec = slot;
		return image;
	},
	release: dropC7,
});
