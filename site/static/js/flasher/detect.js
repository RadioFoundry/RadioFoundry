// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Which hardware variant a radio is (board.toml [hardware] variants), asked of the radio itself: one port, then each of
// the backend's variant probes in turn until one names the variant. A probe speaks one way the radio can answer (RadioFoundry's debug
// link, a bootloader's identify, the original firmware's PC mode) and maps the answer to a variant code. A backend
// lists its probes, in the order that is safe for its radios (backends.js variantProbes); a new protocol is a new
// probe that calls registerProbe(). No DOM here, so the Node tests can use it. docs/dev/web-flasher.md "Variant
// detection".
//
// A probe: {id, baud, run(stream, ctx)}. run returns null when the radio didn't answer that way, else
//   {code, version, via, keep, note}: code the variant code (null: it answered but doesn't say), version the text it
//   reported, via a word for S().variant.via, keep true when the radio stays in a session the backend can take over
//   (the stream is then handed back, not closed), note an i18n key for what the user does next.
// ctx: {radio (radios.json entry), board (its flash-boards.json entry), fb (flash-boards.json), log(line)}. A probe that
// sends anything that changes the radio's state (a restart) says so in its note.

import { Link, probe as linkProbe } from "./link.js";

const PROBES = new Map();

export function registerProbe(p) {
	if (!p?.id || typeof p.run !== "function") throw new Error(`variant probe ${p?.id || "?"}: needs id and run`);
	PROBES.set(p.id, p);
	return p;
}

export const probeOf = (id) => PROBES.get(id) || null;

// The variant whose image is board `boardId`, when exactly one variant takes it (several sharing one image can't be
// told apart by the image the radio runs)
export function variantOfBoard(radio, boardId) {
	const vs = (radio?.variants || []).filter((v) => v.runs?.board === boardId);
	return vs.length === 1 ? vs[0].code : null;
}

// Run the probes `ids` in order on streams from open(baud) (the same stream again for the same baud; the caller's
// open reopens the port at another). Stops at the first probe that names a variant. Returns {found, tried}: found the
// probe's result plus {id, stream} (stream only when keep), or null; tried [{id, outcome: "found" | "answered" |
// "silent" | "error", error}] for the report. Every stream not kept is closed.
export async function detectVariant({ ids, radio, board, fb, open, log = () => {} }) {
	const tried = [];
	let stream = null, baud = null, found = null;
	const close = async () => {
		if (stream) try { await stream.close(); } catch { /* gone */ }
		stream = null;
		baud = null;
	};
	for (const id of ids) {
		const p = probeOf(id);
		if (!p) {
			tried.push({ id, outcome: "error", error: "no such probe" });
			continue;
		}
		try {
			if (!stream || stream.lost || (p.baud && p.baud !== baud)) {
				await close();
				stream = await open(p.baud || null);
				baud = p.baud || null;
			}
			const r = await p.run(stream, { radio, board, fb, log });
			if (!r) {
				tried.push({ id, outcome: "silent" });
				continue;
			}
			tried.push({ id, outcome: r.code ? "found" : "answered", code: r.code || null });
			if (r.code || r.stop) {
				found = { ...r, id, stream: r.keep ? stream : null };
				if (r.keep) stream = null;	// the caller's now
				break;
			}
		} catch (e) {
			tried.push({ id, outcome: "error", error: String(e?.message || e) });
			log(`${id}: ${e?.message || e}`);
			if (stream?.lost) await close();
		}
	}
	await close();
	return { found, tried };
}

// ---------------------------------------------------------------------------------------------------------------
// RadioFoundry: its debug link names the board it runs, which is the image of one variant

registerProbe({
	id: "rfoundry",
	baud: null,
	async run(stream, { radio, fb }) {
		const p = await linkProbe(new Link(stream, { timeout: 700 }));
		if (!p) return null;
		const boardId = Object.entries(fb?.boards || {}).find(([, b]) => b.debuglink_names?.includes(p.board))?.[0] || p.board;
		return { code: variantOfBoard(radio, boardId), version: `RadioFoundry ${boardId} (${p.git || "?"})`, via: "rfoundry",
			board: boardId, keep: false, stop: true };
	},
});

// ---------------------------------------------------------------------------------------------------------------
// The check before a write: the variant asked once more on the session the write goes through, by the probe that
// opened it (its confirm(session, ctx) sends only a read-only question). Throws DetectMismatch when the radio is no
// longer the one judged at connect (another radio plugged in, a changed pick) or the image isn't the one its variant
// runs; returns the answer otherwise. A probe without confirm can't check: the write is refused, not let through

export class DetectMismatch extends Error {}

export async function confirmVariant({ id, session, radio, board, fb, expect, imageBoard, log = () => {} }) {
	const p = probeOf(id);
	if (!p?.confirm) throw new DetectMismatch(`the radio can't be checked again before the write (${id}): connect again`);
	const r = await p.confirm(session, { radio, board, fb, log });
	if (!r?.code) throw new DetectMismatch(`the radio answered ${JSON.stringify(r?.version ?? "")}, which names no variant: nothing written`);
	if (expect && r.code !== expect)
		throw new DetectMismatch(`the radio now answers ${r.version} (${r.code}), not the ${expect} judged at connect: nothing written`);
	const variant = (radio?.variants || []).find((v) => v.code === r.code);
	const runs = variant?.runs?.board || null;
	if (imageBoard && variant?.blocked) throw new DetectMismatch(`the radio is ${r.code}, and ${variant.blocked}: nothing written`);
	if (imageBoard && runs !== imageBoard)
		throw new DetectMismatch(`the radio is ${r.code}, which ${runs ? `takes the ${runs} image` : "has no RadioFoundry image"}, not ${imageBoard}: nothing written`);
	return r;
}

// ---------------------------------------------------------------------------------------------------------------
// The USB ids the port enumerated with (board.toml variants usb_ids): the original firmware's USB device differs by
// variant on some radios (another MCU, another USB stack). Nothing is sent, so it goes first

export const usbId = (info) => (info?.usbVendorId === undefined || info?.usbProductId === undefined ? null
	: `${info.usbVendorId.toString(16).padStart(4, "0")}:${info.usbProductId.toString(16).padStart(4, "0")}`.toUpperCase());

export function variantOfUsb(radio, id) {
	return id ? (radio?.variants || []).find((v) => (v.usb_ids || []).includes(id))?.code || null : null;
}

registerProbe({
	id: "usb-id",
	baud: null,
	async run(stream, { radio }) {
		const id = usbId(stream?.port?.getInfo?.());
		const code = variantOfUsb(radio, id);
		return code ? { code, version: `USB ${id}`, via: "usb", keep: false } : null;
	},
});
