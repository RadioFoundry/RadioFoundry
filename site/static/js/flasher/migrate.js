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
import { writeRanges, VFOS } from "../codeplug/transport.js";

// The stock family a memory chip read holds, or null (a RadioFoundry codeplug, a blank one, or one not read here)
export function stockLayoutOf(bytes) {
	const { layout } = classify(bytes);
	return STOCK[layout] ? layout : null;
}

// How much of the memory chip the conversion reads: the codeplug span RadioFoundry writes, and all of the stock tables
export const readSpan = (layout) => Math.max(CODEPLUG_SPAN, layout ? STOCK[layout].size : 0);

// Whether the step reads the radio at all. Not after an update or a move from another open firmware (before: the
// "current firmware" choice, explain.js nowChoice): the codeplug there is already in the open-firmware layout, and stray
// stock bytes must never replace it. Not on a radio without a memory chip (spiSize 0). And not when this visit's backup
// read every stock family's tables and found no stock codeplug (saved: its memory chip read), since an install
// writes none: the read would only cost time on a radio that never had one
export function channelsWanted({ before, spiSize, saved }) {
	if (before === "rfoundry" || before === "openfw" || !spiSize) return false;
	const all = Math.max(...Object.keys(STOCK).map(readSpan));
	return !(saved && saved.length >= all && !stockLayoutOf(saved));
}

// What to convert: the memory chip read from the backup this visit took, when it holds a stock codeplug, else the
// live read. The backup's read comes first because the backup ends by claiming RadioFoundry's settings area, and on
// some families the stock tables (zones on the 256 KB ones) lie inside it: the live read no longer has them.
// {bytes, layout, from: "backup" | "radio"}; layout null when neither holds a stock codeplug
export function stockSource(live, saved) {
	const fromSaved = savedStock(saved);
	if (fromSaved) return { bytes: saved.subarray(0, readSpan(fromSaved)), layout: fromSaved, from: "backup" };
	return { bytes: live, layout: stockLayoutOf(live), from: "radio" };
}

// The stock family of a backup's memory chip read when it holds all of that family's tables, else null
export function savedStock(saved) {
	const layout = saved ? stockLayoutOf(saved) : null;
	return layout && saved.length >= readSpan(layout) ? layout : null;
}

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

// The sectors to write: the radio's bytes with the converted image laid over the codeplug's regions, where they differ
// from the radio's (restore.js codeplugSectors). Outside the regions the radio keeps its bytes, as a Codeplug Editor
// write leaves them (codeplug/transport.js): the radio's own settings block in sector 0x6000, the ID database area,
// and whatever stock data the regions don't reach. The VFO pair keeps the radio's too: a read shows the live VFOs
// there, in the codeplug's format, and the converted image has none
export function migrationSectors(image, radio, protect = []) {
	const want = radio.slice(0, CODEPLUG_SPAN);
	for (const [a, b] of writeRanges()) want.set(image.subarray(a, b), a);
	want.set(radio.subarray(VFOS[0], VFOS[1]), VFOS[0]);
	return codeplugSectors(want, radio, protect);
}

// After the write: the addresses of written sectors whose bytes the radio does not hold, and whether the radio's
// codeplug counts what the conversion made. Either one wrong means the radio did not take the codeplug
export function checkMigration(sectors, readBack, want) {
	const differ = [];
	for (const [addr, data] of sectors) {
		// the VFO pair reads as the radio's live VFOs (open-firmware-cps.md 2.2), not as the bytes written
		const got = readBack.subarray(addr, addr + data.length);
		const vfo = (i) => addr + i >= VFOS[0] && addr + i < VFOS[1];
		if (got.length !== data.length || got.some((x, i) => x !== data[i] && !vfo(i))) differ.push(addr);
	}
	let have = null;
	try {
		have = imageCounts(readBack);
	} catch (e) { /* unreadable: counts as a mismatch */ }
	const counted = !!have && Object.keys(want).every((k) => have[k] === want[k]);
	return { ok: !differ.length && counted, differ, have };
}
