// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// How the codeplug editor reaches a radio. A transport reads the codeplug's regions into an image and writes back
// the bytes that changed; the page doesn't care how. Today there is one, "cps-serial": the radio's own codeplug
// interface (the open-firmware CPS, docs/spec/cps-protocol.md 2) over its USB serial port or the programming cable.
// A faster route (the loader's CRC-compared sector writes) can be registered beside it later.
//
// Addresses here are image addresses (image.js); a transport adds the radio's base (its flash map's openfw_eeprom:
// 0x700000 on the DM-32UV) on the wire. Writes send only changed bytes inside the codeplug's regions, never whole
// sectors: a sector the codeplug shares with the radio's own settings (0x6000) keeps them.

import { CodeplugImage, REGIONS, IDDB_HEADER, CODEPLUG_SPAN } from "./image.js";
import { readHeader, AREA1, AREA1_SIZE, AREA2 } from "./iddb.js";
import { Link, probe } from "../flasher/link.js";
import { sleep } from "../flasher/bytes.js";
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
const VFOS = [0x7590, 0x7600];

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
	const h = readHeader(b);
	if (!h) return AREA1 + 12;
	const per1 = Math.floor((AREA1_SIZE - 12) / h.rec);
	return h.count <= per1 ? AREA1 + 12 + h.count * h.rec : AREA2 + (h.count - per1) * h.rec;
}

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
	}

	// Which radio answers, matched by the board name it reports to radios.json's codeplug.names
	async identify(radios) {
		const p = await probe(this.link);
		if (!p) throw new TransportError("no_answer");
		const radio = radios.find((r) => r.codeplug?.names?.some((n) => n.toLowerCase() === String(p.board || "").toLowerCase()));
		if (!radio) throw new TransportError("unknown_radio", { board: p.board || "?" });
		this.radio = radio;
		this.base = radio.codeplug.base;
		this.probe = p;
		return { radio, probe: p };
	}

	get end() {
		return this.radio ? this.radio.codeplug.end - this.radio.codeplug.base : CODEPLUG_SPAN;
	}

	// The codeplug's regions read into a fresh image (the rest erased); with iddb, the whole ID database too
	async read({ iddb = true, progress = () => {}, signal } = {}) {
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
			const parts = [[AREA1 + 12, Math.min(top, AREA1 + AREA1_SIZE)]];
			if (top > AREA2) parts.push([AREA2, top]);
			total += parts.reduce((n, [a, b]) => n + Math.max(0, b - a), 0);
			for (const [a, b] of parts) if (b > a) await get(a, b);
		}
		return img;
	}

	// What a write sends: the codeplug's bytes that differ from have (the radio's bytes as last read; null:
	// everything), and with iddb the whole ID database (never read back first: it can be megabytes)
	plan(want, have, { iddb = false } = {}) {
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
		const { plan, bytes: total } = this.plan(want, have, { iddb });
		let done = 0;
		for (const [sector, runs] of plan) {
			if (signal?.aborted) throw new TransportError("stopped");
			await this.writeSector(sector, runs);
			done += runs.reduce((m, [, d]) => m + d.length, 0);
			progress(done, total);
		}
		return { bytes: total, sectors: plan.length, plan };
	}

	// Read back what a write changed
	async verify(want, plan, { progress = () => {} } = {}) {
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

// Estimated seconds for a transfer of n bytes
export function estimate(n, cable) {
	return Math.ceil(n / (cable ? 9000 : 150000)) + 1;
}
