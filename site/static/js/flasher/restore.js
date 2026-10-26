// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Restoring a radio from a backup bundle (backup.js). What can be put back depends on what the bundle holds:
//
//   target "rfoundry"    the radio as it was when the backup was taken on RadioFoundry: the internal flash image exactly (RadioFoundry and
//                   its codec, through the bootloader), then the codeplug areas of the SPI flash (through the CPS,
//                   which only allows the codeplug areas)
//   target "openfw"   the other open firmware the radio ran before the install (a backup taken over its CPS,
//                   openfw.js): its internal flash image exactly, through the bootloader. The SPI flash is not written
//                   back (no command for that yet, as for "stock"): that firmware's own CPS can write its codeplug back
//                   from the bundle's spi-flash.bin
//   target "codeplug"  only the codeplug areas, through the CPS: the firmware stays as it is (a codeplug backup)
//   target "stock"  stock firmware back: the application from the user's stock file (in the bundle, or chosen
//                   again), through the bootloader. The SPI flash keeps what is in it; the bundle's copy shows what
//                   it was. Writing the whole SPI flash back needs a firmware command that doesn't exist yet
//                   (docs/dev/web-flasher.md "Restore"), so the flasher reports what differs instead of guessing.

import { RESTORE, makePlan } from "./plan.js";
import { stockApplication } from "./stockfw.js";
import { CODEPLUG_SPAN } from "./backup.js";
import { classify } from "../codeplug/detect.js";

export class RestoreError extends Error {}

export function targets(bundle) {
	const out = [];
	const internal = bundle.parts.get("internal-flash");
	const spi = bundle.parts.get("spi-flash");
	if (bundle.meta.firmware_now?.kind === "rfoundry" && internal?.complete) out.push("rfoundry");
	if (bundle.meta.firmware_now?.kind === "openfw" && internal?.complete) out.push("openfw");
	if (bundle.meta.firmware_now?.kind === "rfoundry" && spi && spi.data.length >= CODEPLUG_SPAN && codeplugLayout(bundle) === "openfw") out.push("codeplug");
	if (bundle.parts.has("stock-firmware") || bundle.meta.firmware_before?.kind === "stock") out.push("stock");
	return out;
}

// What the backup's codeplug areas hold (codeplug/detect.js): "openfw" for a RadioFoundry codeplug. A backup taken right after
// a first install still holds the original firmware's codeplug: written back as it is, it would be garbage to RadioFoundry, so
// only the Codeplug Editor (which converts it) takes it
export function codeplugLayout(bundle) {
	const spi = bundle.parts.get("spi-flash");
	return spi ? classify(spi.data).layout : null;
}

export function planInternal(bundle, board) {
	const p = bundle.parts.get("internal-flash");
	if (!p || !p.complete) throw new RestoreError("the bundle has no complete internal flash read");
	if (p.addr !== board.app_base) throw new RestoreError("the internal flash read does not start at the application base");
	return makePlan(board, [[p.addr, p.data]], RESTORE);
}

export function planStock(stockFile, key, board, fb) {
	const [addr, app] = stockApplication(stockFile, key, board, fb);
	return makePlan(board, [[addr, app]], RESTORE);
}

// The 4 KB sectors of the codeplug areas whose bytes differ between the radio and the backup: [addr, bytes] (emulated
// EEPROM 0x00000-0x0FFFF and the open-firmware flash image 0x20000-0x11FFFF; never the calibration and RadioFoundry's own areas
// at 0x10000-0x1FFFF, which the CPS refuses anyway)
export function codeplugSectors(backupSpi, radioSpi) {
	const out = [];
	for (let a = 0; a < CODEPLUG_SPAN; a += 4096) {
		if (a >= 0x10000 && a < 0x20000) continue;
		const want = backupSpi.subarray(a, a + 4096);
		const have = radioSpi.subarray(a, a + 4096);
		if (want.length < 4096) break;
		let same = have.length === 4096;
		for (let i = 0; same && i < 4096; i++) same = want[i] === have[i];
		if (!same) out.push([a, want]);
	}
	return out;
}

export async function writeCodeplug(link, sectors, progress = () => {}) {
	const refused = [];
	let done = 0;
	for (const [addr, data] of sectors) {
		if (!await link.cpsWriteSector(addr, data)) refused.push(addr);
		progress(++done, sectors.length);
	}
	await link.cpsClose();
	return refused;
}
