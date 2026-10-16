// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Backup bundles (odr-backup/1): everything the flasher could read from a radio, in one zip the user keeps. A bundle
// is what "restore" takes to put the radio back the way it was (docs/user/02-installing.md, docs/dev/website.md).
//
//   backup.json              what's in the bundle: the radio, its firmware before and now, each part's address,
//                            size, SHA-256 and whether the read was complete
//   internal-flash.bin       the MCU flash from the application base (the bootloader is never read or written)
//   spi-flash.bin            the external SPI flash: codeplug, calibration, the stock firmware's resources
//   security-registers.bin   the SPI flash's security registers (the factory calibration on the DM-1701)
//   radio-info.bin           the CPS radio info block (open-firmware-cps.md 4)
//   stock-firmware.bin       the user's stock firmware file, when they gave one (puts stock firmware back)
//   README.txt               the same in words
//
// Nothing here leaves the user's computer: the bundle is built in the page and saved as a download.

import { hex, sha256 } from "./bytes.js";
import { zip, unzip } from "./zip.js";

export const FORMAT = "odr-backup/1";
const CPS_SPI = 1, CPS_MCU = 5, CPS_INFO = 9, CPS_SECURITY = 10;
export const CODEPLUG_SPAN = 0x120000;	// the open-firmware layout: emulated EEPROM, calibration, codeplug, ID database

export class BackupError extends Error {}

// Read a radio that runs ODR. what: {internal, spi: "full" | "codeplug" | "none", security}. Returns the parts.
export async function readRadio(link, board, radio, { what = {}, progress = () => {}, signal } = {}) {
	const parts = [];
	const want = { internal: true, spi: "full", security: true, ...what };
	const step = (name) => (done, total) => progress(name, done, total);
	const info = await link.cpsRead(CPS_INFO, 0, 128);
	if (info) parts.push({ file: "radio-info.bin", what: "radio-info", addr: 0, data: info, complete: true });
	if (want.internal) {
		// CPS area 5 addresses the MCU flash from 0x08000000 (open-firmware-cps.md 3)
		const base = board.app_base, size = board.flash_end - board.app_base;
		const r = await link.cpsDump(CPS_MCU, base - 0x08000000, size, { progress: step("internal"), signal });
		parts.push({ file: "internal-flash.bin", what: "internal-flash", addr: base, data: r.data, complete: r.complete });
	}
	if (want.spi !== "none") {
		const size = want.spi === "full" ? radio.backup.spi_size : CODEPLUG_SPAN;
		const r = await link.cpsDump(CPS_SPI, 0, size, { progress: step("spi"), signal });
		parts.push({ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: r.data, complete: r.complete && want.spi === "full" });
	}
	if (want.security) {
		const regs = [];
		for (const a of [0x1000, 0x2000, 0x3000]) {
			const d = await link.cpsRead(CPS_SECURITY, a, 256);
			if (d) regs.push(d);
		}
		if (regs.length) {
			const data = new Uint8Array(regs.reduce((n, d) => n + d.length, 0));
			let o = 0;
			for (const d of regs) { data.set(d, o); o += d.length; }
			parts.push({ file: "security-registers.bin", what: "security-registers", addr: 0x1000, data, complete: regs.length === 3 });
		}
	}
	await link.cpsClose();
	return parts;
}

// meta: {radio: {board, name, chip, version}, firmware_now, firmware_before, notes}
export async function makeBundle(meta, parts, site = {}) {
	const files = [];
	const listed = [];
	for (const p of parts) {
		listed.push({ file: p.file, what: p.what, addr: hex(p.addr), size: p.data.length, sha256: await sha256(p.data), complete: p.complete });
		files.push({ name: p.file, data: p.data });
	}
	const doc = { format: FORMAT, created: new Date().toISOString(), tool: { site: site.git || "", url: site.url || "" }, ...meta, parts: listed };
	const readme = [
		`OpenDigitalRadio backup of a ${meta.radio.name}, ${doc.created}`,
		meta.radio.chip ? `Chip UID ${meta.radio.chip.uid}` : "",
		`Firmware when the backup was taken: ${describeFirmware(meta.firmware_now)}`,
		meta.firmware_before ? `Firmware before ODR was installed: ${describeFirmware(meta.firmware_before)}` : "",
		"",
		"Keep this file. The OpenDigitalRadio radio programming utility (Restore) can restore the radio from it.",
		"It contains your radio's calibration and codeplug. Do not share it.",
		"",
		...listed.map((p) => `${p.file}: ${p.what} at ${p.addr}, ${p.size} bytes${p.complete ? "" : " (incomplete read)"}, sha256 ${p.sha256}`),
	].filter((l, i, a) => l !== "" || a[i - 1] !== "").join("\n") + "\n";
	return zip([{ name: "backup.json", data: JSON.stringify(doc, null, 1) }, { name: "README.txt", data: readme }, ...files]);
}

export function describeFirmware(f) {
	if (!f) return "unknown";
	if (f.kind === "odr") return `OpenDigitalRadio ${f.git || ""} (codec ${f.codec || "?"})`.trim();
	if (f.kind === "stock") return `stock ${f.version || "(version unknown)"}`;
	return "unknown";
}

// The name people see in their downloads or on the desktop: the model and the local date, letters and digits only,
// always .zip (DM1801Pro_Backup_2026-10-16.zip)
export function bundleName(meta, when = new Date()) {
	const p = (n) => String(n).padStart(2, "0");
	const model = String(meta.radio.model || meta.radio.board).replace(/[^A-Za-z0-9]/g, "") || "Radio";
	return `${model}_Backup_${when.getFullYear()}-${p(when.getMonth() + 1)}-${p(when.getDate())}.zip`;
}

// Read a bundle back; every part checked against its SHA-256. Returns {meta, parts: Map(what -> {..., data})}
export async function openBundle(bytes) {
	let files;
	try {
		files = await unzip(bytes);
	} catch (e) {
		throw new BackupError(`this file is not a backup created by the radio programming utility (${e.message})`);
	}
	const doc = files.get("backup.json");
	if (!doc) throw new BackupError("this file is not a backup created by the radio programming utility (no backup.json)");
	const meta = JSON.parse(new TextDecoder().decode(doc));
	if (meta.format !== FORMAT) throw new BackupError(`unknown backup format ${meta.format}`);
	const parts = new Map();
	for (const p of meta.parts) {
		const data = files.get(p.file);
		if (!data) throw new BackupError(`${p.file} is missing from the backup file`);
		if (await sha256(data) !== p.sha256) throw new BackupError(`${p.file} does not match its checksum; the backup file is damaged`);
		parts.set(p.what, { ...p, addr: Number(p.addr), data });
	}
	return { meta, parts };
}
