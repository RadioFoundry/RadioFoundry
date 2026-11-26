// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 channels and the contacts they name (docs/spec/air-contacts.md, p25-codeplug.md): a codeplug from before P25
// contacts has P25 channels whose talk target and followed groups are DMR contacts. Converting makes them P25
// contacts, so the channels are written with the contact flag; and a radio whose firmware doesn't know the flag is
// warned about before a write.

import { LIMITS, P25_ALL_CALL } from "./records.js";
import { numberTable } from "./model.js";
import { isDmr, isP25, channelContactMode, wantsP25Contacts, flaggedChannels, scopedContact } from "./contactmode.js";
import { hasFeature } from "./modes.js";

// The firmware feature token of a radio that reads the contact flag (build_features_now)
export const P25_CONTACTS_FEATURE = "p25contacts";

// How many channels would be written with the contact flag to a radio that can't read it: they would talk to the wrong
// talk group. 0 with no radio (null features: a file is saved) or one that has the feature
export const unreadFlagged = (m, features) => (features == null || hasFeature(features, P25_CONTACTS_FEATURE) ? 0 : flaggedChannels(m).length);

const dmrMembers = (l) => (l && typeof l === "object" ? l.contacts.filter(isDmr) : []);

// The channels of a kind that reads P25 contacts which still name DMR ones: by their talk target, or by the groups of
// the RX group list
export const p25ChannelsToConvert = (m) => m.channels.filter((c) => c.kind && c.params && channelContactMode(c) === "p25" && !wantsP25Contacts(c) &&
	((c.contact && typeof c.contact === "object" && isDmr(c.contact)) || dmrMembers(c.rxList).length > 0));

// What a DMR contact is as a P25 one: {type, id}, or null when P25 has no such entry (a unit ID above 0xFFFFFE, a group
// above 65535)
function asP25(d) {
	if (d.type === "all") return { type: "all", id: P25_ALL_CALL };
	if (d.type === "private") return d.id >= 1 && d.id <= 0xFFFFFE ? { type: "unit", id: d.id } : null;
	return d.id >= 1 && d.id <= 0xFFFF ? { type: "group", id: d.id } : null;
}

// The channels of kind P25 made to read P25 contacts, in model m (changed in place; the caller makes it one undoable
// change). Each talk target they use becomes a P25 contact of the same name, call type and ID (one already there is
// reused); an RX group list only they use is converted, one shared with other channels copied for them. A member or a
// talk target with no P25 equivalent is left as it is: the Problems tab says what is wrong with it.
// Returns {channels, contacts: new P25 contacts, lists: converted, copies: copied lists, left: members and talk targets
// left, full: a copy wanted with no room for one}
export function convertP25Channels(m) {
	const todo = p25ChannelsToConvert(m);
	const out = { channels: 0, contacts: 0, lists: 0, copies: 0, left: 0, full: false };
	const made = new Map();
	const key = (x) => `${x.type}:${x.id}`;
	const have = new Map(m.contacts.filter((c) => isP25(c) && !scopedContact(c)).map((x) => [key(x), x]));
	const toP25 = (d) => {
		if (made.has(d)) return made.get(d);
		const p = asP25(d);
		let c = null;
		if (p) {
			c = have.get(key(p));
			if (!c) {
				c = { number: null, mode: "p25", name: d.name, type: p.type, id: p.id, timeslot: 0 };
				m.contacts.push(c);
				have.set(key(p), c);
				out.contacts++;
			}
		}
		made.set(d, c);
		return c;
	};
	const convertList = (l) => l.contacts.map((x) => {
		if (!isDmr(x)) return x;
		const p = toP25(x);
		if (!p) out.left++;
		return p ?? x;
	});

	for (const c of todo) {
		if (c.contact && typeof c.contact === "object" && isDmr(c.contact)) {
			const p = toP25(c.contact);
			if (p) c.contact = p;
			else out.left++;
		}
		out.channels++;
	}

	// RX group lists: by use. One only the channels converted use is converted where it stands
	const doing = new Set(todo);
	const lists = new Set(todo.map((c) => c.rxList).filter((l) => l && typeof l === "object" && dmrMembers(l).length));
	const copies = new Map();
	for (const l of lists) {
		const users = m.channels.filter((c) => c.rxList === l);
		if (users.every((c) => doing.has(c))) {
			l.contacts = convertList(l);
			out.lists++;
			continue;
		}
		if (m.rxLists.length >= LIMITS.rxLists) {
			out.full = true;
			continue;
		}
		const copy = { number: null, name: `${l.name.slice(0, LIMITS.name - 4)} P25`, contacts: convertList(l) };
		m.rxLists.push(copy);
		copies.set(l, copy);
		out.copies++;
	}
	for (const c of todo) if (copies.has(c.rxList)) c.rxList = copies.get(c.rxList);

	numberTable(m, "contacts");
	numberTable(m, "rxLists");
	return out;
}
