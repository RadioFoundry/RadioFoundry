// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LZ4 blocks (the open LZ4 block format): the decoder for the sectors the C7000 stub sends compressed, a port of
// lz4_block() in stub/stubtool.py, and a small greedy encoder the simulated radio uses (stubtool's lz4_encode).

export class Lz4Error extends Error {}

// Decode one block to exactly `size` bytes
export function decodeBlock(src, size) {
	const out = new Uint8Array(size);
	let i = 0, o = 0;
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
	if (o !== size) throw new Lz4Error(`LZ4 block decodes to ${o} bytes, wanted ${size}`);
	return out;
}

export function encodeBlock(src) {
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
	const literals = (from, to) => { for (let k = from; k < to; k++) out.push(src[k]); };
	while (n >= 13 && ip <= n - 12) {
		const key = (src[ip] | (src[ip + 1] << 8) | (src[ip + 2] << 16) | (src[ip + 3] << 24)) >>> 0;
		const ref = seen.get(key);
		seen.set(key, ip);
		if (ref !== undefined && ip - ref < 65536) {
			let ml = 4;
			while (ip + ml < n - 5 && src[ref + ml] === src[ip + ml]) ml++;
			const lit = ip - anchor;
			out.push((Math.min(lit, 15) << 4) | Math.min(ml - 4, 15));
			if (lit >= 15) ext(lit - 15);
			literals(anchor, ip);
			out.push((ip - ref) & 0xFF, (ip - ref) >> 8);
			if (ml - 4 >= 15) ext(ml - 19);
			ip += ml;
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
