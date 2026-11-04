// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bringing the original firmware's channels over at the end of an install (docs/dev/web-flasher.md "Channels after
// an install"). A radio installed from its stock firmware still holds the stock codeplug in its memory chip, in a
// layout RadioFoundry does not read, so the radio would start without channels. The install converts it into the
// open-firmware layout (codeplug/stock_tyt.js, the same conversion as the Codeplug Editor's), writes the sectors that
// change through the CPS, and reads them back. TYT-family stock codeplugs are read from the memory chip after the
// install; AnyTone ones before it, through the stock firmware's PC mode (anytoneui.js), since RadioFoundry can't
// read them afterwards (codeplug/stock_anytone.js). On an HR_C7000 radio the stock codeplug stays in its own pages,
// below RadioFoundry's codeplug (codeplug/stock_baofeng.js): the install converts the read its check step took and
// writes it through the codeplug interface (writeConverted).

import { CODEPLUG_SPAN } from "./backup.js";
import { codeplugSectors } from "./restore.js";
import { classify } from "../codeplug/detect.js";
import { STOCK, detectStock, importStock } from "../codeplug/stock_tyt.js";
import { readAnytone, importAnytone } from "../codeplug/stock_anytone.js";
import { empty, encode, decode } from "../codeplug/model.js";
import { CodeplugImage } from "../codeplug/image.js";
import { writeRanges, VFOS } from "../codeplug/transport.js";
import { DM32_STOCK, detectStock as detectPaged, importStock as importPaged } from "../codeplug/stock_baofeng.js";

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

export { readAnytone };

// The codeplug readAnytone read, converted: {image, counts, notes} as convertStock gives them
export function convertAnytone(tables) {
	const m = empty();
	const { counts, notes } = importAnytone(m, tables);
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

// The stock codeplug in an HR_C7000 radio's data flash read (the install's check step reads it before the write, and
// RadioFoundry never writes there): {image, counts, notes} as convertStock gives them, or null when it holds none this
// page reads
export function convertPagedStock(bytes) {
	if (!bytes || detectPaged(bytes) !== DM32_STOCK) return null;
	const m = empty();
	const { counts, notes } = importPaged(m, bytes);
	return { image: encode(m), counts, notes };
}

const countsOf = (img) => {
	try {
		return classify(img.bytes).layout === "openfw" ? imageCounts(img.bytes) : null;
	} catch (e) {
		return null;
	}
};

// The converted codeplug written through the radio's codeplug interface (codeplug/transport.js CpsSerial, which adds
// the radio's base on the wire): the settings area claimed when another firmware's data is there, the radio's codeplug
// read, the bytes that differ written and read back, the radio's codeplug counted again. ask(counts): whether to
// replace a codeplug with channels already on the radio; false writes nothing. The VFO pair keeps the radio's (a read
// shows the live VFOs there). progress(what, done, total), what "reading" | "writing" | "checking".
// {written, ok, have}: have the counts the radio holds afterwards (before, when nothing was written)
export async function writeConverted(tr, radios, conv, { ask = async () => true, progress = () => {} } = {}) {
	await tr.identify(radios);
	if ((await tr.storage())?.region === "foreign" && !(await tr.claimStorage())) throw new Error("the settings area could not be claimed");
	const have = await tr.read({ iddb: false, progress: (d, n) => progress("reading", d, n) });
	const now = countsOf(have);
	if (now?.channels && !(await ask(now))) return { written: false, ok: false, have: now };
	const want = CodeplugImage.from(conv.image.bytes.slice(0, CODEPLUG_SPAN));
	want.bytes.set(have.bytes.subarray(VFOS[0], VFOS[1]), VFOS[0]);
	const res = await tr.write(want, have, { progress: (d, n) => progress("writing", d, n) });
	await tr.verify(want, res.plan, { progress: (d, n) => progress("checking", d, n) });
	await tr.finish();
	const got = countsOf(await tr.read({ iddb: false }));
	const ok = !!got && Object.keys(conv.counts).every((k) => got[k] === conv.counts[k]);
	return { written: true, ok, have: got };
}
