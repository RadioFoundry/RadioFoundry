// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bringing the original firmware's channels over at the end of an install (docs/dev/web-flasher.md "Channels after
// an install"). A radio installed from its stock firmware still holds the stock codeplug in its memory chip, in a
// layout RadioFoundry does not read, so the radio would start without channels. The install converts it into the
// open-firmware layout (codeplug/stock_tyt.js, the same conversion as the Codeplug Editor's), writes the sectors that
// change through the CPS, and reads them back. Only TYT-family stock codeplugs convert in the browser so far.

import { CODEPLUG_SPAN } from "./backup.js";
import { codeplugSectors } from "./restore.js";
import { classify } from "../codeplug/detect.js";
import { STOCK, detectStock, importStock } from "../codeplug/stock_tyt.js";
import { empty, encode, decode } from "../codeplug/model.js";
import { CodeplugImage } from "../codeplug/image.js";

// The stock family a memory chip read holds, or null (a RadioFoundry codeplug, a blank one, or one not read here)
export function stockLayoutOf(bytes) {
	const { layout } = classify(bytes);
	return STOCK[layout] ? layout : null;
}

// How much of the memory chip the conversion reads: the codeplug span RadioFoundry writes, and all of the stock tables
export const readSpan = (layout) => Math.max(CODEPLUG_SPAN, layout ? STOCK[layout].size : 0);

// The converted codeplug: {image, counts, notes}. counts as the editing model holds them (importStock), notes the
// editor's import note codes (what was cut or dropped)
export function convertStock(bytes, layout) {
	if (!layout || detectStock(bytes) !== layout) throw new Error("no stock codeplug this page can read");
	const m = empty();
	const { counts, notes } = importStock(m, bytes, layout);
	return { image: encode(m).bytes.slice(0, CODEPLUG_SPAN), counts, notes };
}

// What a RadioFoundry codeplug image holds, counted the way convertStock counts
export function imageCounts(bytes) {
	const m = decode(CodeplugImage.from(bytes.subarray(0, CODEPLUG_SPAN)));
	return { channels: m.channels.length, contacts: m.contacts.length, rxLists: m.rxLists.length, zones: m.zones.length };
}

// The sectors to write: those of the converted image that differ from the radio's bytes (restore.js codeplugSectors)
export const migrationSectors = (image, radio, protect = []) => codeplugSectors(image, radio, protect);

// After the write: the addresses of written sectors whose bytes the radio does not hold, and whether the radio's
// codeplug counts what the conversion made. Either one wrong means the radio did not take the codeplug
export function checkMigration(sectors, readBack, want) {
	const differ = [];
	for (const [addr, data] of sectors) {
		const got = readBack.subarray(addr, addr + data.length);
		if (got.length !== data.length || got.some((x, i) => x !== data[i])) differ.push(addr);
	}
	let have = null;
	try {
		have = imageCounts(readBack);
	} catch (e) { /* unreadable: counts as a mismatch */ }
	const counted = !!have && Object.keys(want).every((k) => have[k] === want[k]);
	return { ok: !differ.length && counted, differ, have };
}
