// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USMART, the debug console a stock firmware answers on the programming cable (a board's radios.json
// flash.stock_console "usmart"; the DB25-D PRO's, docs/spec/hardware-db25d.md 7). It reads and writes single words of
// memory, which is enough to put the universal stub (stub.js) into a free stretch of RAM and start it. The stub then
// reads and writes the program flash as on every other HR_C7000 radio. The console writes RAM only, never flash. The
// stock builds it knows are below (BUILDS), by their versions; a board's flash.stock_versions names the ones it runs.
//
// Once RadioFoundry runs, the radio is reached the way the other HR_C7000 radios are (stub.js, c7000.js).

import { sleep } from "./bytes.js";
import { univFits } from "./stub.js";

export class UsmartError extends Error {}

export const FLASH_BASE = 0x03000000, FLASH_SIZE = 0x200000;
// the stock application region: it starts after the vendor loader and ends where the stock data area starts
const APP_END = 0x0310C000;

// ---------------------------------------------------------------------------------------------------------------
// The protocol (hardware-db25d.md 7, Stub from stock). A command is one frame: "@AT", one filler byte, the text, CR LF.
// The console times every byte at 20 ms, so a frame goes out in a single write; anything unframed is ignored. Replies:
//   read_addr(0x0300c000)        -> "read_addr(0X300C000)=0XC001112C;"
//   write_addr(0x00054000,0x1)   -> "write_addr(0X54000,0X1);" (the function returns nothing; the echo is the reply)
// The stock application's own log text follows on the same line, so a reply is matched by its address.
//
// The stub goes into 0x54000-0x58000, free RAM above the heap in both proven builds (018, 020); the others name their
// own window (BUILDS). The console's command table lives in RAM at 0x18000840; its cmd_rec entry (+8) is called
// through the table for every ordinary frame, so pointing it at the stub makes the next frame start the stub.

const FRAME = "@AT ";
export const STUB_WINDOW = [0x54000, 0x4000];		// [load address, size]
export const HIJACK = 0x18000848;
export const APP_SIG = [0x0300C000, 0xC001112C];		// the application's first word: 2C 11 01 C0
// Every vendor build known (hardware-db25d.md 7, USMART console): name, its build date, at: the word just below
// "EARSAB.0NN" in its program flash (four words cover it: how the build is told), ram: the version string's copy in
// RAM, where the firmware writes the RF chips it found at boot (the hardware variant; the flash holds a placeholder).
// series: the version's first three parts, "909E.DBB.EARSAB" when not given. stub: the stub load through this build's
// console is proven on a radio (its free RAM and command table measured); on the others it is only offered as a try.
// window ([load, size]) and hijack: where this build takes the stub and which RAM word its console calls for every
// frame, when not STUB_WINDOW and HIJACK. The proven builds first: a radio on one is told with the fewest reads
export const BUILDS = [
	{ name: "018", date: "2025-01-21", at: 0x03079F3C, ram: 0x18000AF4, stub: true },
	{ name: "020", date: "2026-01-10", at: 0x0307DCCC, ram: 0x18000B74, stub: true },
	// from static analysis (hardware-db25d.md 7.1): the heap ends above 0x54000 in these, so each has a window above it;
	// the 2022-2023 command table has no pad word, so cmd_rec is its first entry
	{ name: "018", date: "2024-09-23", at: 0x03079C64, ram: 0x18000AFC, stub: false, window: [0x55000, 0x3000] },
	{ name: "018", date: "2024-05-09", at: 0x030797A0, ram: 0x18000AF8, stub: false, window: [0x55000, 0x3000] },
	{ name: "018", date: "2023-07-06", at: 0x03075AAC, ram: 0x18000A74, stub: false, window: [0x55000, 0x3000], hijack: 0x18000840 },
	{ name: "018", date: "2022-11-23", at: 0x030759EC, ram: 0x18000A64, stub: false, window: [0x56000, 0x2000], hijack: 0x18000840 },
	{ name: "017", date: "2022-10-21", at: 0x030759E8, ram: 0x18000A60, stub: false, window: [0x56000, 0x2000], hijack: 0x18000840 },
	// the GD-88's main SoC (hardware-gd88.md): of the B49E series only 021 has the console. Its route is not traced, so
	// it takes the window of a build the page doesn't know, and its version's RAM copy is not known
	{ series: "B49E.D64.EOPSAB", name: "021", date: "2025-01-19", at: 0x0307E66C, stub: false, window: [0x56000, 0x2000] },
];
// A build this page doesn't know: the top 8 KB of SRAM, above the heap of every known build (its highest end:
// 0x554A0, 2022-11-23), and the newer builds' table layout. The pointer check of an untried load still applies
export const UNKNOWN_BUILD = { name: null, date: null, stub: false, window: [0x56000, 0x2000], hijack: 0x18000848 };
const VERSION_WORDS = 6;	// "909E.DBB.EARSAB.018" and its terminator
// Where a build this page doesn't know keeps the version string's RAM copy: around the known builds' (0x18000A60-0x18000B74)
const VERSION_SCAN = [0x18000A00, 0x18000C00];

// A build's series, its full version ("909E.DBB.EARSAB.018") and the version's last two parts, what the build check
// reads ("EARSAB.018")
export const seriesOf = (b) => b.series || "909E.DBB.EARSAB";
export const versionOf = (b) => `${seriesOf(b)}.${b.name}`;
export const buildTail = (b) => `${seriesOf(b).split(".")[2]}.${b.name}`;

// The builds of a board whose flash.stock_versions are `versions`: those of the same series word ("EARSAB"), so the
// older builds of a known series are told too; every known build when versions is null
export function buildsFor(versions) {
	if (!versions) return BUILDS;
	const words = new Set(versions.map((v) => v.split(".")[2]));
	return BUILDS.filter((b) => words.has(seriesOf(b).split(".")[2]));
}

// A build's full version as the board's flash.stock_versions name it ("018" -> "909E.DBB.EARSAB.018"), or the build
export const stockName = (flash, build) => (flash.stock_versions || []).find((v) => v.endsWith(`.${build}`)) || build;

// A version's form: its first and third parts as they are, any model code and build number ("909E.DBB.EARSAB.018" ->
// 909E.<2-4 letters or digits>.EARSAB.<3 digits>)
const esc = (x) => x.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const versionForm = (v) => {
	const p = v.split(".");
	return `${esc(p[0])}\\.[0-9A-Za-z]{2,4}\\.${esc(p[2] || "")}\\.\\d{3}`;
};

// The stock application's version string ("909E.D6x.EARSAB.020") in a read of the program flash, wherever the build
// put it, or null. versions: the forms to look for (a board's flash.stock_versions), every known build's by default.
// Only for messages: what a read holds is decided by its contents (c7000.js analyze)
export function stockVersion(progmem, versions = BUILDS.map(versionOf)) {
	if (!versions.length) return null;
	const app = progmem.subarray(APP_SIG[0] - FLASH_BASE, Math.min(progmem.length, APP_END - FLASH_BASE));
	const m = new RegExp([...new Set(versions.map(versionForm))].join("|")).exec(latin(app));
	return m ? m[0] : null;
}

const READ = /^read_addr\(0x[0-9a-f]{8}\)$/;
const WRITE = /^write_addr\(0x([0-9a-f]{8}),0x[0-9a-f]{8}\)$/;
const NUM = "(0[xX][0-9A-Fa-f]+|\\d+)";
const READ_REPLY = new RegExp(`read_addr\\(\\s*${NUM}\\s*\\)\\s*=\\s*${NUM}\\s*;`, "g");
const WRITE_REPLY = new RegExp(`write_addr\\(\\s*${NUM}\\s*,\\s*${NUM}\\s*\\)\\s*;`, "g");

// one char per byte (TextDecoder("latin1") is windows-1252 and would change the checksum bytes)
const latin = (b) => {
	let s = "";
	for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
	return s;
};

export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, "0");

// Writes may go to the stub window and to the one hijack word of the load under way; nothing else, and never to flash
const writable = (a, { window, hijack }) => a === hijack || (a >= window[0] && a + 4 <= window[0] + window[1]);

// Where a build takes the stub: its own window and hijack word, else the proven builds'. A window must lie in the
// SoC's SRAM (0x10000-0x57FFF, hardware-db25d.md 2) clear of where the stub runs (0x30000-0x38000), and the hijack
// word inside the console's command table
const SRAM = [0x10000, 0x58000];
const STUB_RUN = [0x30000, 0x38000];
const TABLE = [0x18000840, 0x18000860];
export function stubTarget(build) {
	const window = build?.window || STUB_WINDOW, hijack = build?.hijack || HIJACK;
	if (!(window[0] >= SRAM[0] && window[0] + window[1] <= SRAM[1] && (window[0] >= STUB_RUN[1] || window[0] + window[1] <= STUB_RUN[0])))
		throw new UsmartError(`stub window 0x${hex8(window[0])}+0x${window[1].toString(16)} is outside the RAM it may use`);
	if (!(hijack >= TABLE[0] && hijack < TABLE[1] && hijack % 4 === 0)) throw new UsmartError(`hijack word 0x${hex8(hijack)} is outside the console's command table`);
	return { window, hijack };
}

export class Usmart {
	// versions: the builds to look for (the board's flash.stock_versions), every known one by default
	constructor(stream, { log = () => {}, quiet = 80, versions = null } = {}) {
		this.s = stream;
		this.builds = buildsFor(versions);
		this.log = log;
		this.quiet = quiet;	// after a write's echo, how long the line must stay quiet before the next frame
		this.target = stubTarget(null);
	}

	// The only things this class can send: framed read_addr lines, and write_addr lines to the stub window or the
	// hijack word. Anything else is refused before the port.
	async send(line) {
		const w = WRITE.exec(line);
		if (!(READ.test(line) || (w && writable(parseInt(w[1], 16), this.target)))) throw new UsmartError(`refused to send ${JSON.stringify(line)}`);
		await this.s.write(new TextEncoder().encode(`${FRAME}${line}\r\n`));
	}

	// Wait until a reply matching re with first number `addr` came in; returns its numbers, or null after `timeout`
	async reply(re, addr, timeout) {
		const until = Date.now() + timeout;
		while (Date.now() < until) {
			const text = latin(this.s.buf);
			if (text.endsWith("\n")) {
				for (const m of text.matchAll(re)) {
					if (Number(m[1]) === addr) return [Number(m[1]) >>> 0, Number(m[2]) >>> 0];
				}
			}
			await sleep(1);
		}
		return null;
	}

	// One read_addr: the 32-bit value, or null when no matching reply came in time
	async readWord(addr, timeout = 1000) {
		this.s.clear();
		await this.send(`read_addr(0x${hex8(addr)})`);
		const r = await this.reply(READ_REPLY, addr, timeout);
		return r ? r[1] : null;
	}

	// One write_addr: true once the radio echoed the address and the value, false when it did not. A write to a fixed
	// address can be sent again.
	async writeWord(addr, value, timeout = 1000) {
		this.s.clear();
		await this.send(`write_addr(0x${hex8(addr)},0x${hex8(value)})`);
		const r = await this.reply(WRITE_REPLY, addr, timeout);
		if (!r) return false;
		// the stock application prints its own lines after the echo: let them pass before the next frame
		let n = this.s.buf.length, last = Date.now();
		const until = Date.now() + 400;
		while (Date.now() < until && Date.now() - last < this.quiet) {
			if (this.s.buf.length !== n) { n = this.s.buf.length; last = Date.now(); }
			await sleep(2);
		}
		return r[1] === value >>> 0;
	}

	async retry(what, addr, fn, tries, signal) {
		for (let i = 0; i < tries; i++) {
			if (signal?.aborted) throw new UsmartError("stopped");
			const v = await fn(1000 + i * 500);
			if (v !== null && v !== false) return v;
			await sleep(100);
		}
		throw new UsmartError(`the console did not answer the ${what} of 0x${hex8(addr)} after ${tries} tries`);
	}

	readWordRetry(addr, tries = 6, signal) {
		return this.retry("read", addr, (t) => this.readWord(addr, t), tries, signal);
	}

	writeWordRetry(addr, value, tries = 6, signal) {
		return this.retry("write", addr, (t) => this.writeWord(addr, value, t), tries, signal);
	}

	// Is a radio with the stock firmware on the cable? true when the console answers a read of the application's
	// first word
	async probe(timeout = 1500) {
		return (await this.readWord(APP_SIG[0], timeout)) !== null;
	}

	// `n` words from `addr` as text (one char per byte)
	async readText(addr, n, signal) {
		const bytes = new Uint8Array(4 * n);
		for (let i = 0; i < n; i++) new DataView(bytes.buffer).setUint32(4 * i, await this.readWordRetry(addr + 4 * i, 6, signal), true);
		return latin(bytes);
	}

	// Which stock build runs (a BUILDS entry), or null for one this page does not know (its RAM may differ)
	async build(signal) {
		for (const b of this.builds) {
			if ((await this.readText(b.at, 4, signal)).includes(buildTail(b))) return b;
		}
		return null;
	}

	// The version string as the running build filled it in ("909E.DBB.EARSAB.018"), from its RAM copy: the text up to
	// its terminator. null for a build whose RAM copy is not known
	async version(build, signal) {
		if (!build.ram) return null;
		const text = await this.readText(build.ram, VERSION_WORDS, signal);
		return text.split("\0")[0];
	}

	// On a build this page doesn't know: the version string's RAM copy, found by reading the RAM around the known
	// builds' copies word by word until `pattern` (the radio's variant_pattern) matches and the string ends. The
	// string ("909E.DBB.EARSAB.018"), or null
	async findVersion(pattern, signal) {
		if (!pattern) return null;
		const re = new RegExp(pattern);
		let text = "";
		for (let a = VERSION_SCAN[0]; a < VERSION_SCAN[1]; a += 4) {
			text += await this.readText(a, 1, signal);
			const m = re.exec(text);
			if (!m) continue;
			const end = text.indexOf("\0", m.index);
			if (end < 0) continue;
			const start = Math.max(text.lastIndexOf("\0", m.index) + 1, 0);
			return text.slice(start, end);
		}
		return null;
	}

	// Write the universal stub into the window, read every word back, then point the hijack word at it and send one
	// more frame, which starts it. The console stops answering from then on: the stub owns the cable.
	// onProgress(phase, done, total), phase "write" | "check" | "start". untried: a build whose stub load is not
	// proven; the hijack word must then hold a code address in the application, as in every known build, or nothing
	// is written
	// build: the BUILDS entry (its window and hijack word), or null for the proven builds' own
	async loadStub(image, { onProgress = () => {}, signal, untried = false, build = null } = {}) {
		this.target = stubTarget(build);
		const { window: [load, size], hijack } = this.target;
		univFits(image, load, [load, size]);
		const pad = new Uint8Array(Math.ceil(image.length / 4) * 4);
		pad.set(image);
		const words = Array.from({ length: pad.length / 4 }, (_, i) => new DataView(pad.buffer).getUint32(4 * i, true));
		const sig = await this.readWordRetry(APP_SIG[0], 6, signal);
		if (sig !== APP_SIG[1]) throw new UsmartError(`the application does not start as the original firmware does (0x${hex8(sig)})`);
		if (untried) {
			// read before anything points it at the stub (a read after that would start the stub)
			const fn = await this.readWordRetry(hijack, 6, signal);
			if (!(fn >= APP_SIG[0] && fn < APP_END)) throw new UsmartError(`the console's command table is not where the known builds keep it (0x${hex8(hijack)} reads 0x${hex8(fn)})`);
		}
		for (let i = 0; i < words.length; i++) {
			await this.writeWordRetry(load + 4 * i, words[i], 6, signal);
			if (i % 16 === 15 || i === words.length - 1) onProgress("write", i + 1, words.length);
		}
		for (let i = 0; i < words.length; i++) {
			const v = await this.readWordRetry(load + 4 * i, 6, signal);
			if (v !== words[i]) throw new UsmartError(`RAM 0x${hex8(load + 4 * i)} reads 0x${hex8(v)} instead of 0x${hex8(words[i])}`);
			if (i % 16 === 15 || i === words.length - 1) onProgress("check", i + 1, words.length);
		}
		onProgress("start", 0, 1);
		this.log(`stub written to 0x${hex8(load)} and checked (${words.length} words); starting it`);
		// Sent once, never again and never read back: with the word in place every frame starts the stub, a resend
		// included. A lost echo is not an error here; whether the stub came up is what the caller checks.
		if (!(await this.writeWord(hijack, load))) this.log("no echo for the hijack word; starting anyway");
		this.s.clear();
		await this.send(`read_addr(0x${hex8(APP_SIG[0])})`);
		onProgress("start", 1, 1);
	}
}
