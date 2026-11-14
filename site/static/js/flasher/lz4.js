// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// LZ4 blocks (the open LZ4 block format): the decoder for the sectors the C7000 stub sends compressed, a port of
// lz4_block() in stub/stubtool.py, and a small greedy encoder (stubtool's lz4_encode) for the simulated radio and the
// fast codeplug protocol. Both take an optional dictionary: bytes as if just before the block, which matches may reach
// into (a patch block, docs/spec/cps-fast.md 3.7).

export class Lz4Error extends Error {}

// Decode one block to exactly `size` bytes
export function decodeBlock(src, size, dict = null) {
	const dn = dict ? dict.length : 0;
	const out = new Uint8Array(dn + size);
	if (dn) out.set(dict);
	size += dn;
	let i = 0, o = dn;
	const n = src.length;
	const ext = (v) => {
		for (;;) {
			if (i >= n) throw new Lz4Error("truncated LZ4 block");
			const b = src[i++];
			v += b;
			if (b !== 255) return v;
		}
	};
	while (i < n) {
		const tok = src[i++];
		let lit = tok >> 4;
		if (lit === 15) lit = ext(lit);
		if (i + lit > n || o + lit > size) throw new Lz4Error("corrupt LZ4 block");
		out.set(src.subarray(i, i + lit), o);
		i += lit;
		o += lit;
		if (i >= n) break;
		if (i + 2 > n) throw new Lz4Error("truncated LZ4 block");
		const off = src[i] | (src[i + 1] << 8);
		i += 2;
		let ml = tok & 15;
		if (ml === 15) ml = ext(ml);
		ml += 4;
		if (off === 0 || off > o || o + ml > size) throw new Lz4Error("corrupt LZ4 block");
		for (let k = 0; k < ml; k++, o++) out[o] = out[o - off];
	}
	if (o !== size) throw new Lz4Error(`LZ4 block decodes to ${o - dn} bytes, wanted ${size - dn}`);
	return dn ? out.subarray(dn) : out;
}

// Encode src. With a dictionary each 4-byte key keeps its last few places and the longest match wins; without, the
// last place only (as the radio's encoder)
export function encodeBlock(src, dict = null) {
	const dn = dict ? dict.length : 0;
	let buf = src;
	if (dn) {
		buf = new Uint8Array(dn + src.length);
		buf.set(dict);
		buf.set(src, dn);
	}
	const ways = dn ? 4 : 1;
	const out = [];
	const seen = new Map();
	let ip = 0, anchor = 0;
	const n = src.length;
	const ext = (v) => {
		while (v >= 255) {
			out.push(255);
			v -= 255;
		}
		out.push(v);
	};
	const keyAt = (a) => (buf[a] | (buf[a + 1] << 8) | (buf[a + 2] << 16) | (buf[a + 3] << 24)) >>> 0;
	const remember = (key, at) => {
		const l = seen.get(key);
		if (!l) seen.set(key, [at]);
		else {
			l.push(at);
			if (l.length > ways) l.shift();
		}
	};
	const literals = (from, to) => { for (let k = from; k < to; k++) out.push(src[k]); };
	for (let a = 0; a + 3 < dn; a++) remember(keyAt(a), a);
	while (n >= 13 && ip <= n - 12) {
		const at = dn + ip, key = keyAt(at);
		let best = 0, bref = 0;
		const l = seen.get(key) || [];
		for (let j = l.length - 1; j >= 0; j--) {
			const ref = l[j];
			if (at - ref >= 65536) continue;
			let ml = 4;
			while (ip + ml < n - 5 && buf[ref + ml] === buf[at + ml]) ml++;
			if (ml > best) {
				best = ml;
				bref = ref;
			}
		}
		remember(key, at);
		if (best) {
			const lit = ip - anchor;
			out.push((Math.min(lit, 15) << 4) | Math.min(best - 4, 15));
			if (lit >= 15) ext(lit - 15);
			literals(anchor, ip);
			out.push((at - bref) & 0xFF, (at - bref) >> 8);
			if (best - 4 >= 15) ext(best - 19);
			if (dn) for (let k = 1; k < best; k++) if (ip + k <= n - 12) remember(keyAt(at + k), at + k);
			ip += best;
			anchor = ip;
			continue;
		}
		ip++;
	}
	const lit = n - anchor;
	out.push(Math.min(lit, 15) << 4);
	if (lit >= 15) ext(lit - 15);
	literals(anchor, n);
	return Uint8Array.from(out);
}
