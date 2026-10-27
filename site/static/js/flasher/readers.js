// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Readers: the ways the page reads a radio's memories in bulk for a backup (backup.js readRadio). A backup names the
// memories it wants ("internal": the MCU flash, "spi": the external SPI flash); readRadio tries the readers it is
// given in order and uses the first that opens, the debug link last. A new read-back protocol (a loader, a stock
// firmware's own PC protocol) is a new reader; the backup steps and the bundle format stay as they are.
//
//   id                         a short name, for logs
//   open(progress) -> bool     take the radio. false: not possible now, nothing changed (the next reader is tried);
//                              a throw: it failed after changing something (abort() was or will be called)
//   used                       true once open() changed the radio's state (a hand-over): close() or abort() owed
//   read(mem, addr, n, {progress, signal}) -> {data, complete}   addr in the memory's own addresses (the MCU flash
//                              from 0x08000000, the SPI flash from 0); complete false when the radio stopped early
//   close()                    give the radio back (restart it into RadioFoundry, end the session)
//   abort()                    the same after a failure, best effort

import { F4Stub, f4Image, MEM_INTERNAL, MEM_SPI } from "./f4stub.js";

const CPS_SPI = 1, CPS_MCU = 5, MCU_BASE = 0x08000000;

// RadioFoundry's open-firmware CPS over the debug link (open-firmware-cps.md 3): every RadioFoundry build
export function debugLinkReader(link) {
	return {
		id: "debuglink",
		used: false,
		open: async () => true,
		read: (mem, addr, n, { progress, signal } = {}) => (mem === "internal"
			? link.cpsDump(CPS_MCU, addr - MCU_BASE, n, { progress, signal })
			: link.cpsDump(CPS_SPI, addr, n, { progress, signal })),
		close: () => link.cpsClose(),
		abort: async () => {},
	};
}

// The STM32F4 loader (f4stub.js, docs/spec/stub-stm32f4.md), on a radio whose RadioFoundry offers the hand-over: both memories
// at the loader's speed, every 4 KB block checked against the radio's CRC, then a restart into RadioFoundry. A loader that did
// not start leaves RadioFoundry as it was (open gives false). stubJson: async () => data/f4-stub.json; reopen: the port after
// a fresh enumeration; onHandOver: the debug link's port now belongs to the loader; board: the radio's f4stub entry of
// data/flash-boards.json, whose record the loader needs before it reaches the SPI flash. A loader that refuses the
// record is restarted into RadioFoundry before the error goes up.
export function f4LoaderReader(link, { stubJson, reopen, board, log = () => {}, onHandOver = () => {} }) {
	let stub = null, holding = false;
	return {
		id: "f4-loader",
		get used() { return !!stub?.jumped; },
		open: async (progress = () => {}) => {
			try {
				const json = await stubJson();
				stub = new F4Stub(link.s, { log, reopen, board });
				await stub.enterFromRFoundry(f4Image(json), progress, { entry: json.run });
			} catch (e) {
				if (!stub?.jumped) {
					log(`the loader did not start (${e.message}); reading over the debug link`);
					return false;
				}
				onHandOver();
				throw e;
			}
			onHandOver();
			try {
				await stub.configure();
			} catch (e) {
				try { await stub.resetChip(); } catch (e2) { /* the radio needs a power cycle */ }
				try { await stub.s.close?.(); } catch (e2) { /* gone */ }
				throw e;
			}
			holding = true;
			return true;
		},
		read: async (mem, addr, n, { progress } = {}) =>
			({ data: await stub.readFlash(addr, n, { mem: mem === "spi" ? MEM_SPI : MEM_INTERNAL, progress }), complete: true }),
		// the SPI flash's security registers through the stub (SREAD), CRC-checked; null where the stub can't
		security: () => stub.sregReadAll(),
		close: async () => {
			holding = false;
			await stub.resetChip();
			try { await stub.s.close?.(); } catch (e) { /* gone */ }
		},
		abort: async () => {
			if (!holding) return;
			holding = false;
			try { await stub.resetChip(); } catch (e) { /* the radio needs a power cycle */ }
			try { await stub.s.close?.(); } catch (e) { /* gone */ }
		},
	};
}
