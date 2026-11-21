// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AM mode module of the Codeplug Editor: kind 5 on the channel-mode layer (channelmodes.js), an AM receiver on
// any frequency the radio's receiver tunes, not only the air band. The channel keeps its own frequencies in the safe
// stand-in record and carries nothing else: no system, no talk target, an empty parameter block. Radios only listen
// in AM, so the kind is receive only, always (rxOnly: the editor never lets the tag allow transmit).

import { grow } from "./modebytes.js";
import { BCAST_HZ } from "./model.js";

export const amKind = {
	id: 5,
	systemType: null,
	contacts: false,
	recordFields: [],
	ownFreqs: true,
	defaultTx: false,
	// the tag never allows transmit: the form's Receive only box stays ticked and fixed
	rxOnly: true,
	fallback: false,
	flags: {},
	newParams: () => ({}),
	decode: () => ({}),
	// nothing to write, but a later version's bytes stay
	encode: (_, prev) => grow(prev, 0),
	fields: () => [],
	validate(c, p, problem) {
		if (c.tagTx) problem.err("rxOnly", "am_rx_only");
		// the broadcast FM band is a separate FM tuner on the radios that have it: nothing there demodulates AM
		if (c.rxHz >= BCAST_HZ[0] && c.rxHz < BCAST_HZ[1]) problem.warn("rxHz", "am_bcast");
	},
};
