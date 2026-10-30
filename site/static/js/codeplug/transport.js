// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// How the codeplug editor reaches a radio. A transport reads the codeplug's regions into an image and writes back
// the bytes that changed; the page doesn't care how. Today there is one, "cps-serial", over the radio's USB serial port
// or the programming cable. It speaks the fast codeplug protocol (docs/spec/cps-fast.md, fastlink.js: 4 KB blocks
// compared by CRC-32, LZ4 on the wire) when the radio lists CPS_FAST, and the radio's open-firmware CPS
// (docs/spec/cps-protocol.md 2) otherwise or for a block the fast protocol refuses.
//
// Addresses here are image addresses (image.js); a transport adds the radio's base (its flash map's openfw_eeprom:
// 0x700000 on the DM-32UV) on the wire. Writes send only changed bytes inside the codeplug's regions, never whole
// sectors: a sector the codeplug shares with the radio's own settings (0x6000) keeps them.

import { CodeplugImage, REGIONS, IDDB_HEADER, CODEPLUG_SPAN } from "./image.js";
import { readHeader, extent, AREA1, AREA1_SIZE, AREA2 } from "./iddb.js";
import { GENERAL, BOOT_TEXT } from "./records.js";
import { Link, probe, whyNoAnswer } from "../flasher/link.js";
import { crc32, sleep } from "../flasher/bytes.js";
import { encodeBlock } from "../flasher/lz4.js";
import { FastLink, FastError, BLOCK, REASON } from "./fastlink.js";
import { note } from "../diag.js";

export class TransportError extends Error {
	constructor(code, params = {}) {
		super(code);
		this.name = "TransportError";
		this.code = code;
		this.params = params;
	}
}

// The radio's settings block starts inside the first region (open-firmware-cps.md 7.1): never written
const KEEP_OUT = [[0x604B, 0x6060]];
// The VFO pair: a read shows the radio's live VFOs there, not the bytes written (open-firmware-cps.md 2.2, "VFO
// overlay"), so a verify doesn't compare it
export const VFOS = [0x7590, 0x7600];

// What makes one radio itself (open-firmware-cps.md 7.3.2, 7.2): its name and DMR ID, and its boot text. A write that
// keeps them programs one codeplug into many radios
export const IDENTITY = [[GENERAL, GENERAL + 12], [BOOT_TEXT, BOOT_TEXT + 32]];

// want with the radio's own identity bytes (have: its bytes as read) in place of the file's
export function keepIdentity(want, have) {
	const out = want.clone();
	for (const [a, b] of IDENTITY) {
		out.ensure(b);
		out.bytes.set(have.bytes.subarray(a, b), a);
	}
	return out;
}

// Regions of the codeplug a write may change
export function writeRanges() {
	const out = [];
	for (const [a, b] of REGIONS) {
		let s = a;
		for (const [ka, kb] of KEEP_OUT) {
			if (ka >= s && ka < b) {
				if (ka > s) out.push([s, ka]);
				s = Math.max(s, kb);
			}
		}
		if (s < b) out.push([s, b]);
	}
	return out;
}

// The ID database's bytes in an image: the header and records in area 1, the rest in area 2
export function iddbRanges(image) {
	const top = idDbEnd(image.bytes);
	const out = [[AREA1, Math.min(AREA1 + AREA1_SIZE, top)]];
	if (top > AREA2) out.push([AREA2, top]);
	return out;
}

export function idDbEnd(b) {
	return extent(b) || IDDB_HEADER[1];
}

// The 4 KB blocks ranges touch, each with the parts of the ranges inside it: [[addr, [[a, b]]]]
export function blocksOf(ranges) {
	const out = new Map();
	for (const [a, b] of ranges) {
		for (let s = Math.floor(a / BLOCK) * BLOCK; s < b; s += BLOCK) {
			if (!out.has(s)) out.set(s, []);
			out.get(s).push([Math.max(a, s), Math.min(b, s + BLOCK)]);
		}
	}
	return [...out.entries()].sort((x, y) => x[0] - y[0]);
}

const covers = (addr, parts) => parts.reduce((n, [a, b]) => n + b - a, 0) === BLOCK;

// Changed bytes of want against have within ranges, as runs grouped by 4 KB sector: [[sector, [[addr, bytes]]]].
// Runs closer than gap bytes are merged (one request costs more than a few unchanged bytes)
export function changes(want, have, ranges, gap = 16) {
	const sectors = new Map();
	for (const [a, b] of ranges) {
		let i = a;
		while (i < b) {
			if (want[i] === have?.[i] && have) {
				i++;
				continue;
			}
			let j = i + 1, same = 0;
			const sectorEnd = (Math.floor(i / 4096) + 1) * 4096;
			while (j < b && j < sectorEnd && same < gap) {
				same = have && want[j] === have[j] ? same + 1 : 0;
				j++;
			}
			const end = j - same;
			const s = Math.floor(i / 4096) * 4096;
			if (!sectors.has(s)) sectors.set(s, []);
			sectors.get(s).push([i, want.subarray(i, end)]);
			i = end;
		}
	}
	return [...sectors.entries()].sort((x, y) => x[0] - y[0]);
}

// --- Over the radio's codeplug interface ---

export class CpsSerial {
	// stream: a SerialStream (or a simulated one); cable: a programming cable at 115200 baud, where a 32 KB read
	// takes about 3 s
	constructor(stream, { cable = false } = {}) {
		this.id = "cps-serial";
		this.stream = stream;
		this.cable = cable;
		this.link = new Link(stream, { timeout: cable ? 8000 : 2000 });
		this.chunk = cable ? 4096 : 32768;
		this.radio = null;
		this.base = 0;
		this.fast = null;	// the fast protocol's INFO, when the radio speaks it
		this.compact = false;	// the radio reads a format C ID database (iddb-compact.md)
	}

	// Which radio answers, matched by the board name it reports to radios.json's codeplug.names. No answer is told
	// apart by what the port gave (link.js whyNoAnswer()): another open firmware that gives its radio info, or bytes
	// that aren't the debug link (another firmware)
	async identify(radios) {
		const p = await probe(this.link);
		if (!p) {
			const why = { ...(await whyNoAnswer(this.link)), cable: this.cable };
			note("no answer", why);
			throw new TransportError(why.kind === "openfw" ? "openfw" : why.kind === "other" ? "not_rfoundry"
				: this.cable ? "no_answer_cable" : "no_answer", why);
		}
		const radio = radios.find((r) => r.codeplug?.names?.some((n) => n.toLowerCase() === String(p.board || "").toLowerCase()));
		if (!radio) throw new TransportError("unknown_radio", { board: p.board || "?" });
		this.radio = radio;
		this.base = radio.codeplug.base;
		this.probe = p;
		const features = String(p.features || "").split(",");
		this.compact = features.includes("IDDB_COMPACT");
		this.fast = null;
		if (features.includes("CPS_FAST") && !this.noFast) {
			const fl = new FastLink(this.link);
			const info = await fl.info().catch(() => null);
			if (info?.writable !== undefined && info.block === BLOCK) {
				this.fl = fl;
				this.fast = info;
			}
		}
		note("transport", { fast: !!this.fast, compact: this.compact, staging: this.fast?.stageMax || 0 });
		return { radio, probe: p };
	}

	// Which radio is on the line now: the board again (identify) and the chip UID, null when the radio doesn't say.
	// A programming cable stays open while the radio on it is swapped, so a write can't trust what an earlier read saw
	async who(radios) {
		await this.identify(radios);
		try {
			return (await this.link.chip())?.uid ?? null;
		} catch {
			return null;
		}
	}

	get end() {
		return this.radio ? this.radio.codeplug.end - this.radio.codeplug.base : CODEPLUG_SPAN;
	}

	// The codeplug's regions read into a fresh image (the rest erased); with iddb, the whole ID database too. cache: an
	// image read before; with the fast protocol its blocks whose CRC still matches aren't read again
	async read({ iddb = true, progress = () => {}, signal, cache = null } = {}) {
		return this.fastOr(() => this.fastRead({ iddb, progress, signal, cache }), () => this.cpsRead({ iddb, progress, signal }));
	}

	async cpsRead({ iddb, progress, signal }) {
		const img = CodeplugImage.from(new Uint8Array(0));
		const ranges = [...REGIONS, IDDB_HEADER];
		let total = ranges.reduce((n, [a, b]) => n + b - a, 0), done = 0;
		const get = async (a, b) => {
			const r = await this.link.cpsDump(1, this.base + a, b - a, {
				chunk: this.chunk, signal, progress: (n) => progress(done + n, total),
			});
			if (!r.complete) throw new TransportError("read_refused", { addr: a });
			img.ensure(b);
			img.bytes.set(r.data, a);
			done += b - a;
		};
		for (const [a, b] of ranges) await get(a, b);
		if (iddb && readHeader(img.bytes)) {
			const top = Math.min(idDbEnd(img.bytes), this.end);
			const parts = [[IDDB_HEADER[1], Math.min(top, AREA1 + AREA1_SIZE)]];
			if (top > AREA2) parts.push([AREA2, top]);
			total += parts.reduce((n, [a, b]) => n + Math.max(0, b - a), 0);
			for (const [a, b] of parts) if (b > a) await get(a, b);
		}
		return img;
	}

	// What a write sends. Fast: the blocks whose CRC on the radio differs from want's (have: the radio's bytes as last
	// read, trusted for a block only where its CRC matches). CPS: the codeplug's bytes that differ from have (null:
	// everything), and with iddb the whole ID database (never read back first: it can be megabytes)
	async plan(want, have, { iddb = false } = {}) {
		return this.fastOr(() => this.fastPlan(want, have, { iddb }), () => this.cpsPlan(want, have, { iddb }));
	}

	// fast() with the fast protocol, else cps(); cps() too when the radio refuses a block as out of range. SUMS, READ and
	// BLOCK share that check, so a read or a plan meets it before anything is written
	async fastOr(fast, cps) {
		if (this.fast) {
			try {
				return await fast();
			} catch (e) {
				if (!(e instanceof FastError && e.reason === REASON.RANGE)) throw e;
				note("fast protocol refused a block: the CPS takes over", { error: e.message });
			}
		}
		return cps();
	}

	cpsPlan(want, have, { iddb = false } = {}) {
		const plan = changes(want.bytes, have?.bytes ?? null, writeRanges());
		if (iddb) plan.push(...changes(want.bytes, null, iddbRanges(want)));
		plan.sort((x, y) => x[0] - y[0]);
		const bytes = plan.reduce((n, [, runs]) => n + runs.reduce((m, [, d]) => m + d.length, 0), 0);
		return { plan, bytes };
	}

	// The radio's settings region (debuglink.md 3.13): {region: "ours" | "foreign" | "none", ...}, or null when the
	// radio doesn't say. A foreign region (another firmware's data still there) makes the radio refuse every write
	async storage() {
		try {
			return await this.link.storageInfo();
		} catch {
			return null;
		}
	}

	// Erase a foreign region and take it; true once the radio reports it ours
	async claimStorage() {
		return this.link.storageClaim();
	}

	// One sector, tried twice: a sector left pending by an interrupted session is refused once and then dropped, and a
	// reply lost on the way is worth one more try (the sector is rewritten whole from the radio's copy, so a repeat is
	// harmless)
	async writeSector(sector, runs) {
		const parts = runs.map(([a, d]) => [this.base + a, d]);
		if (await this.link.cpsWriteParts(this.base + sector, parts)) return;
		const first = this.link.writeFailure;
		note("sector write failed, trying again", { sector, ...first });
		await sleep(first?.result === "no answer" ? 1500 : 200);
		this.stream.clear?.();
		if (await this.link.cpsWriteParts(this.base + sector, parts)) return;
		const fail = this.link.writeFailure || first || {};
		if (fail.result === "no answer") throw new TransportError("write_no_answer", { addr: sector, step: fail.what });
		const st = await this.storage();
		if (st?.region === "foreign") throw new TransportError("storage_foreign", { addr: sector });
		throw new TransportError("write_refused", { addr: sector, step: fail.what });
	}

	// Write the plan; throws when the radio refuses a sector
	async write(want, have, { iddb = false, progress = () => {}, signal } = {}) {
		if (iddb && idDbEnd(want.bytes) > this.end) throw new TransportError("iddb_too_big");
		return this.fastOr(() => this.fastWrite(want, have, { iddb, progress, signal }), () => this.cpsWrite(want, have, { iddb, progress, signal }));
	}

	async cpsWrite(want, have, { iddb, progress, signal }) {
		const { plan, bytes: total } = this.cpsPlan(want, have, { iddb });
		// the ID database's header erased before the rest of it and its sector written last (iddb-compact.md 4.3), so a
		// write cut short leaves no database rather than a new header over old records
		let order = plan;
		const head = iddb ? plan.find(([s]) => s === AREA1) : null;
		if (head) {
			await this.writeSector(AREA1, [[AREA1, new Uint8Array(IDDB_HEADER[1] - AREA1).fill(0xFF)]]);
			order = [...plan.filter((p) => p !== head), head];
		}
		let done = 0;
		for (const [sector, runs] of order) {
			if (signal?.aborted) throw new TransportError("stopped");
			await this.writeSector(sector, runs);
			done += runs.reduce((m, [, d]) => m + d.length, 0);
			progress(done, total);
		}
		return { bytes: total, sectors: plan.length, plan };
	}

	// Read back what a write changed: the fast protocol's blocks by their CRC, the CPS's runs byte by byte
	async verify(want, plan, { progress = () => {} } = {}) {
		if (plan.fast) return this.fastVerify(plan, { progress });
		let done = 0;
		for (const [, runs] of plan) {
			for (const [a, d] of runs) {
				const r = await this.link.cpsDump(1, this.base + a, d.length, { chunk: this.chunk });
				const bad = !r.complete ? 0 : r.data.findIndex((x, i) => x !== d[i] && !(a + i >= VFOS[0] && a + i < VFOS[1]));
				if (bad >= 0) throw new TransportError("verify_failed", { addr: a + bad });
			}
			progress(++done, plan.length);
		}
	}

	// --- The fast protocol (cps-fast.md) ---

	// The blocks of ranges into img: blank ones filled, ones whose CRC matches cache copied from it, the rest read
	async fastBlocks(img, ranges, { cache, progress, signal, done = 0, total = 0 }) {
		const blocks = blocksOf(ranges);
		const sums = await this.fastSums(blocks.map(([a]) => a));
		for (const [a] of blocks) {
			if (signal?.aborted) throw new TransportError("stopped");
			const s = sums.get(a);
			img.ensure(a + BLOCK);
			const c = cache?.bytes.length >= a + BLOCK ? cache.bytes.subarray(a, a + BLOCK) : null;
			if (s.blank) img.bytes.fill(0xFF, a, a + BLOCK);
			else if (c && crc32(c) === s.crc) img.bytes.set(c, a);
			else img.bytes.set(await this.fl.read(a), a);
			progress(done += BLOCK, total);
		}
		return done;
	}

	// SUMS of the blocks at addrs: a Map addr -> {crc, blank}, in runs of consecutive blocks
	async fastSums(addrs) {
		const out = new Map();
		for (let i = 0; i < addrs.length;) {
			let n = 1;
			while (i + n < addrs.length && addrs[i + n] === addrs[i] + n * BLOCK) n++;
			for (const s of await this.fl.sums(addrs[i], n)) out.set(s.addr, s);
			i += n;
		}
		return out;
	}

	// Whole blocks, so the image also holds the radio's bytes around the ranges (the records after the ID database
	// header): a later read or write matches a block's CRC against it. Nothing writes them back
	async fastRead({ iddb, progress, signal, cache }) {
		const img = CodeplugImage.from(new Uint8Array(0));
		const ranges = [...REGIONS, IDDB_HEADER];
		let total = blocksOf(ranges).length * BLOCK;
		let done = await this.fastBlocks(img, ranges, { cache, progress, signal, total });
		if (iddb && readHeader(img.bytes)) {
			const rest = iddbRanges(img).map(([a, b]) => [a, Math.min(b, this.end)]).filter(([a, b]) => b > a);
			total += blocksOf(rest).length * BLOCK;
			await this.fastBlocks(img, rest, { cache, progress, signal, done, total });
		}
		return img;
	}

	// [[addr, bytes]] for every block of ranges whose bytes on the radio differ from want's within the ranges: the
	// radio's own bytes stay outside them (KEEP_OUT, a sector's other contents). A block the ranges fill isn't read
	async fastChanged(want, have, ranges, sums) {
		const out = [];
		for (const [a, parts] of blocksOf(ranges)) {
			const s = sums.get(a);
			let next;
			if (covers(a, parts)) next = new Uint8Array(BLOCK).fill(0xFF);
			else {
				const h = have?.bytes.length >= a + BLOCK ? have.bytes.subarray(a, a + BLOCK) : null;
				next = s.blank ? new Uint8Array(BLOCK).fill(0xFF) : h && crc32(h) === s.crc ? h.slice() : await this.fl.read(a);
			}
			for (const [x, y] of parts) next.set(want.bytes.subarray(x, y), x - a);
			if (crc32(next) !== s.crc) out.push([a, next]);
		}
		return out;
	}

	async fastPlan(want, have, { iddb = false } = {}) {
		const cp = writeRanges(), db = iddb ? iddbRanges(want) : [];
		want.ensure(Math.max(...[...cp, ...db].map(([, b]) => b)));
		const sums = await this.fastSums(blocksOf([...cp, ...db]).map(([a]) => a));
		const codeplug = await this.fastChanged(want, have, cp, sums);
		const ids = iddb ? await this.fastChanged(want, have, db, sums) : [];
		// the database's header block: rewritten first erased and again last whenever any of it changes (iddb-compact.md
		// 4.3), so a write cut short leaves no database rather than a mix
		if (ids.length && !ids.some(([a]) => a === AREA1)) ids.unshift([AREA1, want.bytes.slice(AREA1, AREA1 + BLOCK)]);
		const plan = [...codeplug, ...ids];
		plan.fast = true;
		plan.codeplug = codeplug.length;
		const bytes = plan.reduce((n, [, b]) => n + (b.every((x) => x === 0xFF) ? 0 : encodedSize(b)), 0);
		return { plan, bytes, blocks: plan.length };
	}

	async fastWrite(want, have, { iddb, progress, signal }) {
		const { plan, bytes } = await this.fastPlan(want, have, { iddb });
		const codeplug = plan.slice(0, plan.codeplug), ids = plan.slice(plan.codeplug);
		let done = 0;
		const total = plan.length + (ids.length ? 1 : 0);	// the header block goes twice
		const step = () => progress(++done, total);
		const refused = [];
		if (codeplug.length) {
			const staged = this.fast.staging && codeplug.length <= this.fast.stageMax;
			await this.fastSession(staged, codeplug, { signal, step, refused });
		}
		if (ids.length) {
			// header block first with the header erased, the rest, then the header block as it should be
			const head = ids.find(([a]) => a === AREA1), first = head[1].slice();
			first.fill(0xFF, 0, 32);
			const order = [[AREA1, first], ...ids.filter(([a]) => a !== AREA1), head];
			await this.fastSession(false, order, { signal, step, refused: null });
		}
		// a block the fast protocol refuses (outside what it serves on this radio) goes through the CPS
		for (const a of refused) {
			const runs = changes(want.bytes, null, writeRanges().filter(([x, y]) => x < a + BLOCK && y > a)
				.map(([x, y]) => [Math.max(x, a), Math.min(y, a + BLOCK)]));
			for (const [sector, r] of runs) await this.writeSector(sector, r);
		}
		return { bytes, sectors: plan.length, plan };
	}

	async fastSession(staged, blocks, { signal, step, refused }) {
		await this.fl.begin(staged);
		try {
			for (const [a, b] of blocks) {
				if (signal?.aborted) throw new TransportError("stopped");
				try {
					await this.fl.write(a, b);
				} catch (e) {
					// the ID database has no way round: its blocks are all written or the write fails
					if (!refused || !(e instanceof FastError && e.reason === REASON.RANGE)) throw e;
					note("fast write refused a block: the CPS writes it", { addr: a });
					refused.push(a);
				}
				step();
			}
			await this.fl.commit();
		} catch (e) {
			await this.fl.abort();
			if (e instanceof FastError) {
				if (e.status === "no answer") throw new TransportError("write_no_answer", { addr: 0, step: e.what });
				if (e.reason === REASON.STORAGE) throw new TransportError("storage_foreign", { addr: 0 });
				throw new TransportError("write_refused", { addr: 0, step: `${e.what} (${e.message})` });
			}
			throw e;
		}
		if (!staged) return;
		// the radio copies one staged block per pass of its host link task
		const until = Date.now() + 5000 + blocks.length * 2000;
		for (;;) {
			const st = await this.fl.status();
			if (st.state === 4) return;
			if (st.state === 5) throw new TransportError("write_refused", { addr: 0, step: `commit (reason ${st.reason})` });
			if (Date.now() > until) throw new TransportError("write_no_answer", { addr: 0, step: "commit" });
			await sleep(50);
		}
	}

	// Each written block's CRC on the radio against the bytes written. The VFO pair shows the live VFOs, so a block
	// holding it that differs is read and compared without them
	async fastVerify(plan, { progress }) {
		const sums = await this.fastSums(plan.map(([a]) => a).sort((x, y) => x - y));
		let done = 0;
		for (const [a, b] of plan) {
			if (sums.get(a).crc !== crc32(b)) {
				const live = a < VFOS[1] && a + BLOCK > VFOS[0] ? await this.fl.read(a) : null;
				const bad = live ? live.findIndex((x, i) => x !== b[i] && !(a + i >= VFOS[0] && a + i < VFOS[1])) : 0;
				if (bad >= 0) throw new TransportError("verify_failed", { addr: a + bad });
			}
			progress(++done, plan.length);
		}
	}

	// End the session: the radio reloads the codeplug (C 5) and shows the new channels without a restart
	async finish() {
		await this.link.cpsClose();
	}

	async close() {
		try {
			await this.stream.close?.();
		} catch {
			// already gone
		}
	}
}

// LZ4 stream bytes for a block (what the fast protocol sends)
function encodedSize(b) {
	return encodeBlock(b).length;
}

// Estimated seconds for a transfer of n bytes; blocks: 4 KB blocks the fast protocol writes (each an erase and two
// programs on the radio, about 0.15 s)
export function estimate(n, cable, blocks = 0) {
	return Math.ceil(n / (cable ? 9000 : 150000) + blocks * 0.15) + 1;
}
