// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Variant detection (detect.js, anytonedetect.js) against simulated radios: the original firmware's USB ids, the
// AnyTone update mode, RadioFoundry's debug link, a radio that answers nothing, and the check before a write. Run with
// site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const { detectVariant, confirmVariant, DetectMismatch, usbId, variantOfUsb, variantOfBoard, probeOf, portFilters } = await import("../static/js/flasher/detect.js");
const { PROBES } = await import("../static/js/flasher/anytonedetect.js");
const { FakeAnytone } = await import("../static/js/flasher/fakeanytone.js");
const { frame, parseFrames, CMD } = await import("../static/js/flasher/link.js");
const { concat } = await import("../static/js/flasher/bytes.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const FB = json("flash-boards.json");
const RADIOS = json("radios.json").radios;
// the AnyTone radio with two variants that take two images, and the USB ids of each
const RADIO = RADIOS.find((r) => r.flash.backend === "anytone-update" && r.variants?.some((v) => v.usb_ids?.length) &&
	new Set(r.variants.map((v) => v.runs?.board)).size > 1);
const BOARD = FB.boards[RADIO.flash.board];
const [A, B] = RADIO.variants;
const modelOf = (code) => BOARD.anytone_models[code][0];
const textOf = (b) => new TextDecoder().decode(b);

// A stream that answers nothing; info: what its port says it is
function silent(info = {}) {
	const s = { buf: new Uint8Array(0), sent: [], closed: false, port: { getInfo: () => info },
		clear() { s.buf = new Uint8Array(0); }, take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
		async write(b) { s.sent.push(b); }, arrived: (ms) => new Promise((ok) => setTimeout(ok, Math.min(ms, 20))), async close() { s.closed = true; } };
	return s;
}

// RadioFoundry's debug link: VERSION answered with the board it runs, everything else refused
function rfoundry(board) {
	const s = silent({ usbVendorId: 0x1FC9, usbProductId: 0x0094 });
	let inbuf = new Uint8Array(0);
	s.write = async (b) => {
		s.sent.push(b);
		inbuf = concat(inbuf, b);
		for (const f of parseFrames(inbuf)) {
			inbuf = inbuf.slice(f.end);
			const body = f.cmd === CMD.VERSION ? concat(Uint8Array.of(0), new TextEncoder().encode(`board=${board}\ngit=0123abcd\n`)) : Uint8Array.of(1);
			s.buf = concat(s.buf, frame(f.cmd | 0x80, f.seq, body));
		}
	};
	return s;
}

const run = (stream, ids = PROBES) => detectVariant({ ids, radio: RADIO, board: BOARD, fb: FB, open: async () => stream });

test("detect: the original firmware's USB ids name the variant, nothing sent", async () => {
	const [vid, pid] = B.usb_ids[0].split(":").map((x) => parseInt(x, 16));
	assert.equal(usbId({ usbVendorId: vid, usbProductId: pid }), B.usb_ids[0]);
	assert.equal(variantOfUsb(RADIO, B.usb_ids[0]), B.code);
	assert.equal(variantOfUsb(RADIO, "0000:0000"), null);
	const s = silent({ usbVendorId: vid, usbProductId: pid });
	const { found, tried } = await run(s);
	assert.equal(found.code, B.code);
	assert.equal(found.via, "usb");
	assert.equal(s.sent.length, 0, "the USB probe sends nothing");
	assert.deepEqual(tried.map((x) => x.outcome), ["found"]);
	assert.ok(s.closed, "a stream not kept is closed");
});

test("detect: the update mode's identify names the variant and the session is kept for the install", async () => {
	for (const v of [A, B]) {
		const fake = new FakeAnytone({ model: modelOf(v.code) });
		fake.stream.port = { getInfo: () => ({}) };
		const { found, tried } = await run(fake.stream);
		assert.equal(found.code, v.code);
		assert.equal(found.via, "update-mode");
		assert.equal(found.stream, fake.stream, "kept");
		assert.equal(found.ident.model, modelOf(v.code));
		assert.deepEqual(tried.map((x) => x.id), ["usb-id", "anytone-update"]);
		// the check before the write, on that session: the same radio, its own image
		const own = confirmVariant({ id: found.id, session: found.radio, radio: RADIO, board: BOARD, fb: FB, expect: v.code, imageBoard: v.runs.board });
		if (v.blocked) await assert.rejects(own, (e) => e instanceof DetectMismatch && e.message.includes(v.blocked));
		else assert.equal((await own).code, v.code);
		// the other variant's image is refused before a frame
		const other = v === A ? B : A;
		await assert.rejects(confirmVariant({ id: found.id, session: found.radio, radio: RADIO, board: BOARD, fb: FB, expect: v.code, imageBoard: other.runs.board }), DetectMismatch);
		assert.equal(fake.frames, 0, "nothing written");
	}
});

test("detect: a radio swapped after connect is caught by the check before the write", async () => {
	const fake = new FakeAnytone({ model: modelOf(A.code) });
	const { found } = await run(fake.stream, ["anytone-update"]);
	fake.model = modelOf(B.code);
	await assert.rejects(confirmVariant({ id: found.id, session: found.radio, radio: RADIO, board: BOARD, fb: FB, expect: A.code, imageBoard: A.runs.board }),
		(e) => e instanceof DetectMismatch && /not the/.test(e.message));
	await assert.rejects(confirmVariant({ id: "rfoundry", session: null, radio: RADIO, board: BOARD, fb: FB, expect: A.code }), DetectMismatch,
		"a probe that can't ask again refuses the write");
});

test("detect: the running original firmware ignores UPDATE; RadioFoundry names the image it runs", async () => {
	for (const v of [A, B]) {
		const s = rfoundry(v.runs.board);
		const { found, tried } = await run(s);
		assert.equal(found.code, variantOfBoard(RADIO, v.runs.board));
		assert.equal(found.via, "rfoundry");
		assert.equal(found.keep, false);
		assert.deepEqual(tried.map((x) => `${x.id}:${x.outcome}`), ["usb-id:silent", "anytone-update:silent", "rfoundry:found"]);
		assert.equal(textOf(s.sent[0]), "UPDATE", "the update mode's word goes before any debug link frame");
	}
});

test("detect: a radio that answers nothing: every probe tried, none found", async () => {
	const s = silent();
	const { found, tried } = await run(s);
	assert.equal(found, null);
	assert.deepEqual(tried.map((x) => x.outcome), ["silent", "silent", "silent"]);
	assert.ok(PROBES.every((id) => probeOf(id)), "every probe the backend lists is registered");
	const none = await detectVariant({ ids: ["no-such"], radio: RADIO, board: BOARD, fb: FB, open: async () => s });
	assert.equal(none.tried[0].outcome, "error");
});

test("detect: the port chooser lists the variants' USB devices and RadioFoundry's, nothing else", () => {
	const ids = portFilters(RADIO).map((f) => usbId(f));
	for (const v of RADIO.variants) for (const id of v.usb_ids || []) assert.ok(ids.includes(id), id);
	assert.ok(ids.includes("1FC9:0094"));
	assert.equal(ids.length, new Set(ids).size);
	assert.deepEqual(portFilters({}).map((f) => usbId(f)), ["1FC9:0094"]);
});

test("detect: every probe and via word has its strings", () => {
	const strings = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));
	for (const id of PROBES) assert.ok(strings[`flash.detect.probe.${id.replace(/-/g, "_")}`], id);
	for (const via of ["usb", "update_mode", "rfoundry"]) assert.ok(strings[`flash.detect.via.${via}`], via);
	for (const o of ["found", "answered", "silent", "error"]) assert.ok(strings[`flash.detect.outcome.${o}`], o);
});
