// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A small zip writer and reader for backup bundles: stored or deflated entries (the browser's CompressionStream),
// no zip64, so a bundle stays under 4 GB (a full backup is about 17 MB before compression).

import { concat, crc32 } from "./bytes.js";

const enc = new TextEncoder();

async function streamBytes(bytes, transform) {
	const s = new Blob([bytes]).stream().pipeThrough(transform);
	return new Uint8Array(await new Response(s).arrayBuffer());
}

const deflate = (b) => streamBytes(b, new CompressionStream("deflate-raw"));
const inflate = (b) => streamBytes(b, new DecompressionStream("deflate-raw"));

function dosTime(d) {
	return {
		time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
		date: ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
	};
}

function header(fields) {
	const size = fields.reduce((a, [n]) => a + n, 0);
	const out = new Uint8Array(size);
	const v = new DataView(out.buffer);
	let o = 0;
	for (const [n, val] of fields) {
		if (n === 2) v.setUint16(o, val, true);
		else if (n === 4) v.setUint32(o, val, true);
		else out.set(val, o);
		o += n;
	}
	return out;
}

// entries: [{name, data: Uint8Array | string}] -> Uint8Array
export async function zip(entries, when = new Date()) {
	const { time, date } = dosTime(when);
	const local = [], central = [];
	let offset = 0;
	for (const e of entries) {
		const data = typeof e.data === "string" ? enc.encode(e.data) : e.data;
		const name = enc.encode(e.name);
		const crc = crc32(data);
		let body = data, method = 0;
		if (typeof CompressionStream !== "undefined" && data.length > 256) {
			const d = await deflate(data);
			if (d.length < data.length) { body = d; method = 8; }
		}
		const lh = header([[4, 0x04034B50], [2, 20], [2, 0x0800], [2, method], [2, time], [2, date], [4, crc],
			[4, body.length], [4, data.length], [2, name.length], [2, 0], [name.length, name]]);
		local.push(lh, body);
		central.push(header([[4, 0x02014B50], [2, 20], [2, 20], [2, 0x0800], [2, method], [2, time], [2, date], [4, crc],
			[4, body.length], [4, data.length], [2, name.length], [2, 0], [2, 0], [2, 0], [2, 0], [4, 0], [4, offset],
			[name.length, name]]));
		offset += lh.length + body.length;
	}
	const cd = concat(...central);
	const eocd = header([[4, 0x06054B50], [2, 0], [2, 0], [2, entries.length], [2, entries.length], [4, cd.length], [4, offset], [2, 0]]);
	return concat(...local, cd, eocd);
}

// Uint8Array -> Map(name -> Uint8Array); checks every CRC
export async function unzip(bytes) {
	const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	let e = bytes.length - 22;
	while (e >= 0 && v.getUint32(e, true) !== 0x06054B50) e--;
	if (e < 0) throw new Error("not a zip file");
	const count = v.getUint16(e + 10, true);
	let p = v.getUint32(e + 16, true);
	const out = new Map();
	const dec = new TextDecoder();
	for (let i = 0; i < count; i++) {
		if (v.getUint32(p, true) !== 0x02014B50) throw new Error("damaged zip directory");
		const method = v.getUint16(p + 10, true), crc = v.getUint32(p + 16, true), csize = v.getUint32(p + 20, true);
		const nlen = v.getUint16(p + 28, true), xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true);
		const lho = v.getUint32(p + 42, true);
		const name = dec.decode(bytes.subarray(p + 46, p + 46 + nlen));
		const dataAt = lho + 30 + v.getUint16(lho + 26, true) + v.getUint16(lho + 28, true);
		const body = bytes.subarray(dataAt, dataAt + csize);
		let data;
		if (method === 0) data = body.slice();
		else if (method === 8) data = await inflate(body);
		else throw new Error(`${name}: unsupported compression ${method}`);
		if (crc32(data) !== crc) throw new Error(`${name}: CRC mismatch, the file is damaged`);
		out.set(name, data);
		p += 46 + nlen + xlen + clen;
	}
	return out;
}
