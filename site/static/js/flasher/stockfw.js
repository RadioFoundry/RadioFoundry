// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// TYT / Baofeng stock firmware files in the browser: container, decryption, codec extraction, key derivation.
// A port of tools/odrflash/odrflash/stockfw.py; the profiles come from that file through data/flash-boards.json, so
// both read the same table. The codec and the key are only ever handled on the user's machine.

import { u32le, sha256, startsWith, crc32 } from "./bytes.js";
import { byCodeSha, moveCode, moveBranches, branchesIn, MapError } from "./codecmap.js";
import { unzip } from "./zip.js";

export class StockError extends Error {}

export function parseContainer(data, fb) {
	if (data.length < fb.header_len || !startsWith(data, fb.magic))
		throw new StockError("not a TYT/Baofeng firmware file (no \"OutSecurityBin\" header)");
	let count = u32le(data, 0x7C);
	if (count === fb.single_segment) count = 1;
	if (count < 1 || count > 15) throw new StockError(`implausible segment count ${count}`);
	const segs = [];
	for (let i = 0; i < count; i++) segs.push([u32le(data, 0x80 + 8 * i), u32le(data, 0x84 + 8 * i)]);
	const total = segs.reduce((a, s) => a + s[1], 0);
	if (fb.header_len + total > data.length) throw new StockError("segment lengths run past the end of the file");
	return segs;
}

export function appFileOffset(segs, addr, fb) {
	let off = fb.header_len;
	for (const [base, length] of segs) {
		if (base <= addr && addr < base + length) return off + addr - base;
		off += length;
	}
	throw new StockError(`address 0x${addr.toString(16)} is in no segment`);
}

// plain[i] = file[off + i] XOR key[(off + i - header) mod key_len]
export function decrypt(data, off, length, key, fb) {
	if (off < fb.header_len || off + length > data.length) throw new StockError("window outside the payload");
	const out = new Uint8Array(length);
	for (let i = 0; i < length; i++) out[i] = data[off + i] ^ key[(off + i - fb.header_len) % fb.key_len];
	return out;
}

// A stock file as the user picked it: the updater .bin itself, or the vendor's zip with it inside (TYT ships the .bin
// next to Upgrade.exe and its DLLs). From a zip: the one entry with the container header, macOS "._" copies skipped.
// Returns {name, data}.
export async function stockFile(name, data, fb) {
	if (!(data.length > 4 && data[0] === 0x50 && data[1] === 0x4B && data[2] === 0x03 && data[3] === 0x04)) return { name, data };
	const found = [];
	for (const [path, body] of await unzip(data)) {
		const base = path.split("/").pop();
		if (path.startsWith("__MACOSX/") || base.startsWith("._") || !startsWith(body, fb.magic)) continue;
		found.push({ name: base, data: body });
	}
	if (found.length !== 1) throw new StockError(found.length ? `${name} holds ${found.length} firmware files: pick the .bin itself` : `no firmware file (.bin with an "OutSecurityBin" header) in ${name}`);
	return found[0];
}

// What a file is: {sha, known: "DM-1701 V02.06 CSV" | null, profile | null}
export async function identify(data, fb) {
	const sha = await sha256(data);
	return { sha, known: fb.known_files[sha] || null, profile: fb.profiles[sha] || null };
}

// Where the merge puts a profile's codec (stockfw.placed_windows): [[addr, length, sha256]], code first, then the
// table windows (the extra ones at their stock address, unchanged)
export function placedWindows(prof) {
	if (!prof.placement) return prof.windows;
	const pl = prof.placement, pd = prof.placed, tab = prof.windows[1];
	return [[pl.code, prof.windows[0][1], pd.code[1]], [tab[0], pl.tables_len ?? tab[1], pd.tables[1]], ...prof.windows.slice(2)];
}

// The windows a merge of this profile writes, [[addr, length]] (stockfw.layout): the placed windows and the descriptor
// for a profile that moves the code, else its stock windows. A board with several stock files plans each merge
// against its own file's layout, not the board's windows (their union)
export function layoutWindows(prof, fb) {
	const wins = placedWindows(prof).map(([a, n]) => [a, n]);
	return prof.placement ? [...wins, [fb.descriptor.addr, fb.descriptor.len]] : wins;
}

// The 128-byte descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h, stockfw.descriptor)
export function descriptor(prof, fb) {
	const wins = placedWindows(prof);
	const crcs = [prof.placed.code[0], prof.placed.tables[0], ...(prof.placed.extra || [])];
	const d = fb.descriptor;
	const words = [d.magic, d.version, prof.placement.tag, wins[0][0], wins[0][1], crcs[0], wins.length - 1];
	for (let i = 0; i < d.windows; i++) words.push(...(i + 1 < wins.length ? [wins[i + 1][0], wins[i + 1][1], crcs[i + 1]] : [0, 0, 0]));
	const out = new Uint8Array(d.len);
	const view = new DataView(out.buffer);
	words.forEach((w, i) => view.setUint32(4 * i, w >>> 0, true));
	view.setUint32(d.len - 4, crc32(out.subarray(0, d.len - 4)), true);
	return out;
}

// The decrypted stock windows as the merge writes them (stockfw.place): the code moved (its self references
// rewritten by the codec map), the table window cut to the codec's data with the islands' calls into the code
// re-encoded, the extra windows unchanged, then the descriptor; every part checked against the profile's "placed" hashes
export async function place(prof, segs, fb) {
	if (!prof.placement) return segs;
	const pl = prof.placement;
	const m = prof.map || byCodeSha(fb.codec_maps, prof.windows[0][2])[0];
	if (!m) throw new StockError(`no codec map for ${prof.name}: its code can't be moved`);
	const [[, code], [tabAddr, tables], ...extra] = segs;
	let moved, tab;
	try {
		moved = await moveCode(code, m, pl.code);
		tab = moveBranches(tabAddr, tables.slice(0, pl.tables_len ?? tables.length), m, pl.code);
	} catch (e) {
		if (e instanceof MapError) throw new StockError(`can't move the codec: ${e.message}`);
		throw e;
	}
	if (branchesIn(m, tabAddr, tab.length) !== (m.branches || []).length) throw new StockError("a branch into the code lies outside the tables the merge keeps");
	const want = placedWindows(prof);
	const got = [moved, tab, ...extra.map(([, d]) => d)];
	for (const [i, d] of got.entries()) {
		if ((await sha256(d)) !== want[i][2]) throw new StockError("the moved codec doesn't match the profile");
	}
	return [[pl.code, moved], [tabAddr, tab], ...extra, [fb.descriptor.addr, descriptor(prof, fb)]];
}

// The codec windows decrypted, checked and placed as the merge writes them: [[addr, bytes], ...] (code first)
export async function extract(data, key, fb, profiles = fb.profiles) {
	const sha = await sha256(data);
	const prof = profiles[sha];
	if (!prof) {
		const known = fb.known_files[sha];
		const names = Object.values(profiles).map((p) => p.name).join(", ");
		throw new StockError(`${known ? "this file is " + known : "unknown stock file"}, not a supported codec source. Supported: ${names}`);
	}
	if (data.length !== prof.size) throw new StockError(`size ${data.length}, expected ${prof.size}`);
	const segs = parseContainer(data, fb);
	if (JSON.stringify(segs) !== JSON.stringify(prof.segments)) throw new StockError("unexpected segment table");
	const out = [];
	for (const [i, [addr, length, want]] of prof.windows.entries()) {
		const plain = decrypt(data, appFileOffset(segs, addr, fb), length, key, fb);
		const got = await sha256(plain);
		if (got !== want) throw new StockError(`decrypted ${i === 0 ? "code" : i === 1 ? "tables" : "table"} window does not match (wrong key?)`);
		out.push([addr, plain]);
	}
	if (u32le(out[0][1], 0) !== fb.codec_sig) throw new StockError("the codec's first word is wrong");
	return { profile: prof, segments: await place(prof, out, fb) };
}

// The whole application segment of a stock file, decrypted: what the stock firmware is in internal flash. Used to
// put a radio back on stock firmware (restore). Returns [addr, bytes].
export function stockApplication(data, key, board, fb) {
	const segs = parseContainer(data, fb);
	const seg = segs.find(([base]) => base === board.app_base);
	if (!seg) throw new StockError("the file has no application segment at the radio's application base");
	return [seg[0], decrypt(data, appFileOffset(segs, seg[0], fb), seg[1], key, fb)];
}

// Recover the 1024-byte bootloader key from a stock file, checked against its published SHA-256: the same steps and
// tie orders as odrflash's stockfw.derive_key, so both give the same key. The payload is plain XOR key[i mod 1024].
// The column vote is enough for a DM-1701 image (0x00 dominates every column); the MD-380 image votes about a third of
// the columns wrong, so a byte-pair model learned from the file itself (refine) re-picks them. A wrong key can't pass
// the hash check, and no key material is needed or stored.
export async function deriveKey(data, wantSha, fb, { maxAlternatives = 2, candidates = 24 } = {}) {
	const segs = parseContainer(data, fb);
	const total = segs.reduce((a, s) => a + s[1], 0);
	const payload = data.subarray(fb.header_len, fb.header_len + total);
	const K = fb.key_len;
	if (payload.length < 64 * K) throw new StockError("the file is too short to derive the key from");
	const cols = [];
	for (let j = 0; j < K; j++) cols.push(mostCommon(payload, j, K, 8));
	const base = Uint8Array.from(cols.map((c) => c[0][0]));
	if (await sha256(base) === wantSha) return base;
	const cands = cols.map((c) => c.map((x) => x[0]));
	const margin = cols.map((c) => c[0][1] - (c.length > 1 ? c[1][1] : 0));
	const doubtful = stableOrder(K, (j) => margin[j]).slice(0, candidates);
	let k = await alternatives(base, cands.map((c) => c.slice(0, 4)), doubtful, wantSha, maxAlternatives);
	if (k) return k;
	// cipher byte pairs (column j-1, column j) per column: column 0 pairs with the previous row's last column
	const pairs = [];
	for (let j = 0; j < K; j++) {
		const m = new Map();
		const start = j || K;
		for (let i = start; i < payload.length; i += K) {
			const p = payload[i - 1] << 8 | payload[i];
			m.set(p, (m.get(p) || 0) + 1);
		}
		pairs.push(m);
	}
	const rel = stableOrder(K, (j) => (cols[j].length > 1 ? -(cols[j][0][1] - cols[j][1][1]) / cols[j][0][1] : -1));
	for (const [frac, strict] of [[0.4, false], [0.4, true], [0.3, false], [0.3, true], [0.2, true]]) {
		const [refined, scoreMargin] = refine(pairs, cands, base, rel.slice(0, Math.floor(K * frac)), strict, K);
		if (await sha256(refined) === wantSha) return refined;
		const order = stableOrder(K, (j) => scoreMargin[j]).slice(0, Math.floor(candidates / 2));
		const alt = cands.map((c, j) => [refined[j], ...c.filter((b) => b !== refined[j]).slice(0, 2)]);
		k = await alternatives(refined, alt, order, wantSha, maxAlternatives);
		if (k) return k;
	}
	throw new StockError("could not derive the bootloader key from this file (is it a stock firmware file for this radio?)");
}

// Column j's most common bytes, [[byte, count]], count down, ties in first-seen order (Python's Counter.most_common)
function mostCommon(payload, j, K, n) {
	const count = new Uint32Array(256), first = new Int32Array(256).fill(-1);
	let seen = 0;
	for (let i = j; i < payload.length; i += K) {
		const b = payload[i];
		if (first[b] < 0) first[b] = seen++;
		count[b]++;
	}
	return [...Array(256).keys()].filter((b) => count[b]).sort((a, b) => count[b] - count[a] || first[a] - first[b])
		.slice(0, n).map((b) => [b, count[b]]);
}

// Indices 0..n-1 sorted by key, ties by index (a stable sort)
function stableOrder(n, key) {
	return [...Array(n).keys()].sort((a, b) => key(a) - key(b) || a - b);
}

// The next candidates in the least certain columns (order), one or two at a time, until the key matches its hash
async function alternatives(base, cands, order, wantSha, maxAlternatives) {
	for (let r = 1; r <= maxAlternatives; r++) {
		for (const js of combinations(order, r)) {
			for (const alts of product(js.map((j) => cands[j].slice(1)))) {
				const k = base.slice();
				js.forEach((j, n) => { k[j] = alts[n]; });
				if (await sha256(k) === wantSha) return k;
			}
		}
	}
	return null;
}

// Bigram refinement of a voted key (odrflash stockfw._refine). pairs[j] counts the cipher byte pairs (column j-1,
// column j), so a key guess gives the plain pair counts without touching the payload again; even and odd columns keep
// separate models (Thumb-2 halfwords, little endian words). Round 0 learns only from the seed columns (the clearest
// votes) and re-picks the others among their vote candidates (strict: only next to a seed column, judged against its
// seed neighbors alone); later rounds learn from the whole image and re-pick every column until nothing changes.
// Returns [key, per-column score margin].
function refine(pairs, cands, start, seedColumns, strict, K, rounds = 8) {
	const key = Uint8Array.from(start);
	const seed = new Set(seedColumns);
	const margin = new Float64Array(K);
	const logp = [new Float64Array(65536), new Float64Array(65536)];
	for (let rnd = 0; rnd < rounds; rnd++) {
		const counts = [new Float64Array(65536).fill(1), new Float64Array(65536).fill(1)];
		for (let j = 0; j < K; j++) {
			const jl = (j - 1 + K) % K;
			if (rnd === 0 && !(seed.has(j) && seed.has(jl))) continue;
			const m = counts[jl & 1], kl = key[jl], kj = key[j];
			for (const [p, n] of pairs[j]) m[((p >> 8) ^ kl) << 8 | ((p & 0xFF) ^ kj)] += n;
		}
		for (let s = 0; s < 2; s++) for (let i = 0; i < 65536; i++) logp[s][i] = Math.log(counts[s][i]);
		let changed = 0;
		for (let j = 0; j < K; j++) {
			if (rnd === 0 && seed.has(j)) continue;
			const jl = (j - 1 + K) % K, jr = (j + 1) % K;
			const left = logp[jl & 1], right = logp[j & 1];
			const kl = key[jl], kr = key[jr];
			// strict: round 0 trusts only seed neighbors (two wrong neighbors can hold each other in place)
			const trust = rnd !== 0 || !strict;
			const useL = trust || seed.has(jl), useR = trust || seed.has(jr);
			if (!useL && !useR) continue;
			const scored = [];
			for (const k of cands[j]) {
				let sc = 0;
				if (useL) for (const [p, n] of pairs[j]) sc += n * left[((p >> 8) ^ kl) << 8 | ((p & 0xFF) ^ k)];
				if (useR) for (const [p, n] of pairs[jr]) sc += n * right[((p >> 8) ^ k) << 8 | ((p & 0xFF) ^ kr)];
				scored.push([sc, k]);
			}
			scored.sort((a, b) => b[0] - a[0] || b[1] - a[1]);
			margin[j] = scored.length > 1 ? scored[0][0] - scored[1][0] : 1e9;
			if (scored[0][1] !== key[j]) {
				key[j] = scored[0][1];
				changed++;
			}
		}
		if (rnd && !changed) break;
	}
	return [key, margin];
}

function* combinations(items, r, start = 0, acc = []) {
	if (acc.length === r) {
		yield acc.slice();
		return;
	}
	for (let i = start; i < items.length; i++) {
		acc.push(items[i]);
		yield* combinations(items, r, i + 1, acc);
		acc.pop();
	}
}

function* product(lists, i = 0, acc = []) {
	if (i === lists.length) {
		yield acc.slice();
		return;
	}
	for (const v of lists[i]) {
		acc.push(v);
		yield* product(lists, i + 1, acc);
		acc.pop();
	}
}
