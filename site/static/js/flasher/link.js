// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Talking to a radio that runs RadioFoundry, over its USB serial port (WebSerial): the debug link (docs/spec/debuglink.md:
// VERSION, INFO_CHIP, READ_MEM) and the open-firmware CPS reads (docs/spec/open-firmware-cps.md 2.1 and 3: area 1 the
// SPI flash, 5 the MCU flash, 9 the radio info, 10 the flash security registers). Read only, apart from the CPS
// codeplug writes restore uses (X 1-3), which the radio limits to the codeplug areas.

import { be32, concat, le32, sleep, u16be, u16le, u32le, toHex } from "./bytes.js";
import { wire, note } from "../diag.js";
import { serialApi, usbSerialNeeded, showPhoneNotes } from "./usbserial.js";

export { serialApi, showPhoneNotes };

export const RFOUNDRY_USB = { usbVendorId: 0x1FC9, usbProductId: 0x0094 };
// The same radio as a USB network adapter (CDC-NCM, docs/spec/usb-network.md 1): no serial port, and the OS network
// driver owns the interfaces, so a page can only see that it is there (WebUSB lists it without claiming it)
export const RFOUNDRY_NET = { vendorId: 0x1FC9, productId: 0x0096 };
const SOF1 = 0xA5, SOF2 = 0x5A;
export const CMD = { PING: 0x01, VERSION: 0x02, INFO_CHIP: 0x0B, READ_MEM: 0x0C, STORAGE: 0x0F };

export class LinkError extends Error {}

// The flash.err.* text for one of the browser's own USB or serial errors (a DOMException, by name), or null. The
// browser's words ("Failed to open serial port.") say little; every tool shows what to do, with them after it
export function browserErrorKey(e) {
	const raw = e?.message || String(e);
	switch (e?.name) {
	case "NotFoundError": return /disconnect/i.test(raw) ? "flash.err.disconnected" : "flash.err.no_device";
	// "Access denied" on opening a USB device is the device's driver or another program (Windows without WinUSB), not the page
	case "SecurityError": return /access denied/i.test(raw) ? "flash.err.usb_busy" : "flash.err.security";
	case "InvalidStateError": return "flash.err.port_open";
	case "NotAllowedError": return "flash.err.not_allowed";
	case "NetworkError": return /open serial port/i.test(raw) ? "flash.err.port_busy" : /claim|access denied/i.test(raw) ? "flash.err.usb_busy" : "flash.err.connection_lost";
	case "BufferOverrunError": case "BreakError": case "FramingError": case "ParityError": return "flash.err.connection_lost";
	default: return null;
	}
}

// Web Serial, or WebUSB standing in for it where Web Serial reaches no USB device (usbserial.js)
export const serialSupported = () => typeof navigator !== "undefined" && ("serial" in navigator || usbSerialNeeded());

// CRC-16/CCITT-FALSE
export function crc16(data, crc = 0xFFFF) {
	for (const b of data) {
		crc ^= b << 8;
		for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
	}
	return crc;
}

export function frame(cmd, seq, payload = new Uint8Array(0)) {
	const body = concat(new Uint8Array([cmd, seq, payload.length & 0xFF, payload.length >> 8]), payload);
	const c = crc16(body);
	return concat(new Uint8Array([SOF1, SOF2]), body, new Uint8Array([c & 0xFF, c >> 8]));
}

// [{cmd, seq, payload, end}] for each complete, CRC-valid frame in buf
export function parseFrames(buf) {
	const out = [];
	let i = 0;
	while (i + 8 <= buf.length) {
		if (buf[i] !== SOF1 || buf[i + 1] !== SOF2) { i++; continue; }
		const n = u16le(buf, i + 4);
		const e = i + 8 + n;
		if (e > buf.length) break;
		const body = buf.subarray(i + 2, i + 6 + n);
		if (u16le(buf, i + 6 + n) === crc16(body)) {
			out.push({ cmd: buf[i + 2], seq: buf[i + 3], payload: buf.slice(i + 6, i + 6 + n), end: e });
			i = e;
		} else i++;
	}
	return out;
}

export function parseKv(text) {
	const out = {};
	for (const line of text.split("\n")) {
		const k = line.indexOf("=");
		if (k > 0) out[line.slice(0, k).trim()] = line.slice(k + 1).trim();
	}
	return out;
}

// A byte stream with a receive buffer: WebSerial in the browser, fake.js in tests and the simulator.
// Web Serial's non-fatal read errors: the reader ends, port.readable is a new stream. A cap, so a line that keeps
// erroring can't spin the loop for ever.
const SOFT_READ_ERRORS = new Set(["FramingError", "ParityError", "BreakError", "BufferOverrunError"]);
const MAX_READ_GLITCHES = 50;

export class SerialStream {
	constructor(port) {
		this.port = port;
		this.buf = new Uint8Array(0);
		this.closed = false;
		this.lost = false;	// the pump ended without close(): the device is gone
		// what came in since the port opened, for a report when nothing answered as expected (silence())
		this.received = 0;
		this.first = new Uint8Array(0);
	}

	static async request() {
		const port = await serialApi().requestPort({ filters: [RFOUNDRY_USB] });
		return SerialStream.open(port);
	}

	// Any serial port (USB-serial programming cables on radios without USB)
	static async requestAny() {
		return SerialStream.open(await serialApi().requestPort());
	}

	// Chrome's default 255 byte buffer stops the USB reads whenever the page is busy for a moment; the radio gives up
	// on a reply the host doesn't take for a while (drivers/usb/usb_device.c), so a 2 KB read could lose its tail
	static async open(port, baudRate = 115200) {
		const opts = { baudRate, bufferSize: 65536 };
		try {
			await port.open(opts);
		} catch (e) {
			// still open from an earlier connection of this page (the user picked the same radio again): closed and
			// opened afresh, so its settings are these
			if (e?.name !== "InvalidStateError") throw e;
			// a port another stream of this page is reading stays as it is: close() refuses it, and the error stands
			try { await port.close(); } catch { throw e; }
			note("serial port was still open: closed and opened again");
			await port.open(opts);
		}
		const s = new SerialStream(port);
		s.baud = baudRate;
		s.pumping = s.pump();
		return s;
	}

	async pump() {
		let glitches = 0;
		while (!this.closed && this.port.readable) {
			this.reader = this.port.readable.getReader();
			try {
				for (;;) {
					const { value, done } = await this.reader.read();
					if (done) break;
					this.buf = concat(this.buf, value);
					this.received += value.length;
					if (this.first.length < 16) this.first = concat(this.first, value.subarray(0, 16 - this.first.length));
				}
			} catch (e) {
				// A framing, parity or overrun error or a break (a radio switched on or off with the cable in) ends only
				// this reader: the port gives a fresh readable and reading goes on. Stopping here left every later
				// reply unread ("nothing received"). A lost device has no readable, which ends the loop.
				const soft = SOFT_READ_ERRORS.has(e?.name) && ++glitches <= MAX_READ_GLITCHES;
				if (!this.closed) note(soft ? "serial read error, reading on" : "serial read stopped", String(e?.message || e));
				if (!soft) break;
			} finally {
				this.reader.releaseLock();
			}
		}
		// the device went away (or its port stopped reading): a page that kept this stream would talk to nothing
		if (!this.closed) this.lost = true;
	}

	// A port whose device went away has no writable: that is a lost connection (errorKey), not a TypeError
	async write(data) {
		if (!this.port.writable) throw new DOMException("The device has been lost.", "NetworkError");
		const w = this.port.writable.getWriter();
		try { await w.write(data); } finally { w.releaseLock(); }
	}

	clear() { this.buf = new Uint8Array(0); }

	take(n) {
		const out = this.buf.slice(0, n);
		this.buf = this.buf.slice(n);
		return out;
	}

	// The port closes only once the pump has let go of its reader: WebSerial refuses to close a port whose stream is
	// still locked, and a port left open fails the next connection with "The port is already open"
	async close() {
		this.closed = true;
		try { await this.reader?.cancel(); } catch (e) { /* gone */ }
		// a pump that never ends (a browser that doesn't settle the read) must not hang the page's close
		await Promise.race([this.pumping, sleep(2000)]);
		try { await this.port.close(); } catch (e) { note("serial port did not close", String(e?.message || e)); }
	}
}

export class Link {
	constructor(stream, { timeout = 1500 } = {}) {
		this.s = stream;
		this.timeout = timeout;
		this.seq = 0x40;
	}

	async waitFor(test, timeout = this.timeout) {
		const until = Date.now() + timeout;
		for (;;) {
			const r = test(this.s.buf);
			if (r !== undefined) return r;
			if (Date.now() > until) return undefined;
			await sleep(2);
		}
	}

	// ---- debug link

	async request(cmd, payload = new Uint8Array(0)) {
		this.seq = (this.seq + 1) & 0xFF;
		const seq = this.seq;
		this.s.clear();
		await this.s.write(frame(cmd, seq, payload));
		const reply = await this.waitFor((buf) => parseFrames(buf).find((f) => f.seq === seq && (f.cmd === (cmd | 0x80) || f.cmd === 0xFF)));
		if (!reply) {
			wire(`cmd 0x${cmd.toString(16)} seq ${seq} len ${payload.length}: no answer`);
			throw new LinkError(`no answer to command 0x${cmd.toString(16)}`);
		}
		this.s.take(reply.end);
		wire(`cmd 0x${cmd.toString(16)} seq ${seq} len ${payload.length}: status ${reply.payload[0]} len ${reply.payload.length}`);
		if (!reply.payload.length || reply.payload[0] !== 0) throw new LinkError(`command 0x${cmd.toString(16)}: status ${reply.payload[0]}`);
		return reply.payload.subarray(1);
	}

	async version() {
		return parseKv(new TextDecoder().decode(await this.request(CMD.VERSION)));
	}

	// INFO_CHIP (debuglink.md 3.9), offsets after the status byte
	async chip() {
		const r = await this.request(CMD.INFO_CHIP);
		if (r.length < 28) return null;
		// family: 0 STM32F4, 1 HR_C7000 (no option bytes: rdp to user read 0xFF); firmware before the byte was STM32F4 only
		return { idcode: u32le(r, 0), flashKb: u16le(r, 4), uid: toHex(r.subarray(6, 18)), token: u32le(r, 6), optcr: u32le(r, 18),
			rdp: r[22], wrp: u16le(r, 24), bor: r[26], user: r[27], family: r.length > 28 ? r[28] : 0 };
	}

	// STORAGE INFO (debuglink.md 3.13): whether the settings and journal region is ours, or still holds another
	// firmware's data ("foreign": the radio then refuses every settings save and codeplug write). null from a build
	// without the command
	async storageInfo() {
		let r;
		try {
			r = await this.request(CMD.STORAGE, new Uint8Array([0]));
		} catch (e) {
			if (e instanceof LinkError && !/no answer/.test(e.message)) return null;
			throw e;
		}
		if (r.length < 2) return null;
		const region = ["none", "ours", "foreign"][r[0]] || String(r[0]);
		const range = (o) => (r.length >= o + 8 ? { addr: u32le(r, o), size: u32le(r, o + 4) } : null);
		return { region, pending: !!r[1], settings: range(2), journal: range(10), calibration: range(18) };
	}

	// STORAGE CLAIM: the radio erases a foreign region and takes it (some 10 sector erases), then the poll sees it
	// ours. The token is the chip UID's low word, so only a client that read INFO_CHIP can ask. True once it is ours
	async storageClaim({ timeout = 20000 } = {}) {
		const chip = await this.chip();
		if (!chip) throw new LinkError("no chip info");
		await this.request(CMD.STORAGE, concat(new Uint8Array([2]), le32(chip.token)));
		note("storage claim sent");
		const until = Date.now() + timeout;
		while (Date.now() < until) {
			await sleep(500);
			const i = await this.storageInfo().catch(() => null);
			if (i?.region === "ours") return true;
		}
		return false;
	}

	// READ_MEM (debuglink.md 3.10) in 1000-byte pieces: internal flash, the stock bootloader included
	async readMem(addr, size) {
		const out = new Uint8Array(size);
		for (let done = 0; done < size;) {
			const n = Math.min(1000, size - done);
			const r = await this.request(CMD.READ_MEM, concat(le32(addr + done), new Uint8Array([n & 0xFF, n >> 8])));
			if (r.length !== 4 + n || u32le(r, 0) !== addr + done) throw new LinkError(`READ_MEM answer does not match 0x${(addr + done).toString(16)}`);
			out.set(r.subarray(4), done);
			done += n;
		}
		return out;
	}

	// ---- open-firmware CPS

	// R area addr len: the reply is "R" + length (BE) + data, or "-" when refused. A read changes nothing, so one
	// that went unanswered or came back short is asked again, after the rest of a broken reply has drained.
	async cpsRead(area, addr, len, tries = 3) {
		for (let i = 0; i < tries; i++) {
			if (i) await sleep(300);
			this.s.clear();
			await this.s.write(concat(new Uint8Array([0x52, area]), be32(addr), new Uint8Array([len >> 8, len & 0xFF])));
			const got = await this.waitFor((buf) => {
				if (buf.length && buf[0] === 0x2D) return null;
				if (buf.length >= 3 && buf[0] === 0x52 && buf.length >= 3 + u16be(buf, 1)) return buf.slice(3, 3 + u16be(buf, 1));
				return undefined;
			});
			this.s.clear();
			wire(`R ${area} 0x${addr.toString(16)} ${len}: ${got === undefined ? "no answer" : got === null ? "refused" : `${got.length} bytes`}`);
			if (got !== undefined) return got;	// null: refused (out of range, not offered)
		}
		throw new LinkError(`no answer to a codeplug read at 0x${addr.toString(16)}`);
	}

	// Read [addr, addr + size) of an area in pieces; stops early (and says so) when the radio refuses. RadioFoundry answers up
	// to 32 KB per read, older builds 2045 bytes (open-firmware-cps.md 2.1): each read takes what comes back.
	async cpsDump(area, addr, size, { chunk = 32768, progress = () => {}, signal } = {}) {
		const out = new Uint8Array(size);
		let done = 0;
		while (done < size) {
			if (signal?.aborted) throw new LinkError("stopped");
			const n = Math.min(chunk, size - done);
			const part = await this.cpsRead(area, addr + done, n);
			if (part === null || part.length === 0) return { data: out.subarray(0, done), complete: false };
			out.set(part.subarray(0, n), done);
			done += Math.min(part.length, n);
			progress(done, size);
		}
		return { data: out, complete: true };
	}

	// X 1 (prepare sector), X 2 (merge data), X 3 (commit): one 4 KB sector of the SPI flash
	async cpsWriteSector(addr, data) {
		return this.cpsWriteParts(addr, [[addr, data]]);
	}

	// One sector, changing only the given [addr, bytes] parts (each inside the sector at addr): the radio keeps the
	// sector's other bytes, so a part of a sector the codeplug shares with other data can be written safely. False
	// when a step fails; writeFailure then says which and how ("refused": '-', "no answer")
	async cpsWriteParts(addr, parts) {
		const sector = addr >>> 12;
		const ok = async (req, what) => {
			this.s.clear();
			await this.s.write(req);
			const r = await this.waitFor((buf) => (buf.length >= 1 && buf[0] === 0x2D ? false : buf.length >= 2 ? true : undefined), Math.max(3000, this.timeout));
			this.s.clear();
			const result = r === true ? "ok" : r === false ? "refused" : "no answer";
			wire(`${what}: ${result}`);
			if (r !== true) this.writeFailure = { what, result };
			return r === true;
		};
		this.writeFailure = null;
		if (!await ok(new Uint8Array([0x58, 1, sector >> 16, (sector >> 8) & 0xFF, sector & 0xFF]), `X 1 sector 0x${addr.toString(16)}`)) return false;
		for (const [a, data] of parts) {
			for (let o = 0; o < data.length; o += 1024) {
				const part = data.subarray(o, o + 1024);
				if (!await ok(concat(new Uint8Array([0x58, 2]), be32(a + o), new Uint8Array([part.length >> 8, part.length & 0xFF]), part), `X 2 0x${(a + o).toString(16)} ${part.length}`)) return false;
			}
		}
		return ok(new Uint8Array([0x58, 3]), "X 3 commit");
	}

	async cpsClose() {
		this.s.clear();
		await this.s.write(new Uint8Array([0x43, 5]));
		await this.waitFor((buf) => (buf.length ? true : undefined), 500);
		this.s.clear();
	}
}

// Ask a running RadioFoundry radio what it is. Returns null when nothing answers.
export async function probe(link) {
	try {
		const v = await link.version();
		let chip = null;
		try { chip = await link.chip(); } catch (e) { /* older firmware */ }
		return { board: v.board, git: v.git, codec: v.codec, features: v.features || "", display: v.display, version: v, chip };
	} catch (e) {
		link.probeError = e;
		return null;
	}
}

// Ports this site was granted before and that are plugged in now (the browser's chooser marks them paired), as a
// chooser with these filters would list them
export async function grantedPorts(filters) {
	if (!serialSupported()) return [];
	return (await serialApi().getPorts()).filter((p) => {
		const i = p.getInfo();
		return filters.some((f) => i.usbVendorId === f.usbVendorId && (f.usbProductId === undefined || i.usbProductId === f.usbProductId));
	});
}

// A granted port (grantedPorts) whose radio answers, so the page needs no chooser: want(probe) says whether it's the
// radio expected. {stream, port, probe}, or null (none answered, the page shows the chooser). The probe is short: the
// chooser opens only within a few seconds of the click that asked for it
export async function knownRadioPort(filters, want = () => true) {
	for (const port of await grantedPorts(filters)) {
		let stream;
		try {
			stream = await SerialStream.open(port);
		} catch {
			continue;	// held by another tab or program
		}
		const p = await probe(new Link(stream, { timeout: 700 }));
		if (p && want(p)) return { stream, port, probe: p };
		try { await stream.close(); } catch { /* gone */ }
	}
	return null;
}

// Why a probe got no answer, for the message and the error report: what failed, and what the port gave meanwhile.
// kind "silent": nothing at all (the wrong port, a radio that is off, a cable not plugged in fully); "link": debug
// link frames that weren't the reply (RadioFoundry, but the exchange failed); "other": bytes that aren't the debug
// link, so something else answers there, most often a radio still on its original firmware
export function silence(link) {
	const s = link.s || {};
	const first = s.first || new Uint8Array(0);
	const kind = !s.received ? "silent" : first[0] === SOF1 && first[1] === SOF2 ? "link" : "other";
	return { kind, cause: link.probeError ? String(link.probeError.message || link.probeError) : null,
		received: s.received ?? null, first: first.length ? toHex(first) : null };
}

// The open-firmware CPS's radio info (open-firmware-cps.md 4: area 9, little endian): {structVersion, radioType, git,
// built, flashId, features}, or null when the bytes aren't that structure. Checked field by field, so a reply of another
// protocol that happens to start with "R" isn't taken for it. The revision or the build date must be there: our shim
// (core/cps/cps_openfw.c) always fills both, so all zeros after the struct version is not that structure
export const RADIO_INFO_LEN = 46;
export function parseRadioInfo(b) {
	if (!b || b.length < 40) return null;
	// ASCII, NUL padded: printable characters, then only NULs
	const text = (o, n) => {
		const f = b.subarray(o, o + n);
		const end = f.indexOf(0) < 0 ? n : f.indexOf(0);
		if (f.subarray(0, end).some((c) => c < 0x20 || c > 0x7E) || f.subarray(end).some((c) => c !== 0)) return null;
		return String.fromCharCode(...f.subarray(0, end));
	};
	const structVersion = u32le(b, 0), radioType = u32le(b, 4), git = text(8, 16), built = text(24, 16);
	if (structVersion < 1 || structVersion > 15 || radioType > 63 || git === null || built === null) return null;
	if (!git && !built) return null;
	if (built && !/^\d{8,14}$/.test(built)) return null;
	return { structVersion, radioType, git, built, flashId: b.length >= 44 ? u32le(b, 40) : null,
		features: b.length >= 46 ? u16le(b, 44) : null };
}

// A radio whose port refused the debug link probe with the open-firmware CPS's "-" (open-firmware-cps.md 2: the reply to
// any request it doesn't know) is asked for its radio info, a read that changes nothing on the radio; never a C control
// command (some save settings or restart the radio). The info, or null when the port isn't that protocol. A
// RadioFoundry build with the CPS shim but without the debug link (CONFIG_DEBUGLINK=n) answers the same way
export async function openFirmwareInfo(link) {
	const first = link.s?.first;
	if (silence(link).kind !== "other" || first?.[0] !== 0x2D) return null;
	try {
		return parseRadioInfo(await link.cpsRead(9, 0, RADIO_INFO_LEN, 1));
	} catch (e) {
		return null;
	}
}

// Whether a radio that answers like another open firmware runs the RadioFoundry build just written (manifest: its
// manifest), so without the debug link: the build's feature list leaves DEBUGLINK out. A build with DEBUGLINK answers
// over the debug link, so a radio that doesn't runs something else. Only when the features aren't known (a local image:
// "") does the revision decide: the radio info carries the version the Makefile stamps (8 hex digits and maybe
// "-dirty"), and that build's commit is the one just written. A build of unknown features and another commit can't be
// told from another open firmware
export function writtenWithoutLink(info, manifest) {
	if (!info || !manifest) return false;
	const feats = String(manifest.features || "").split(",").filter(Boolean);
	if (feats.length) return !feats.includes("DEBUGLINK");
	const head = String(info.git || "").split("-")[0].toLowerCase();
	return /^[0-9a-f]{7,}$/.test(head) && String(manifest.git || "").toLowerCase().startsWith(head);
}

// silence(), told apart further where the port can be asked: kind "openfw" for a radio on another open firmware (one that
// answers the open-firmware CPS protocol), with its radio type and revision. A RadioFoundry build without the debug link
// answers the same way; written: the manifest of the build just written, so that it is told apart (kind
// "rfoundry-nodebug") right after installing it
export async function whyNoAnswer(link, { written = null } = {}) {
	const why = silence(link);
	const info = await openFirmwareInfo(link);
	if (!info) return why;
	return { ...why, kind: writtenWithoutLink(info, written) ? "rfoundry-nodebug" : "openfw", radioType: info.radioType, git: info.git };
}

// The message for whyNoAnswer()'s kind (site/i18n/en.json)
export function noAnswerKey(why) {
	return { openfw: "flash.err.openfw", "rfoundry-nodebug": "flash.err.rfoundry_nodebug", other: "flash.err.not_rfoundry" }[why?.kind]
		|| "flash.err.no_answer";
}
