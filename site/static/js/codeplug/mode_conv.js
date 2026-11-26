// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Conventional system module of the Codeplug Editor (docs/spec/channel-modes.md 7.5): system type 6, shown as a
// network. It has no kind and no block: it is what a plain channel points at so the radio knows which contact lists name
// its calls (docs/spec/contact-lists.md). The record is the header only: identity length 0, flags 0, no block. Which
// lists it selects is the key 0x1F00 + n (contactlists.js), not a field of the record, so the module has no fields.

import { grow } from "./modebytes.js";

export const CONV_TYPE = 6;

export const convSystem = {
	id: CONV_TYPE,
	idLen: 0,
	conventional: true,
	defaultTx: false,
	defaultSharing: 0,	// not shared (channel-modes.md 7.5); SHARE.off, which channelmodes.js can't be imported for here
	classes: [],
	newFields: () => ({}),
	decode: () => ({}),
	encode: (_, prev) => grow(prev, 0),
	fields: () => [],
	identity: () => "",
	ownedKeys: () => [],
	validate() {},
};
