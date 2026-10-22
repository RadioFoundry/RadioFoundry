// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The YAML subset a qdmr codeplug file uses (docs/spec/qdmr-yaml.md 2): block maps and lists, flow maps and lists,
// plain, single- and double-quoted scalars, literal and folded block scalars, comments, document markers and local
// tags (!default). Anchors, aliases, complex keys and multiple documents are refused with a YamlError. Written from
// the YAML 1.2 specification; runs in browsers and Node.

export class YamlError extends Error {
	constructor(message, line) {
		super(line ? `line ${line}: ${message}` : message);
		this.line = line;
	}
}

// A tagged value: !default "" parses to new Tag("!default", "")
export class Tag {
	constructor(tag, value) {
		this.tag = tag;
		this.value = value;
	}
}

// Wraps a map or list the emitter writes in flow style ({a: 1} or [a, b])
export class Flow {
	constructor(value) {
		this.value = value;
	}
}

export const flow = (v) => new Flow(v);

// --- Parser ---

// Lines with their indent, comments removed; blank and comment-only lines dropped. A line that holds a block scalar's
// text keeps its raw form, since # there is text
function lines(text) {
	const out = [];
	const src = String(text).replace(/^﻿/, "").split(/\r?\n/);
	for (let i = 0; i < src.length; i++) {
		const raw = src[i];
		if (raw.includes("\t") && /^\s*\t/.test(raw)) throw new YamlError("tab in indentation", i + 1);
		const body = stripComment(raw);
		const t = body.trim();
		if (t === "" ) {
			out.push({ indent: -1, text: "", raw, line: i + 1 });
			continue;
		}
		if (/^(---|\.\.\.)(\s|$)/.test(body)) {
			if (t.startsWith("...")) break;
			if (out.some((l) => l.indent >= 0)) throw new YamlError("more than one document", i + 1);
			const rest = t.slice(3).trim();
			if (rest) out.push({ indent: 0, text: rest, raw, line: i + 1 });
			continue;
		}
		if (t.startsWith("%")) continue;	// a directive
		out.push({ indent: body.length - body.trimStart().length, text: t, raw, line: i + 1 });
	}
	return out;
}

// The line without its comment: # after whitespace (or at the start) and outside quotes
function stripComment(s) {
	let q = null;
	for (let i = 0; i < s.length; i++) {
		const c = s[i];
		if (q) {
			if (c === "\\" && q === '"') i++;
			else if (c === q) {
				if (q === "'" && s[i + 1] === "'") i++;
				else q = null;
			}
		} else if (c === '"' || c === "'") {
			if (i === 0 || /[\s[{,:-]/.test(s[i - 1])) q = c;
		} else if (c === "#" && (i === 0 || /\s/.test(s[i - 1]))) {
			return s.slice(0, i).replace(/\s+$/, "");
		}
	}
	return s.replace(/\s+$/, "");
}

export function parse(text) {
	const p = new BlockParser(lines(text));
	p.skipBlank();
	if (p.done()) return null;
	const v = p.node(p.cur().indent);
	p.skipBlank();
	if (!p.done()) throw new YamlError("unexpected text", p.cur().line);
	return v;
}

class BlockParser {
	constructor(ls) {
		this.ls = ls;
		this.i = 0;
	}

	done() {
		return this.i >= this.ls.length;
	}

	cur() {
		return this.ls[this.i];
	}

	skipBlank() {
		while (!this.done() && this.cur().indent < 0) this.i++;
	}

	// The node whose first line is the current one, at this indent
	node(indent) {
		const l = this.cur();
		if (isSeqItem(l.text)) return this.seq(indent);
		if (keyOf(l.text, l.line)) return this.map(indent);
		this.i++;
		return this.inlineRest(l.text, l, indent);
	}

	seq(indent) {
		const out = [];
		for (;;) {
			this.skipBlank();
			if (this.done()) break;
			const l = this.cur();
			if (l.indent !== indent || !isSeqItem(l.text)) {
				if (l.indent > indent) throw new YamlError("bad indentation", l.line);
				break;
			}
			const rest = l.text.slice(1).trimStart();
			if (rest === "") {
				this.i++;
				out.push(this.child(indent));
			} else {
				// "- key: value" opens a map whose keys line up with "key"; "- - x" a nested list
				const inner = l.indent + (l.text.length - rest.length);
				if (isSeqItem(rest) || keyOf(rest, l.line)) {
					this.ls[this.i] = { ...l, indent: inner, text: rest };
					out.push(this.node(inner));
				} else {
					this.i++;
					out.push(this.inlineRest(rest, l, indent));
				}
			}
		}
		return out;
	}

	map(indent) {
		const out = {};
		for (;;) {
			this.skipBlank();
			if (this.done()) break;
			const l = this.cur();
			if (l.indent !== indent) {
				if (l.indent > indent) throw new YamlError("bad indentation", l.line);
				break;
			}
			if (isSeqItem(l.text)) break;
			const kv = keyOf(l.text, l.line);
			if (!kv) throw new YamlError("expected a key", l.line);
			if (Object.prototype.hasOwnProperty.call(out, kv.key)) throw new YamlError(`duplicate key ${kv.key}`, l.line);
			this.i++;
			if (kv.rest === "") {
				// the value is on the following lines: deeper, or a list at the same indent
				this.skipBlank();
				if (!this.done() && this.cur().indent === indent && isSeqItem(this.cur().text)) out[kv.key] = this.seq(indent);
				else out[kv.key] = this.child(indent);
			} else {
				out[kv.key] = this.inlineRest(kv.rest, l, indent);
			}
		}
		return out;
	}

	// The block node below a line at indent, or null if none follows
	child(indent) {
		this.skipBlank();
		if (this.done() || this.cur().indent <= indent) return null;
		return this.node(this.cur().indent);
	}

	// An inline value; a block scalar or a flow collection may continue on the following lines
	inlineRest(text, l, indent) {
		let tag = null;
		let m = /^(![\w!-]*)(?:\s+|$)/.exec(text);
		if (m) {
			tag = m[1];
			text = text.slice(m[0].length);
		}
		if (/^[&*]/.test(text)) throw new YamlError("anchors and aliases are not supported", l.line);
		let v;
		if (/^[|>][-+0-9]*$/.test(text)) {
			v = this.blockScalar(text, indent);
		} else if (text === "" && tag) {
			v = this.child(indent);
		} else {
			// a flow collection or a quoted scalar left open continues on the next lines
			while (open(text) && !this.done()) {
				const n = this.cur();
				this.i++;
				if (n.indent >= 0) text += " " + n.text;
			}
			const f = new FlowParser(text, l.line);
			v = f.value(false);
			f.end();
		}
		return tag ? new Tag(tag, v) : v;
	}

	blockScalar(head, indent) {
		const folded = head[0] === ">";
		const chomp = head.includes("-") ? "strip" : head.includes("+") ? "keep" : "clip";
		const body = [];
		let ind = null;
		while (!this.done()) {
			const l = this.cur();
			const lead = l.raw.length - l.raw.trimStart().length;
			if (l.raw.trim() !== "" && lead <= indent) break;
			if (ind === null && l.raw.trim() !== "") ind = lead;
			body.push(l.raw.trim() === "" ? "" : l.raw.slice(ind ?? lead));
			this.i++;
		}
		let s;
		if (folded) {
			s = "";
			body.forEach((x, k) => {
				if (k === 0) s = x;
				else if (x === "" || body[k - 1] === "") s += "\n" + x;
				else s += " " + x;
			});
		} else s = body.join("\n");
		s = s.replace(/\n+$/, "");
		if (chomp === "clip" && s) s += "\n";
		if (chomp === "keep") s += "\n";
		return s;
	}
}

const isSeqItem = (t) => t === "-" || t.startsWith("- ");

// {key, rest} if the text starts a map entry ("key: value" or "key:"), else null
function keyOf(text, line) {
	if (text.startsWith("? ")) throw new YamlError("complex keys are not supported", line);
	if (text[0] === "{" || text[0] === "[") return null;
	let key, i;
	if (text[0] === '"' || text[0] === "'") {
		const f = new FlowParser(text, line);
		key = f.quoted();
		i = f.i;
	} else {
		const m = /^[^#:]*?(?=:(\s|$))/.exec(text);
		if (!m || m[0] === "" || /^[-?|>!&*%@`]/.test(m[0]) && !/^-\w/.test(m[0])) return null;
		key = m[0].trim();
		i = m[0].length;
	}
	if (text[i] !== ":" || (i + 1 < text.length && !/\s/.test(text[i + 1]))) return null;
	return { key: String(key), rest: text.slice(i + 1).trim() };
}

// True while a flow collection or a quoted scalar in the text isn't closed
function open(text) {
	let depth = 0, q = null;
	for (let i = 0; i < text.length; i++) {
		const c = text[i];
		if (q) {
			if (c === "\\" && q === '"') i++;
			else if (c === q) {
				if (q === "'" && text[i + 1] === "'") i++;
				else q = null;
			}
		} else if ((c === '"' || c === "'") && (i === 0 || /[\s[{,:]/.test(text[i - 1]))) q = c;
		else if (c === "[" || c === "{") depth++;
		else if (c === "]" || c === "}") depth--;
	}
	return q !== null || depth > 0;
}

class FlowParser {
	constructor(s, line) {
		this.s = s;
		this.i = 0;
		this.line = line;
	}

	err(msg) {
		return new YamlError(msg, this.line);
	}

	ws() {
		while (this.i < this.s.length && /\s/.test(this.s[this.i])) this.i++;
	}

	end() {
		this.ws();
		if (this.i < this.s.length) throw this.err(`unexpected "${this.s.slice(this.i, this.i + 10)}"`);
	}

	// One value; inFlow: inside [] or {}, where , ] } end a plain scalar
	value(inFlow) {
		this.ws();
		let tag = null;
		const m = /^![\w!-]*/.exec(this.s.slice(this.i));
		if (m) {
			tag = m[0];
			this.i += m[0].length;
			this.ws();
		}
		const c = this.s[this.i];
		let v;
		if (c === "&" || c === "*") throw this.err("anchors and aliases are not supported");
		if (c === "[") v = this.list();
		else if (c === "{") v = this.mapping();
		else if (c === '"' || c === "'") v = this.quoted();
		else v = this.plain(inFlow);
		return tag ? new Tag(tag, v) : v;
	}

	list() {
		const out = [];
		this.i++;
		for (;;) {
			this.ws();
			if (this.s[this.i] === "]") {
				this.i++;
				return out;
			}
			out.push(this.value(true));
			this.ws();
			if (this.s[this.i] === ",") this.i++;
			else if (this.s[this.i] !== "]") throw this.err("expected , or ]");
		}
	}

	mapping() {
		const out = {};
		this.i++;
		for (;;) {
			this.ws();
			if (this.s[this.i] === "}") {
				this.i++;
				return out;
			}
			const k = this.s[this.i] === '"' || this.s[this.i] === "'" ? this.quoted() : this.plain(true, true);
			this.ws();
			let v = null;
			if (this.s[this.i] === ":") {
				this.i++;
				v = this.value(true);
			}
			out[String(k)] = v;
			this.ws();
			if (this.s[this.i] === ",") this.i++;
			else if (this.s[this.i] !== "}") throw this.err("expected , or }");
		}
	}

	quoted() {
		const q = this.s[this.i++];
		let out = "";
		for (;;) {
			if (this.i >= this.s.length) throw this.err("unclosed quote");
			const c = this.s[this.i++];
			if (c === q) {
				if (q === "'" && this.s[this.i] === "'") {
					out += "'";
					this.i++;
					continue;
				}
				return out;
			}
			if (c === "\\" && q === '"') out += this.escape();
			else out += c;
		}
	}

	escape() {
		const c = this.s[this.i++];
		const simple = { 0: "\0", a: "\x07", b: "\b", t: "\t", "\t": "\t", n: "\n", v: "\v", f: "\f", r: "\r", e: "\x1b",
			" ": " ", '"': '"', "/": "/", "\\": "\\", N: "\x85", _: "\xa0", L: "\u2028", P: "\u2029" };
		if (c in simple) return simple[c];
		const len = { x: 2, u: 4, U: 8 }[c];
		if (!len) throw this.err(`unknown escape \\${c}`);
		const hex = this.s.slice(this.i, this.i + len);
		if (!/^[0-9a-fA-F]+$/.test(hex) || hex.length !== len) throw this.err("bad escape");
		this.i += len;
		return String.fromCodePoint(parseInt(hex, 16));
	}

	// A plain scalar, resolved to null, a boolean, a number or a string; asKey: stops at ':' too
	plain(inFlow, asKey = false) {
		const start = this.i;
		while (this.i < this.s.length) {
			const c = this.s[this.i];
			if (inFlow && (c === "," || c === "]" || c === "}")) break;
			if (c === ":" && (asKey || inFlow) && (this.i + 1 >= this.s.length || /[\s,\]}]/.test(this.s[this.i + 1]))) break;
			this.i++;
		}
		return resolve(this.s.slice(start, this.i).trim());
	}
}

// The core schema's resolution of a plain scalar
export function resolve(s) {
	if (s === "" || s === "~" || /^(null|Null|NULL)$/.test(s)) return null;
	if (/^(true|True|TRUE)$/.test(s)) return true;
	if (/^(false|False|FALSE)$/.test(s)) return false;
	if (/^[-+]?\d+$/.test(s)) return Number(s);
	if (/^0x[0-9a-fA-F]+$/.test(s)) return parseInt(s, 16);
	if (/^[-+]?(\.\d+|\d+(\.\d*)?)([eE][-+]?\d+)?$/.test(s)) return Number(s);
	return s;
}

// --- Emitter ---

// YAML text for a value. Maps and lists are written in block style unless wrapped in Flow; Tag values as !tag value
export function stringify(v) {
	const out = [];
	block(v, 0, out);
	return out.join("\n") + "\n";
}

function isCollection(v) {
	return Array.isArray(v) || (v !== null && typeof v === "object" && !(v instanceof Tag) && !(v instanceof Flow));
}

function block(v, indent, out) {
	const pad = " ".repeat(indent);
	if (Array.isArray(v)) {
		if (!v.length) return out.push(pad + "[]");
		for (const x of v) {
			if (isCollection(x) && !Array.isArray(x) && Object.keys(x).length) {
				const sub = [];
				block(x, indent + 2, sub);
				out.push(pad + "- " + sub[0].slice(indent + 2), ...sub.slice(1));
			} else if (Array.isArray(x) && x.length) {
				out.push(pad + "-");
				block(x, indent + 2, out);
			} else out.push(pad + "- " + inline(x));
		}
		return;
	}
	if (isCollection(v)) {
		const keys = Object.keys(v);
		if (!keys.length) return out.push(pad + "{}");
		for (const k of keys) {
			const x = v[k];
			if (x === undefined) continue;
			if (isCollection(x) && (Array.isArray(x) ? x.length : Object.keys(x).length)) {
				out.push(`${pad}${scalar(k)}:`);
				// a list under a key is indented by two, as qdmr writes it
				block(x, indent + 2, out);
			} else out.push(`${pad}${scalar(k)}: ${inline(x)}`);
		}
		return;
	}
	out.push(pad + inline(v));
}

function inline(v) {
	if (v instanceof Tag) return `${v.tag} ${inline(v.value)}`.trimEnd();
	if (v instanceof Flow) v = v.value;
	if (Array.isArray(v)) return `[${v.map(inline).join(", ")}]`;
	if (v !== null && typeof v === "object") {
		return `{${Object.keys(v).filter((k) => v[k] !== undefined).map((k) => `${scalar(k)}: ${inline(v[k])}`).join(", ")}}`;
	}
	return scalar(v);
}

// A scalar, quoted when plain text would read back as something else
export function scalar(v) {
	if (v === null || v === undefined) return "~";
	if (typeof v === "boolean") return String(v);
	if (typeof v === "number") return Number.isFinite(v) ? String(v) : ".nan";
	const s = String(v);
	const plainOk = s !== "" && s === s.trim() && resolve(s) === s && !/^[-?:,[\]{}#&*!|>'"%@`]/.test(s) &&
		!/[:#,[\]{}]/.test(s) && !/[\x00-\x1f\x7f-\x9f\u2028\u2029]/.test(s);
	if (plainOk) return s;
	return '"' + s.replace(/[\\"\x00-\x1f\x7f-\x9f\u2028\u2029]/g, (c) => ({ "\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t", "\r": "\\r" }[c] ||
		"\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"))) + '"';
}
