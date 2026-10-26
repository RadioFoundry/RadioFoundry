// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codeplug files for the editor: what a file holds and the image in it (a codeplug image, a radio backup bundle, the
// USB network codeplug.bin), and the draft the page keeps in the browser between visits. A file in another layout (a
// stock TYT-family codeplug, docs/spec/codeplug-stock-tyt.md) is converted, or reported as foreign.

import { CodeplugImage, CODEPLUG_SPAN, TFTP_SIZE } from "../codeplug/image.js";
import { readHeader, recordSize, AREA1, AREA1_SIZE, HEADER, AREA2 } from "../codeplug/iddb.js";
import { openBundle } from "../flasher/backup.js";
import { unzip } from "../flasher/zip.js";
import { startsWith, toHex } from "../flasher/bytes.js";
import { classify } from "../codeplug/detect.js";
import { importStock, STOCK, RDT_MD380 } from "../codeplug/stock_tyt.js";
import { empty, encode } from "../codeplug/model.js";

// code: the editor.err.* text; params: what the file was, for the error report (never its contents)
export class FileError extends Error {
	constructor(code, params = undefined) {
		super(code);
		this.name = "FileError";
		this.code = code;
		if (params) this.params = params;
	}
}

// What an unknown file was, for the report: its type by extension, size and first 8 bytes (a format's signature),
// never its name
const extOf = (name) => (/\.([^./]{1,8})$/.exec(name || "")?.[1] || "").toLowerCase();
const fileFacts = (name, bytes, more = {}) => ({ ext: extOf(name), size: bytes.length, head: toHex(bytes.subarray(0, 8)), ...more });

// How far a dump must be kept so the DMR ID database in it stays whole
function idDbTop(b) {
	const hd = readHeader(b);
	if (!hd) return 0;
	const per1 = Math.floor((AREA1_SIZE - HEADER) / hd.rec);
	return hd.count <= per1 ? AREA1 + HEADER + hd.count * hd.rec : AREA2 + (hd.count - per1) * hd.rec;
}

export function imageFrom(bytes) {
	const keep = Math.max(CODEPLUG_SPAN, Math.min(bytes.length, idDbTop(bytes)));
	return CodeplugImage.from(bytes.subarray(0, Math.min(bytes.length, keep)));
}

// What a codeplug image or SPI flash dump holds, as openFile returns it:
//   {kind: "image", image, board}: the open-firmware layout, or blank
//   {kind: "converted", from, model, image, counts, notes, board}: a stock codeplug, converted. model is the editing
//     model (on a blank image), image its encoding; notes are {code, n} with qdmr.js's note codes
//   {kind: "foreign", format, board}: a layout the editor can't read ("tyt-uv380-rdt" or "unknown")
export function fromBytes(bytes, board = null) {
	const { layout } = classify(bytes);
	if (layout === "openfw" || layout === "blank") return { kind: "image", image: imageFrom(bytes), board };
	if (STOCK[layout]) {
		const model = empty();
		const { counts, notes } = importStock(model, bytes, layout);
		return { kind: "converted", from: layout, model, image: encode(model), counts, notes, board };
	}
	return { kind: "foreign", format: layout, board };
}

// {kind: "image" | "converted" | "foreign" | "yaml" | "csv" | "csv-set", ...} from a file the user picked (fromBytes
// for the first three)
export async function openFile(name, bytes) {
	if (startsWith(bytes, "PK\u0003\u0004")) {
		// a radio backup, or the editor's own .zip of CSV tables
		const entries = await unzip(bytes);
		if (!entries.has("backup.json")) {
			const dec = new TextDecoder();
			const files = [...entries].filter(([n]) => /\.csv$/i.test(n)).map(([n, d]) => ({ name: n.replace(/^.*\//, ""), text: dec.decode(d) }));
			if (!files.length) throw new FileError("unknown_file", fileFacts(name, bytes, { entries: entries.size, types: [...new Set([...entries.keys()].map(extOf))].slice(0, 10) }));
			return { kind: "csv-set", files };
		}
		const { meta, parts } = await openBundle(bytes);
		const spi = parts.get("spi-flash");
		if (!spi || spi.data.length < TFTP_SIZE) throw new FileError("backup_no_codeplug");
		return fromBytes(spi.data, meta?.radio?.board || null);
	}
	if (/\.ya?ml$/i.test(name)) return { kind: "yaml", text: new TextDecoder().decode(bytes) };
	if (/\.(csv|txt)$/i.test(name)) return { kind: "csv", text: new TextDecoder().decode(bytes) };
	if (startsWith(bytes, "OutSecurityBin")) throw new FileError("firmware_not_codeplug");
	// a codeplug image (0x120000), the USB network codeplug.bin (0xAEE60), a whole data flash dump, or a TYT MD-380
	// CPS file or memory image
	if (bytes.length >= TFTP_SIZE) return fromBytes(bytes);
	if (bytes.length === RDT_MD380 || bytes.length === 0x40000) {
		const f = fromBytes(bytes);
		if (f.kind === "converted") return f;
	}
	throw new FileError("unknown_file", fileFacts(name, bytes));
}

// The bytes Save writes: the image up to the end of its ID database
export function saveBytes(image) {
	return image.bytes.subarray(0, Math.max(CODEPLUG_SPAN, idDbTop(image.bytes)));
}

// codeplug.bin for the radio's TFTP server (USB network mode, docs/spec/tftp.md 5.2): the codeplug, and the ID database
// in its image layout when there is one, so one file carries both. The radio takes the database part only when the
// client announces the file's size
export function tftpBytes(image) {
	return image.bytes.slice(0, Math.max(TFTP_SIZE, idDbTop(image.bytes)));
}

export { recordSize };

// --- The draft: the working codeplug kept in this browser (IndexedDB), so a reload doesn't lose edits ---

const DB = "rfoundry-codeplug-editor", STORE = "draft";

function db() {
	return new Promise((resolve, reject) => {
		const req = indexedDB.open(DB, 1);
		req.onupgradeneeded = () => req.result.createObjectStore(STORE);
		req.onsuccess = () => resolve(req.result);
		req.onerror = () => reject(req.error);
	});
}

async function tx(mode, fn) {
	const d = await db();
	return new Promise((resolve, reject) => {
		const t = d.transaction(STORE, mode);
		const req = fn(t.objectStore(STORE));
		t.oncomplete = () => resolve(req?.result);
		t.onerror = () => reject(t.error);
	});
}

export async function saveDraft(draft) {
	try {
		await tx("readwrite", (s) => s.put(draft, "current"));
	} catch {
		// private window or storage blocked: the page works without a draft
	}
}

export async function loadDraft() {
	try {
		return (await tx("readonly", (s) => s.get("current"))) || null;
	} catch {
		return null;
	}
}

export async function clearDraft() {
	try {
		await tx("readwrite", (s) => s.delete("current"));
	} catch {
		// nothing kept
	}
}
