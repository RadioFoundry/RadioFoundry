// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Conventional system module of the Codeplug Editor (docs/spec/channel-modes.md 7.5): system type 6, shown as a
// network. It has no kind and no block: it is what a plain channel points at so the radio knows which contact lists name
// its calls (docs/spec/contact-lists.md). A stub for now: the editor's contact list model and pages come with it.

import { grow } from "./modebytes.js";

export const CONV_TYPE = 6;

export const convSystem = {
	id: CONV_TYPE,
	idLen: 0,
	conventional: true,
	defaultTx: false,
	classes: [],
	newFields: () => ({}),
	decode: () => ({}),
	encode: (_, prev) => grow(prev, 0),
	fields: () => [],
	identity: () => "",
	ownedKeys: () => [],
	validate() {},
};
