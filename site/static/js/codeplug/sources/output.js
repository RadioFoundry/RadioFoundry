// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// What a contacts source's users become in a codeplug: the DMR ID database, or private-call contacts. Both work on the
// editing model (model.js); the database is written into a new image that replaces the model's base, so undo can put
// the old one back.

import * as iddb from "../iddb.js";
import { LIMITS } from "../records.js";
import { newContact } from "../model.js";
import { format } from "./radioid.js";
import { foldLetters } from "../fields.js";

// The room for the ID database: a radio's codeplug end as an image address (radios.json codeplug {base, end})
export function iddbEnd(radio) {
	const cp = radio?.codeplug;
	return cp && cp.end > cp.base ? cp.end - cp.base : Infinity;
}

// The ID database's size for n users at textLen characters, and how many fit before end
export function iddbSize(n, textLen, end) {
	const rec = iddb.recordSize(textLen);
	const fit = iddb.capacity(rec, end);
	return { rec, bytes: iddb.HEADER + Math.min(n, fit) * rec, fit };
}

// Replace the model's ID database with these users. Returns {count, dropped (no room), top}
export function writeIddb(m, users, { template, upper = false, textLen = 32, end = Infinity }) {
	const img = m.base.clone();
	const res = iddb.write(img, users.map((u) => ({ id: u.id, text: format(u, template, { upper, max: textLen }) })), { textLen, end });
	m.base = img;
	m.iddb = iddb.readHeader(img.bytes);
	return res;
}

export function eraseIddb(m) {
	const img = m.base.clone();
	iddb.erase(img);
	m.base = img;
	m.iddb = null;
}

// Names are stored in Latin-1: other characters lose their accents (Ł -> L) or are dropped
function latin1(s) {
	return [...s].map((c) => (c.charCodeAt(0) < 0x100 ? c : foldLetters(c))).map((c) => (c.charCodeAt(0) < 0x100 ? c : c.normalize("NFD").replace(/[^\u0000-\u00FF]/g, ""))).join("").replace(/\s+/g, " ").trim();
}

// Add users as private-call contacts. A user whose ID is already a private contact is skipped; when the contact list
// fills, the rest are left out. Returns {added, existing, full}
export function addContacts(m, users, { template, upper = false }) {
	const have = new Set(m.contacts.filter((c) => c.type === "private").map((c) => c.id));
	let added = 0, existing = 0, full = 0;
	for (const u of users) {
		if (have.has(u.id)) {
			existing++;
			continue;
		}
		if (m.contacts.length >= LIMITS.contacts) {
			full++;
			continue;
		}
		const name = latin1(format(u, template, { upper })).slice(0, LIMITS.name).trim() || u.call;
		m.contacts.push(newContact({ name, id: u.id, type: "private" }));
		have.add(u.id);
		added++;
	}
	return { added, existing, full };
}
