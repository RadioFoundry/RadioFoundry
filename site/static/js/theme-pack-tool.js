// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 RadioFoundry Theme Pack contributors
import {
	Link, SerialStream, RFOUNDRY_USB, browserErrorKey, probe, serialApi, serialSupported,
} from "./flasher/link.js";
import { page } from "./i18n.js";

const { t } = page;
const COMMAND = 0x29;
const OP = Object.freeze({ INFO: 0, BEGIN: 1, DATA: 2, COMMIT: 3, REMOVE: 4 });
const filters = [RFOUNDRY_USB, ...[0x0403, 0x1A86, 0x10C4, 0x067B].map((usbVendorId) => ({ usbVendorId }))];
const fileInput = document.getElementById("theme-pack-file");
const installButton = document.getElementById("theme-pack-install");
const removeButton = document.getElementById("theme-pack-remove");
const status = document.getElementById("theme-pack-status");
let pack = null;

function crc32(bytes) {
	let crc = 0xFFFFFFFF;
	for (const byte of bytes) {
		crc ^= byte;
		for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return (crc ^ 0xFFFFFFFF) >>> 0;
}

function inspect(bytes) {
	if (bytes.length < 20 || bytes.length > 4096) throw new Error(t("tools.theme_pack.status.invalid"));
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	if (String.fromCharCode(...bytes.subarray(0, 4)) !== "RFTP" || view.getUint16(4, true) !== 1 ||
	    view.getUint16(6, true) !== 20 || view.getUint16(14, true) !== 0)
		throw new Error(t("tools.theme_pack.status.invalid"));
	const payloadSize = view.getUint32(8, true), count = view.getUint16(12, true);
	if (count < 1 || count > 10 || payloadSize !== count * 60 || bytes.length !== 20 + payloadSize ||
	    crc32(bytes.subarray(20)) !== view.getUint32(16, true))
		throw new Error(t("tools.theme_pack.status.invalid"));
	const decoder = new TextDecoder("utf-8", { fatal: true });
	for (let i = 0; i < count; i++) {
		const at = 20 + i * 60, nameLength = bytes[at];
		if (nameLength < 1 || nameLength > 23 || bytes.subarray(at + 1 + nameLength, at + 24).some((b) => b !== 0))
			throw new Error(t("tools.theme_pack.status.invalid"));
		const name = decoder.decode(bytes.subarray(at + 1, at + 1 + nameLength));
		if (!name || /[\u0000-\u001F\u007F]/u.test(name)) throw new Error(t("tools.theme_pack.status.invalid"));
	}
	return { bytes, count };
}

function put16(payload, offset, value) { new DataView(payload.buffer).setUint16(offset, value, true); }

async function radioInfo(link) {
	const reply = await link.request(COMMAND, new Uint8Array([OP.INFO]));
	if (reply.length !== 4 || reply[0] !== 1) throw new Error(t("tools.theme_pack.status.unsupported"));
	return { capacity: new DataView(reply.buffer, reply.byteOffset, reply.byteLength).getUint16(1, true), count: reply[3] };
}

async function run(action) {
	let stream;
	for (const control of [fileInput, installButton, removeButton]) control.disabled = true;
	try {
		status.textContent = t("tools.theme_pack.status.connecting");
		if (!serialSupported()) throw new Error(t("tools.theme_pack.status.no_serial"));
		stream = await SerialStream.open(await serialApi().requestPort({ filters }));
		const link = new Link(stream);
		if (!await probe(link)) throw new Error(t("tools.theme_pack.status.radio_not_found"));
		const info = await radioInfo(link);
		if (action === "remove") {
			await link.request(COMMAND, new Uint8Array([OP.REMOVE]));
			status.textContent = t("tools.theme_pack.status.removed");
			return;
		}
		if (!pack || pack.bytes.length > info.capacity) throw new Error(t("tools.theme_pack.status.invalid"));
		const begin = new Uint8Array([OP.BEGIN, 0, 0]);
		put16(begin, 1, pack.bytes.length);
		await link.request(COMMAND, begin);
		status.textContent = t("tools.theme_pack.status.writing");
		for (let offset = 0; offset < pack.bytes.length;) {
			const size = Math.min(61, pack.bytes.length - offset);
			const payload = new Uint8Array(3 + size);
			payload[0] = OP.DATA;
			put16(payload, 1, offset);
			payload.set(pack.bytes.subarray(offset, offset + size), 3);
			await link.request(COMMAND, payload);
			offset += size;
		}
		status.textContent = t("tools.theme_pack.status.verifying");
		await link.request(COMMAND, new Uint8Array([OP.COMMIT]));
		const after = await radioInfo(link);
		if (after.count !== pack.count) throw new Error(t("tools.theme_pack.status.verify_failed"));
		status.textContent = t("tools.theme_pack.status.done", { count: pack.count });
	} catch (error) {
		const browserKey = browserErrorKey(error);
		status.textContent = error.status === 1 || error.status === 4 || error.message === t("tools.theme_pack.status.unsupported")
			? t("tools.theme_pack.status.unsupported")
			: t("tools.theme_pack.status.failed", {
				error: browserKey ? t("tools.theme_pack.status.port_error") : error.message,
			});
	} finally {
		if (stream) await stream.close();
		fileInput.disabled = false;
		installButton.disabled = pack === null;
		removeButton.disabled = false;
	}
}

fileInput.addEventListener("change", async () => {
	const file = fileInput.files?.[0];
	pack = null;
	installButton.disabled = true;
	if (!file) return;
	try {
		pack = inspect(new Uint8Array(await file.arrayBuffer()));
		installButton.disabled = false;
		status.textContent = t("tools.theme_pack.status.ready", { count: pack.count });
	} catch (error) {
		status.textContent = error.message;
	}
});

installButton.addEventListener("click", () => run("install"));
removeButton.addEventListener("click", () => run("remove"));
