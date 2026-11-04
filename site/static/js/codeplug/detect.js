// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What layout a codeplug image or SPI flash dump holds (docs/spec/codeplug-stock-tyt.md 2): the open-firmware layout
// (open-firmware-cps.md 7), a stock TYT-family codeplug, nothing (blank), or something else. Decided from the
// records each layout marks as in use, so stock bytes RadioFoundry hasn't overwritten yet don't hide a RadioFoundry codeplug.

import {
	LIMITS, CH_SIZE, bankBitmap, channelAddr, decodeChannel, zoneLayout, ZONE_BITMAP, ZONE_DATA, CONTACTS,
	CONTACT_SIZE, decodeContact, LIST_COUNTS, LISTS, LIST_SIZE, GENERAL,
} from "./records.js";
import { getBcdId } from "./fields.js";
import { STOCK, stockBytes, stockEvidence, stockDmrId, RDT_MD380, RDT_UV380 } from "./stock_tyt.js";

// A Latin-1 name field: {text, ok}; ok when the text is printable and only 0x00 or 0xFF follow it
export function latinName(b, off, len) {
	let text = "", ok = true, end = false;
	for (let i = 0; i < len; i++) {
		const c = b[off + i];
		if (end) {
			if (c !== 0x00 && c !== 0xFF) ok = false;
		} else if (c === 0x00 || c === 0xFF) end = true;
		else {
			if (c < 0x20 || (c > 0x7E && c < 0xA0)) ok = false;
			text += String.fromCharCode(c);
		}
	}
	return { text, ok };
}

// Good and bad in-use records of the open-firmware layout (spec section 2, step 1)
export function openfwEvidence(b) {
	let good = 0, bad = 0;
	const named = (n) => (n.ok && n.text ? good++ : bad++);
	const has = (a, len) => a + len <= b.length;
	// erased flash: an in-use bit over an all-0xFF record is a never-written area, not a bad record
	const erased = (a, len) => b.subarray(a, a + len).every((x) => x === 0xFF);

	if (has(GENERAL, 12)) {
		const call = latinName(b, GENERAL, 8);
		if (!call.ok) bad++;
		else if (call.text && getBcdId(b, GENERAL + 8)) good++;
	}
	for (let n = 1; n <= LIMITS.channels; n++) {
		const bank = Math.floor((n - 1) / 128), k = (n - 1) % 128;
		const bm = bankBitmap(bank) + (k >> 3);
		if (!has(channelAddr(n), CH_SIZE) || !(b[bm] & (1 << (k & 7))) || erased(channelAddr(n), CH_SIZE)) continue;
		const r = b.subarray(channelAddr(n), channelAddr(n) + CH_SIZE);
		if (decodeChannel(r)) named(latinName(r, 0, 16));
		else bad++;
	}
	if (has(ZONE_DATA, LIMITS.zones * 0xB0)) {
		const rec = 16 + 2 * zoneLayout(b);
		for (let slot = 0; slot < LIMITS.zones; slot++) {
			const a = ZONE_DATA + slot * rec;
			if ((b[ZONE_BITMAP + (slot >> 3)] & (1 << (slot & 7))) && !erased(a, rec)) named(latinName(b, a, 16));
		}
	}
	for (let n = 1; n <= LIMITS.contacts; n++) {
		const a = CONTACTS + (n - 1) * CONTACT_SIZE;
		if (!has(a, CONTACT_SIZE)) break;
		if (decodeContact(b.subarray(a, a + CONTACT_SIZE))) named(latinName(b, a, 16));
	}
	for (let n = 1; n <= LIMITS.rxLists; n++) {
		const a = LISTS + (n - 1) * LIST_SIZE;
		if (!has(a, LIST_SIZE)) break;
		const count = b[LIST_COUNTS + n - 1];
		if (count === 0 || count === 0xFF) continue;
		if (count > LIMITS.rxListContacts + 1) bad++;
		else named(latinName(b, a, 16));
	}
	return { good, bad };
}

const NAMES = {
	"tyt-uv380": "Stock TYT MD-UV380 family codeplug (DM-1701, RT84, MD-UV380, MD-UV390, RT3S, MD-2017)",
	"tyt-md380": "Stock TYT MD-380 family codeplug (MD-380, MD-390, RT3)",
	"tyt-uv380-rdt": "TYT MD-UV380 family CPS file (.rdt)",
	unknown: "Unknown codeplug layout",
};
export const formatName = (f) => NAMES[f] || f;

// {layout: "openfw" | "blank" | "tyt-uv380" | "tyt-md380" | "tyt-uv380-rdt" | "unknown", good, bad}
export function classify(bytes) {
	if (bytes.length === RDT_UV380) return { layout: "tyt-uv380-rdt", good: 0, bad: 0 };
	const ev = bytes.length === RDT_MD380 ? { good: 0, bad: 0 } : openfwEvidence(bytes);
	const sb = stockBytes(bytes);
	const stock = () => {
		for (const format of Object.keys(STOCK)) {
			const st = stockEvidence(sb, format);
			if (st.good && st.bad * 4 <= st.good) return { layout: format, ...st };
		}
		return null;
	};
	if (ev.good && ev.bad * 4 <= ev.good) {
		// A radio put back on its stock firmware after RadioFoundry keeps RadioFoundry's records where the stock codeplug
		// doesn't reach, but both layouts keep their general settings in the chip's first sectors: a stock DMR ID beside
		// none of RadioFoundry's means the stock software wrote last (spec section 2, step 1)
		const back = !(bytes.length >= GENERAL + 12 && getBcdId(bytes, GENERAL + 8)) && stockDmrId(sb) ? stock() : null;
		return back || { layout: "openfw", ...ev };
	}
	return stock() || { layout: ev.good || ev.bad ? "unknown" : "blank", ...ev };
}
