// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone update mode (radios.json flash.backend "anytone-update"): the vendor's own firmware update protocol on the
// radio's USB serial port at 921600 baud, which the bootloader runs when the radio is turned on with the board's keys
// held (docs/spec/hardware-d578uv.md 8). A port of rfoundry's anytone.py: the same frames, the same checks. No DOM
// here, so the Node tests can use it (site/tests/anytone.test.mjs).
//
// The radio names its model when asked (02), and the name differs by MCU: the board's flash-boards.json anytone_models
// map the names to [hardware] variants codes. As the vendor's updater does, the page writes a vendor firmware set only to
// a radio whose model its .spi names, and RadioFoundry only to a variant that runs this board's image.

import { sha256, u32le, fromHex, sleep } from "./bytes.js";

export class AnytoneError extends Error {}

export const ACK = 0x06, NAK = 0x15, DATA = 0x01;
export const BAUD = 921600;
export const HELLO = new TextEncoder().encode("UPDATE");
export const HELLO_REPLY = Uint8Array.of(0x51, 0x58, ACK);	// "QX", ACK
export const IDENTIFY = Uint8Array.of(0x02), IDENTIFY_LEN = 16;
export const FINISH = Uint8Array.of(0x18);
export const CHUNK = 32, FRAME_LEN = 40;
export const RETRIES = 3;
export const TIMEOUT = 2000;			// ms for each answer
// The stock firmware's PC mode, with the radio on and running its stock firmware: the codeplug is read before the
// install (docs/spec/codeplug-stock-anytone.md)
export const PROGRAM = new TextEncoder().encode("PROGRAM"), END = new TextEncoder().encode("END");
export const READ = 0x52, READ_REPLY = 0x57, READ_MAX = 16;
const SPI_HEADER = 14, SPI_MODEL_LEN = 8, SPI_MODEL2 = SPI_HEADER + 12;
const CDI_ENTRY = 278;

const ascii = (b) => String.fromCharCode(...b);
const hexs = (b) => (b?.length ? [...b].map((x) => x.toString(16).padStart(2, "0")).join("") : "nothing");

// One data frame: 01, the address (LE), 32 bytes (zero padded), the 16-bit LE sum of bytes 1..36, 06
export function frame(addr, chunk) {
	if (chunk.length > CHUNK) throw new RangeError(`a frame carries at most ${CHUNK} bytes`);
	const f = new Uint8Array(FRAME_LEN);
	f[0] = DATA;
	f[1] = addr & 0xFF; f[2] = (addr >>> 8) & 0xFF; f[3] = (addr >>> 16) & 0xFF; f[4] = (addr >>> 24) & 0xFF;
	f.set(chunk, 5);
	let sum = 0;
	for (let i = 1; i < 37; i++) sum += f[i];
	f[37] = sum & 0xFF;
	f[38] = (sum >>> 8) & 0xFF;
	f[39] = ACK;
	return f;
}

export const framesNeeded = (n) => Math.ceil(n / CHUNK);

// One read: 'R', the memory address (big-endian), the count
export function readRequest(addr, n) {
	if (!(n > 0 && n <= READ_MAX)) throw new RangeError(`a read asks for 1 to ${READ_MAX} bytes`);
	return Uint8Array.of(READ, (addr >>> 24) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 8) & 0xFF, addr & 0xFF, n);
}

const sum8 = (b) => b.reduce((a, x) => (a + x) & 0xFF, 0);

// The radio's answer to a read: 'W', address, count, data, the 8-bit sum of those, 06 (the fake's, and the tests')
export function readReply(addr, data) {
	const r = new Uint8Array(data.length + 8);
	r.set(readRequest(addr, data.length));
	r[0] = READ_REPLY;
	r.set(data, 6);
	r[r.length - 2] = sum8(r.subarray(1, r.length - 2));
	r[r.length - 1] = ACK;
	return r;
}

// The data of the answer to readRequest(addr, n), or AnytoneError
export function parseRead(addr, n, r) {
	const at = `0x${(addr >>> 0).toString(16)}`;
	if (r?.length !== n + 8 || r[0] !== READ_REPLY || r[r.length - 1] !== ACK) throw new AnytoneError(`the radio's answer to a read at ${at} is malformed: ${hexs(r)}`);
	const got = ((r[1] << 24) | (r[2] << 16) | (r[3] << 8) | r[4]) >>> 0;
	if (got !== addr >>> 0 || r[5] !== n) throw new AnytoneError(`the radio answered a read at ${at} with 0x${got.toString(16)}`);
	if (sum8(r.subarray(1, r.length - 2)) !== r[r.length - 2]) throw new AnytoneError(`bad sum in the answer to a read at ${at}`);
	return r.slice(6, r.length - 2);
}

// {model, hw, version} from the 16-byte answer to 02: 'I', the model (7 bytes, NUL padded), the hardware byte, the
// version (4 bytes), 00 00 06
export function parseIdentify(r) {
	if (r?.length !== IDENTIFY_LEN || r[0] !== 0x49 || r[15] !== ACK) throw new AnytoneError(`the radio's identity answer is malformed: ${hexs(r)}`);
	const name = r.subarray(1, 8);
	const end = name.indexOf(0);
	return { model: ascii(end < 0 ? name : name.subarray(0, end)).trim(), hw: r[8], version: ascii(r.subarray(9, 13)) };
}

// ---------------------------------------------------------------------------------------------------------------
// which radio, which image. board: a flash-boards.json board; radio: its radios.json entry (variants)

// The variant code whose model names include `model`, or null
export function variantOf(board, model) {
	for (const [code, names] of Object.entries(board?.anytone_models || {})) if (names.includes(model)) return code;
	return null;
}

// The radios.json variant entry of a code
const variantEntry = (radio, code) => (radio?.variants || []).find((v) => v.code === code) || null;
export const variantLabel = (radio, code) => { const v = variantEntry(radio, code); return v ? `${code} (${v.name})` : code; };

// What a radio that answered `model` means for a task ("install" | "restore"), as rfoundry's check_install and
// check_restore judge it. chosen: the variant code the user picked, or null; spiModels: the restore set's .spi names.
// {ok, code, why, text}: why is "unknown" (not this board's radio), "mismatch" (the user picked another variant),
// "no_image" (no RadioFoundry image for the variant: restore only), "other_board" (the variant takes another board's
// image, `board` its id) or "wrong_set" (the set is for another model)
export function judge({ board, radio, model, task, chosen = null, spiModels = null }) {
	const code = variantOf(board, model);
	const all = Object.values(board?.anytone_models || {}).flat().join(", ");
	if (!code) return { ok: false, code, why: "unknown", text: `the radio answers model ${JSON.stringify(model)}, which is not a ${board?.name} (known: ${all})` };
	if (chosen && chosen !== code)
		return { ok: false, code, why: "mismatch", text: `you picked ${variantLabel(radio, chosen)}, but the radio answers model ${model}: it is ${variantLabel(radio, code)}` };
	if (task === "install") {
		const runs = variantEntry(radio, code)?.runs?.board || null;
		if (runs && runs !== board.id) return { ok: false, code, why: "other_board", board: runs, text: `the radio is ${variantLabel(radio, code)}, which takes the ${runs} image, not the ${board.id} one` };
		if (runs !== board.id) return { ok: false, code, why: "no_image", text: `the radio is ${variantLabel(radio, code)}, which RadioFoundry has no image for. Its original firmware can still be written back` };
	} else if (!(spiModels || []).includes(model))
		return { ok: false, code, why: "wrong_set", text: `this firmware is for ${(spiModels || []).join(" or ") || "no model"}, the radio answers ${model} (${variantLabel(radio, code)}): refused, as the manufacturer's updater refuses it` };
	return { ok: true, code, why: null, text: "" };
}

// ---------------------------------------------------------------------------------------------------------------
// files: the vendor's firmware set, the boot block

// The model names of a .spi file (its two 8-byte fields), blanks dropped
export function spiModels(spi) {
	const out = [];
	for (const at of [SPI_HEADER, SPI_MODEL2]) {
		const f = spi.subarray(at, at + SPI_MODEL_LEN);
		const end = f.indexOf(0);
		const name = ascii(end < 0 ? f : f.subarray(0, end)).trim();
		if (name && !out.includes(name)) out.push(name);
	}
	return out;
}

// {addr, image, models} of a vendor firmware set's three files ({cdd, cdi, spi} bytes)
export function stockSet({ cdd, cdi, spi }) {
	if (!cdd || !cdi || !spi) throw new AnytoneError("a firmware set needs its .CDD, .CDI and .spi files");
	if (cdi.length !== CDI_ENTRY) throw new AnytoneError(`the .CDI holds ${cdi.length % CDI_ENTRY ? `${cdi.length} bytes` : `${cdi.length / CDI_ENTRY} entries`}, not one firmware image`);
	const addr = u32le(cdi, 256), len = u32le(cdi, 260), off = u32le(cdi, 264);
	const image = cdd.slice(off, off + len);
	if (!len || image.length !== len) throw new AnytoneError(`the .CDD is shorter than its .CDI says (${len} bytes from ${off})`);
	const models = spiModels(spi);
	if (!models.length) throw new AnytoneError("the .spi names no radio model");
	return { addr, image, models };
}

// Sort picked files into the set: {cdd, cdi, spi, names} by extension; the .CDD is the largest of the three
export function sortSet(files) {
	const out = { names: {} };
	for (const f of files) {
		const ext = (f.name.match(/\.(cdd|cdi|spi)$/i)?.[1] || "").toLowerCase();
		if (ext) { out[ext] = f.data; out.names[ext] = f.name; }
	}
	return out;
}

// The image must sit in the application area
export function checkFits(board, addr, data) {
	if (addr !== board.app_base || !data.length || addr + data.length > board.flash_end)
		throw new AnytoneError(`the image (${data.length} bytes at 0x${addr.toString(16)}) doesn't fit the application area 0x${board.app_base.toString(16)}..0x${board.flash_end.toString(16)}`);
}

// The DMR baseband's boot block in a stock file or flash read (fb.modem_boot: len, sha256, prefix), checked by its hash;
// null when it isn't there
export async function findBootBlock(data, mb) {
	const pre = fromHex(mb.prefix);
	outer: for (let at = 0; at + mb.len <= data.length; at++) {
		for (let i = 0; i < pre.length; i++) if (data[at + i] !== pre[i]) continue outer;
		const blk = data.slice(at, at + mb.len);
		if (await sha256(blk) === mb.sha256) return blk;
	}
	return null;
}

// What the update mode writes for RadioFoundry: the application, 0xFF up to the board's slot, the boot block
export function installImage(board, app, block) {
	const [slot, end] = board.codec_slot || [];
	if (!slot) throw new AnytoneError(`${board.name} has no modem boot slot`);
	const off = slot - board.app_base;
	if (app.length > off) throw new AnytoneError(`the image (${app.length} bytes) runs into the modem boot slot at 0x${slot.toString(16)}`);
	if (slot + block.length > end) throw new AnytoneError("the boot block doesn't fit the slot");
	const out = new Uint8Array(off + block.length).fill(0xFF);
	out.set(app, 0);
	out.set(block, off);
	return out;
}

// An image file with the boot block merged in (tools/modem_boot.py): {image, block}, block null when there is none
export async function splitImage(board, data, mb) {
	const off = (board.codec_slot?.[0] ?? 0) - board.app_base;
	if (!board.codec_slot || data.length < off + mb.len) return { image: data, block: null };
	const blk = data.slice(off, off + mb.len);
	if (await sha256(blk) !== mb.sha256) return { image: data, block: null };
	let end = off;
	while (end > 0 && data[end - 1] === 0xFF) end--;
	return { image: data.slice(0, end), block: blk };
}

// ---------------------------------------------------------------------------------------------------------------
// the radio, on a stream (SerialStream, or fakeanytone.js's)

export class AnytoneUpdate {
	constructor(stream, { log = () => {}, timeout = TIMEOUT, retries = RETRIES } = {}) {
		this.s = stream;
		this.log = log;
		this.timeout = timeout;
		this.retries = retries;
	}

	async readExact(n, timeout = this.timeout) {
		const end = Date.now() + timeout;
		while (this.s.buf.length < n && Date.now() < end) {
			if (this.s.arrived) await this.s.arrived(Math.max(1, end - Date.now()));
			else await sleep(1);
		}
		return this.s.take(n);
	}

	async hello() {
		this.s.clear();
		await this.s.write(HELLO);
		const r = await this.readExact(HELLO_REPLY.length);
		if (r.length !== 3 || r.some((b, i) => b !== HELLO_REPLY[i]))
			throw new AnytoneError(`no answer to UPDATE (${hexs(r)}): is the radio in update mode?`);
	}

	async identify() {
		await this.s.write(IDENTIFY);
		return parseIdentify(await this.readExact(IDENTIFY_LEN));
	}

	// Every frame of data from addr, each acknowledged (a NAK or no answer: sent again, up to retries), then the
	// finish. onFrame(done, total); signal: an AbortSignal that stops between frames. Returns the frames written.
	async write(addr, data, { onFrame = () => {}, signal } = {}) {
		const total = framesNeeded(data.length);
		for (let i = 0; i < total; i++) {
			if (signal?.aborted) throw new AnytoneError(`stopped after ${i} of ${total} frames: the radio stays in update mode, write again`);
			const f = frame(addr + i * CHUNK, data.subarray(i * CHUNK, (i + 1) * CHUNK));
			let r = null;
			for (let k = 0; k < this.retries; k++) {
				await this.s.write(f);
				r = await this.readExact(1);
				if (r.length === 1 && r[0] === ACK) break;
				if (k < this.retries - 1) this.log(`frame at 0x${(addr + i * CHUNK).toString(16)}: ${r.length ? (r[0] === NAK ? "NAK" : hexs(r)) : "no answer"}, sent again`);
			}
			if (!(r.length === 1 && r[0] === ACK))
				throw new AnytoneError(`the radio refused the frame at 0x${(addr + i * CHUNK).toString(16)} ${this.retries} times (${r.length ? (r[0] === NAK ? "NAK" : hexs(r)) : "no answer"}): the radio stays in update mode, write again`);
			onFrame(i + 1, total);
		}
		await this.s.write(FINISH);
		const r = await this.readExact(1);
		if (!(r.length === 1 && r[0] === ACK)) throw new AnytoneError("the radio didn't confirm the end of the write");
		return total;
	}
}

// The stock firmware's PC mode on a stream: read its memory, then END (the radio restarts)
export class AnytoneProgram extends AnytoneUpdate {
	async hello() {
		this.s.clear();
		await this.s.write(PROGRAM);
		const r = await this.readExact(HELLO_REPLY.length);
		if (r.length !== 3 || r.some((b, i) => b !== HELLO_REPLY[i]))
			throw new AnytoneError(`no answer to PROGRAM (${hexs(r)}): is the radio on, running its original firmware?`);
	}

	// n bytes from addr, READ_MAX at a time, each read asked again on a bad answer. onBytes(done, n); signal stops it
	async read(addr, n, { onBytes = () => {}, signal } = {}) {
		const out = new Uint8Array(n);
		for (let done = 0; done < n;) {
			if (signal?.aborted) throw new AnytoneError("stopped");
			const k = Math.min(READ_MAX, n - done), a = addr + done;
			let err = null;
			for (let i = 0; i < this.retries; i++) {
				this.s.clear();
				await this.s.write(readRequest(a, k));
				try {
					out.set(parseRead(a, k, await this.readExact(k + 8)), done);
					err = null;
					break;
				} catch (e) {
					err = e;
				}
			}
			if (err) throw err;
			done += k;
			onBytes(done, n);
		}
		return out;
	}

	// Leave the PC mode; the radio answers, then restarts
	async end() {
		await this.s.write(END);
		const r = await this.readExact(HELLO_REPLY.length);
		return r.length === 3 && r.every((b, i) => b === HELLO_REPLY[i]);
	}
}
