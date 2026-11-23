// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A CHIRP image file (.img) of a Baofeng UV-5R into the editing model. An image is a radio's memory followed by a
// footer CHIRP adds: a marker and a base64 JSON object naming the radio. The memory layout is learned from the
// bytes of an image and the CSV CHIRP wrote from it (never from CHIRP's source), and only what the pair confirms is
// read: the channel memories and their names. Other radios' images have other layouts and are refused by name.
//
// Memory (little endian), 128 channels:
//   0x0008 + 16 n  channel n: RX and TX frequency (4 bytes each, BCD in 10 Hz units, low digits first; RX all 0xFF =
//                  unused, TX all 0xFF = no transmit), RX tone and TX tone (2 bytes each), 4 flag bytes
//   0x1008 + 16 n  its name, 0xFF padded
// A tone is 0 or 0xFFFF for none, 600 to 3000 for a CTCSS tone in tenths of a hertz, or 1 to 104 for the DCS code
// at that place in the standard code list (below; the samples confirm places 53 and 65). Flags: byte 2 bit 0 = low
// power; byte 3 bit 6 = wide, bit 2 = in
// the scan list. Reversed DCS polarity is not seen in the sample, so it is not read.

import { importRows, ChirpError } from "./chirp.js";
import { DCS } from "./fields.js";

const MARK = [0x63, 0x68, 0x69, 0x72, 0x70, 0xEE, 0x69, 0x6D, 0x67, 0x00];	// "chirp", 0xEE, "img", 0
const MEM = 0x194A;
const CHANNELS = 128;


// The offset of the footer marker, or -1. Searched from the end of the whole file: another radio's image is larger, and
// is refused by the model its footer names
function footerAt(b) {
	outer: for (let i = b.length - MARK.length; i >= 0; i--) {
		for (let j = 0; j < MARK.length; j++) if (b[i + j] !== MARK[j]) continue outer;
		return i;
	}
	return -1;
}

export function isChirpImg(b) {
	return footerAt(b) >= 0;
}

// The radio the footer names: {model, vendor, rclass}
function footer(b, at) {
	let json = "";
	try {
		const text = String.fromCharCode(...b.subarray(at + MARK.length + 1)).replace(/[^A-Za-z0-9+/=]/g, "");
		json = atob(text);
		return JSON.parse(json);
	} catch {
		throw new ChirpError("img_footer");
	}
}

const bcd = (b, o) => {
	let hz = 0;
	for (let i = 3; i >= 0; i--) {
		const hi = b[o + i] >> 4, lo = b[o + i] & 15;
		if (hi > 9 || lo > 9) return null;
		hz = hz * 100 + hi * 10 + lo;
	}
	return hz * 10;
};

const u16 = (b, o) => b[o] | (b[o + 1] << 8);

// A tone word as the model's tone text; null if not one the radio can use
function tone(v) {
	if (v === 0 || v === 0xFFFF) return "";
	if (v >= 600 && v <= 3000) return (v / 10).toFixed(1);
	if (v >= 1 && v <= DCS.length) return `D${String(DCS[v - 1]).padStart(3, "0")}N`;
	return null;
}

// Add the image's channels to m as importChirp does a CSV's: {counts, notes}
export function importChirpImg(m, bytes, zoneName = "CHIRP") {
	const at = footerAt(bytes);
	if (at < 0) throw new ChirpError("not_chirp");
	const info = footer(bytes, at);
	if (info.model !== "UV-5R") {
		throw new ChirpError("img_model", { model: [info.vendor, info.model].filter(Boolean).join(" ") || "?" });
	}
	// The model's name, but saved by CHIRP as another radio class (an alias for a rebadged radio): its layout is unconfirmed
	if (info.rclass !== "BaofengUV5RGeneric") throw new ChirpError("img_class", { model: info.model, rclass: String(info.rclass ?? "?") });
	// The right radio, but its memory isn't the size the layout has: cut short, or changed by hand
	if (at !== MEM) throw new ChirpError("img_size", { model: info.model });
	const notes = new Map();
	const rows = [];
	for (let n = 0; n < CHANNELS; n++) {
		const o = 8 + 16 * n;
		const rx = bcd(bytes, o), tx = bcd(bytes, o + 4);
		if (rx === null || !rx) {
			if (bytes[o] !== 0xFF) notes.set("bad_frequency", (notes.get("bad_frequency") || 0) + 1);
			continue;
		}
		const a = tone(u16(bytes, o + 8)), b = tone(u16(bytes, o + 10));
		let name = "";
		for (let i = 0; i < 16 && bytes[0x1008 + 16 * n + i] !== 0xFF && bytes[0x1008 + 16 * n + i] !== 0; i++) name += String.fromCharCode(bytes[0x1008 + 16 * n + i]);
		const flags = bytes[o + 14], f3 = bytes[o + 15];
		// TX all 0xFF: transmit off, read as CHIRP's duplex "off" (RX only), never as simplex
		const txOff = bytes.subarray(o + 4, o + 8).every((x) => x === 0xFF);
		// CSV columns the shared reader takes; the offset column holds the TX frequency for a split
		rows.push({
			name, frequency: (rx / 1e6).toFixed(6), duplex: txOff ? "off" : tx === rx ? "" : "split",
			offset: tx === null ? "" : (tx / 1e6).toFixed(6), mode: f3 & 0x40 ? "FM" : "NFM", skip: f3 & 0x04 ? "" : "S",
			power: flags & 1 ? "1.0W" : "4.0W", _rx: a ?? "", _tx: b ?? "",
		});
		if (a === null || b === null) notes.set("bad_tone", (notes.get("bad_tone") || 0) + 1);
	}
	return importRows(m, rows, zoneName, notes);
}
