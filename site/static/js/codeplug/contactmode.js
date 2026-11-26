// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Contact modes in the editing model (docs/spec/air-contacts.md): one list of contacts, each of one mode. The mode
// decides where it is stored: DMR contacts in the contact list (open-firmware-cps.md 7.3.5), P25 contacts in the air
// mode contact table, and its number is its slot in that list. A new contact has no mode (null) until one is chosen;
// a contact object without the field is a DMR contact, as the code that predates modes made them.

import { kindRow, hasFeature } from "./modes.js";
import { P25_ALL_CALL, P25_ID_MAX } from "./records.js";
import { systemIdentityCheck } from "./channelmodes.js";

export const DMR_ALL_CALL = 0xFFFFFF;
export const MODES = ["dmr", "p25"];

// An all call's ID in a mode ("p25", else DMR)
export const allCallId = (mode) => (mode === "p25" ? P25_ALL_CALL : DMR_ALL_CALL);

export const isP25 = (c) => !!c && typeof c === "object" && c.mode === "p25";
export const isDmr = (c) => !!c && typeof c === "object" && (c.mode === "dmr" || c.mode === undefined);

// A scoped contact keeps an actual System reference, or a dangling 1-based index and its stored check. Index 256
// writes FF: deleting a System cannot leave a contact that adopts a different System when its old slot is reused.
export const scopedContact = (c) => isP25(c) && c.system != null;
export function contactScope(c) {
	if (!scopedContact(c)) return {};
	if (typeof c.system !== "object") return { system: c.system, systemCheck: c.systemCheck ?? 0xFFFF };
	const check = systemIdentityCheck(c.system);
	return { system: check == null ? 256 : c.system.number ?? 256, systemCheck: check ?? c.systemCheck ?? 0xFFFF };
}

export function resolveContactSystems(m) {
	for (const c of m.contacts.filter(scopedContact)) {
		if (typeof c.system !== "number") continue;
		const s = m.systems.find((x) => x.number === c.system && x.type === 3 && systemIdentityCheck(x) === c.systemCheck);
		if (s) c.system = s;
	}
}

// Membership never uses the unscoped name-lookup fallback. Unscoped legacy contacts remain usable; a scoped one
// applies only to a trunked personality on that exact, complete System.
export const contactOnChannel = (ct, c) => !scopedContact(ct) || (c.kind === 3 && typeof ct.system === "object" && ct.system === c.system && systemIdentityCheck(ct.system) != null);
// "dmr", "p25", or null for a contact whose mode is not chosen yet
export const contactModeOf = (c) => (c.mode === undefined ? "dmr" : c.mode);
// The highest ID a contact may have: a P25 contact's by its call type, else the DMR range's
export const contactIdMax = (c) => (isP25(c) ? P25_ID_MAX[c.type] ?? P25_ID_MAX.group : DMR_ALL_CALL);
// The mode of the contacts a channel reads: its kind's, else DMR (channel-modes.md; a kind's module says
// contactMode)
export const channelContactMode = (c) => kindRow(c.kind)?.module.contactMode ?? "dmr";

// The call types of each mode, in the order a list shows them
export const CALL_TYPES_OF = { dmr: ["group", "private", "all"], p25: ["group", "unit", "all"] };

// A call type as the other mode names it: a private call is a P25 unit call. A type the mode has stays
export function typeFor(type, mode) {
	if (mode === "p25") return type === "private" ? "unit" : type;
	if (mode === "dmr") return type === "unit" ? "private" : type;
	return type;
}

// Contact c moved to a mode: its call type as the mode names it, an all call's ID the mode's, and no slot until
// model.js numberTable() gives it the first free one in the new list. The timeslot override stays (a P25 contact keeps
// it unused; it is dropped when the codeplug is saved, clearP25Timeslots)
export function setContactMode(c, mode) {
	if (c.mode === mode) return;
	c.mode = mode;
	if (mode !== "p25") { delete c.system; delete c.systemCheck; }
	c.type = typeFor(c.type, mode);
	if (c.type === "all") c.id = allCallId(mode);
	c.number = null;
}

// What a P25 contact has no field for is cleared from the rows: the air table has no timeslot override. Returns how
// many it cleared
export function clearP25Timeslots(m) {
	let n = 0;
	for (const c of m.contacts) {
		if (isP25(c) && c.timeslot) {
			c.timeslot = 0;
			n++;
		}
	}
	return n;
}

// --- Channels and the contacts they name ---

const members = (l) => (l && typeof l === "object" ? l.contacts.filter((x) => x && typeof x === "object") : []);

// Whether a channel is written with the flag that makes its contact and RX group list entries slots of the air mode
// contact table: its contact is a P25 contact. A channel with a DMR contact is not (an older codeplug's: the contact
// list); one with no contact follows its RX list, else the flag it was read with
export function wantsP25Contacts(c) {
	if (channelContactMode(c) !== "p25") return false;
	if (c.contact && typeof c.contact === "object") return isP25(c.contact);
	const list = members(c.rxList);
	if (list.length) return isP25(list[0]);
	return !!c.params?.p25Contacts;
}

// The channels that would be written with the flag
export const flaggedChannels = (m) => m.channels.filter((c) => c.kind && c.params && wantsP25Contacts(c));

// The mode of an RX group list's contacts: "p25", "dmr", "mixed", or null for a list with none stored
export function listMode(l) {
	const modes = new Set(members(l).map((x) => contactModeOf(x)));
	if (modes.size > 1) return "mixed";
	return modes.size ? [...modes][0] : null;
}

// The channels' contact flag as the model says it, before the modes are written: every channel of a kind that reads P25
// contacts gets params.p25Contacts as wantsP25Contacts says
export function syncContactFlags(m) {
	for (const c of m.channels) {
		if (!c.kind || !c.params || channelContactMode(c) !== "p25") continue;
		const want = wantsP25Contacts(c);
		if (!!c.params.p25Contacts !== want) c.params = { ...c.params, p25Contacts: want };
	}
}

// Problems of the links between channels, RX group lists and contacts, in validate()'s terms
export function validateContactLinks(m, err, warn, features = null) {
	const p25Lists = new Set(flaggedChannels(m).map((c) => c.rxList));
	for (const l of m.rxLists) if (p25Lists.has(l) || l.contacts.some(isP25)) {
		for (const c of l.contacts) if (typeof c === "number") err("rxLists", l, "contacts", "dangling", { number: c });
	}
	for (const c of m.contacts.filter(scopedContact)) {
		if (typeof c.system !== "object" || !m.systems.includes(c.system) || c.system.type !== 3 || systemIdentityCheck(c.system) == null)
			err("contacts", c, "system", "p25_system_unresolved");
	}
	if (features != null && !hasFeature(features, "P25_SYSTEM_CONTACTS") && m.contacts.some(scopedContact))
		warn("contacts", null, "system", "p25_system_contacts_not_in_radio");
	for (const l of m.rxLists) if (listMode(l) === "mixed") err("rxLists", l, "contacts", "rx_list_mixed");
	for (const c of m.channels) {
		const reads = channelContactMode(c);
		const ct = c.contact && typeof c.contact === "object" ? c.contact : null;
		if (ct && !contactOnChannel(ct, c)) err("channels", c, "contact", "p25_contact_wrong_system");
		if (members(c.rxList).some((x) => !contactOnChannel(x, c))) err("channels", c, "rxList", "p25_contact_wrong_system");
		if (ct && reads !== "p25" && isP25(ct)) err("channels", c, "contact", "p25_contact_on_channel");
		if (ct && reads === "p25" && isDmr(ct)) warn("channels", c, "contact", "dmr_contact_on_p25");
		const mode = listMode(c.rxList);
		if (mode == null || mode === "mixed") continue;
		const flagged = reads === "p25" && wantsP25Contacts(c);
		if (flagged && mode === "dmr") err("channels", c, "rxList", "rx_list_dmr_on_p25");
		else if (!flagged && mode === "p25") err("channels", c, "rxList", "rx_list_p25_on_channel");
	}
}
