// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The variant probes of the AnyTone radios (detect.js): the bootloader's update mode answers the identify (02) with a
// model name that differs by MCU (anytone.js variantOf). No DOM here.

import { registerProbe } from "./detect.js";
import { AnytoneUpdate, ACK, BAUD, HELLO, HELLO_REPLY, variantOf } from "./anytone.js";

// The order the AnyTone backend tries them. The original firmware's USB ids name the MCU (board.toml usb_ids) with
// nothing sent. Its PC mode can't: both builds of a model answer the identify with the same name, and leaving PC mode
// (END) restarts the radio, so the page never enters it to detect. Then the update mode's UPDATE: the running original
// firmware ignores the word, as RadioFoundry's debug link does. RadioFoundry's frames (A5 5A) come last, once no
// bootloader answered (docs/spec/hardware-d578uv.md 1, 8)
export const PROBES = ["usb-id", "anytone-update", "rfoundry"];

const PROBE_TIMEOUT = 800;	// ms: a radio that answers this protocol does so at once

// The bootloader's update mode: UPDATE answered with the ACK alone, then the identify. The session stays open (keep),
// so the install goes on from it without a second connect
registerProbe({
	id: "anytone-update",
	baud: BAUD,
	async run(stream, { board, log }) {
		const u = new AnytoneUpdate(stream, { log, timeout: PROBE_TIMEOUT });
		stream.clear();
		await stream.write(HELLO);
		let r = await u.readExact(1);
		if (r.length === 1 && r[0] === HELLO_REPLY[0]) r = Uint8Array.of(r[0], ...await u.readExact(HELLO_REPLY.length - 1));
		if (!(r.length === 1 && r[0] === ACK)) return null;	// silence, or the original firmware's own UPDATE answer
		const ident = await u.identify();
		return { code: variantOf(board, ident.model), version: ident.model, ident, via: "update-mode", keep: true, radio: u };
	},
	// Before the first frame: the identify again on the open session, nothing else sent
	async confirm(radio, { board }) {
		const ident = await radio.identify();
		return { code: variantOf(board, ident.model), version: ident.model };
	},
});
