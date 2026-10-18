// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug files for the editor: what a file holds and the image in it (a codeplug image, a radio backup bundle, the
// USB network codeplug.bin), and the draft the page keeps in the browser between visits.

import { CodeplugImage, CODEPLUG_SPAN, TFTP_SIZE } from "../codeplug/image.js";
import { readHeader, recordSize, AREA1, AREA1_SIZE, HEADER, AREA2 } from "../codeplug/iddb.js";
import { openBundle } from "../flasher/backup.js";
import { unzip } from "../flasher/zip.js";
import { startsWith } from "../flasher/bytes.js";

export class FileError extends Error {}

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

// {kind: "image" | "yaml" | "csv" | "csv-set", image?, text?, files?} from a file the user picked
export async function openFile(name, bytes) {
	if (startsWith(bytes, "PK\u0003\u0004")) {
		// a radio backup, or the editor's own .zip of CSV tables
		const entries = await unzip(bytes);
		if (!entries.has("backup.json")) {
			const dec = new TextDecoder();
			const files = [...entries].filter(([n]) => /\.csv$/i.test(n)).map(([n, d]) => ({ name: n.replace(/^.*\//, ""), text: dec.decode(d) }));
			if (!files.length) throw new FileError("unknown_file");
			return { kind: "csv-set", files };
		}
		const { meta, parts } = await openBundle(bytes);
		const spi = parts.get("spi-flash");
		if (!spi || spi.data.length < TFTP_SIZE) throw new FileError("backup_no_codeplug");
		return { kind: "image", image: imageFrom(spi.data), board: meta?.radio?.board || null };
	}
	if (/\.ya?ml$/i.test(name)) return { kind: "yaml", text: new TextDecoder().decode(bytes) };
	if (/\.(csv|txt)$/i.test(name)) return { kind: "csv", text: new TextDecoder().decode(bytes) };
	if (startsWith(bytes, "OutSecurityBin")) throw new FileError("firmware_not_codeplug");
	// a codeplug image (0x120000), the USB network codeplug.bin (0xAEE60) or a whole data flash dump
	if (bytes.length >= TFTP_SIZE) return { kind: "image", image: imageFrom(bytes) };
	throw new FileError("unknown_file");
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

const DB = "odr-codeplug-editor", STORE = "draft";

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
