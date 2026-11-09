// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The backup before the install (origbackup.js) against fake.js's original firmware and bootloader: the allowlist,
// the order (the internal flash mirror before program mode), the SPI reads and their limits, the restart, the bundle,
// and a write after a bootloader backup. Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { Guard, BackupRefused, readOriginal, deviceKind, spiSize, spiSeconds } from "../static/js/flasher/origbackup.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { makePlan, KEEP } from "../static/js/flasher/plan.js";
import { makeBundle, openBundle } from "../static/js/flasher/backup.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const fb = JSON.parse(readFileSync(join(DIR, "flash-boards.json"), "utf8"));
// a board that declares the backup, whichever it is (board.toml [flash.original_backup])
const board0 = Object.values(fb.boards).filter((b) => b.original_backup).sort((a, b) => a.id.localeCompare(b.id))[0];
const noWait = { wait: async () => {} };
const v = (...b) => new Uint8Array(b);

async function radio(opts = {}) {
	const sim = await simulation(fb, board0.id, 7);
	return { sim, r: new FakeRadio(sim, { spiSize: 0x1000000, ...opts }) };
}

// a transport that records and answers nothing: what the guard lets through
function recorder() {
	const sent = [];
	return { sent, out: async (...a) => { sent.push(["out", ...a]); return 0; }, in: async (...a) => { sent.push(["in", ...a]); return new Uint8Array(6); } };
}

test("a board declares it", () => {
	assert.ok(board0, "no board declares [flash.original_backup]");
	assert.ok(board0.original_version, "the backup tells the devices apart by original_version.interface");
});

test("the allowlist: only the backup's requests reach the device", async () => {
	for (const original of [true, false]) {
		const t = recorder(), g = new Guard(t, { original });
		const refused = [
			[1, 0, v(0x41, 0, 0xC0, 0, 8)],		// erase
			[1, 2, new Uint8Array(1024)],		// write
			[1, 0, v()],				// zero-length DNLOAD (manifest)
			[1, 0, v(0x91, 0x02)], [1, 0, v(0x91, 0x03)], [1, 0, v(0x91, 0x04)], [1, 0, v(0x91, 0x31)],
			[1, 0, v(0x91, 0x32)], [1, 0, v(0x91, 0x50)], [1, 0, v(0xB2, ...new Array(32).fill(0))],
			[1, 0, v(0xB4, 1)], [1, 0, v(0xB5, 0, 0, 0, 0, 0, 0, 0)], [1, 0, v(0xC4, 0, 0, 0, 0, 0, 0, 0, 0, 0)],
			[1, 0, v(0xD5, 0)], [1, 0, v(0xA2)], [1, 0, v(0xA2, 1, 2)], [1, 0, v(0x21, 0, 0, 0)], [0, 0, v()],
			[6, 0, v(1)], [4, 1, v()],
		];
		if (!original) refused.push([1, 0, v(0x91, 0x05)]);
		for (const [req, value, data] of refused) await assert.rejects(g.out(req, value, data), BackupRefused, `${req} ${value} ${Array.from(data)}`);
		await assert.rejects(g.in(2, 1024, 1), BackupRefused);
		await assert.rejects(g.in(3, 7), BackupRefused);
		await assert.rejects(g.in(5, 1), BackupRefused);
		assert.deepEqual(t.sent, []);
		// the mirror takes up to 65535 bytes, but only before program mode; then 1024 per block
		await g.in(2, 65535, 2);
		await g.out(1, 0, v(0x91, 0x01));
		await assert.rejects(g.in(2, 2048, 2), BackupRefused);
		await g.in(2, 1024, 3);
		await g.out(1, 0, v(0xA2, 0x07));
		await g.out(1, 0, v(0x21, 0, 0, 0, 0));
		await g.out(6, 0);
		await g.out(4, 0);
		if (original) await g.out(1, 0, v(0x91, 0x05));
		assert.equal(t.sent.length, original ? 8 : 7);
	}
});

test("which device: the strings tell the original firmware from the bootloader", async () => {
	const { r } = await radio({ spiSize: 0x200000 });
	assert.equal(deviceKind(board0, await r.dfuTransport().fingerprint()), "original");
	r.enterBootloader();
	assert.equal(deviceKind(board0, await r.dfuTransport().fingerprint()), "bootloader");
	assert.equal(deviceKind(board0, { interfaces: ["@Internal Flash  /0x08000000/03*016Kg,01*016Kg"] }), null);
	assert.equal(deviceKind(board0, null), null);
});

test("the original firmware: info blocks, the mirror before program mode, all of the SPI flash, then the restart", async () => {
	const { r } = await radio();
	const spec = board0.original_backup;
	const steps = [];
	const res = await readOriginal(r.dfuTransport(), board0, "original", { spi: "full", progress: (w) => steps.push(w) });
	assert.deepEqual(r.forbidden, []);
	assert.equal(res.version, "06.09");
	assert.match(res.uid, /^[0-9a-f]{24}$/);
	const files = res.parts.map((p) => p.file);
	for (const c of spec.info) assert.ok(files.includes(`info/info-${c}.bin`), c);
	const mirror = res.parts.find((p) => p.what === "internal-flash-mirror");
	assert.equal(mirror.file, "bootloader-65535.bin");
	assert.deepEqual(mirror.data, r.flash.subarray(0, 65535));
	const spi = res.parts.find((p) => p.what === "spi-flash");
	assert.equal(spi.file, "spi-flash.bin");
	assert.equal(spi.complete, true);
	assert.deepEqual(spi.data, r.spi);
	assert.equal(res.restarted, true);
	assert.equal(r.appMode, 0);
	assert.ok(r.events.includes("restart (91 05)"));
	assert.equal(res.programMode, false);
	// the mirror came before 91 01
	const i9101 = r.dfuRequests.findIndex((q) => q[0] === "out" && q[3]?.[0] === 0x91 && q[3]?.[1] === 0x01);
	const iMirror = r.dfuRequests.findIndex((q) => q[0] === "in" && q[1] === 2 && q[3] === 2);
	assert.ok(iMirror >= 0 && iMirror < i9101);
	assert.ok(steps.includes("spi"));
});

test("the quick read and a host that caps a control transfer at 4 KB", async () => {
	const { r } = await radio({ usbCap: 4096 });
	const logs = [];
	const res = await readOriginal(r.dfuTransport(), board0, "original", { spi: "quick", log: (m) => logs.push(m) });
	assert.deepEqual(r.forbidden, []);
	const mirror = res.parts.find((p) => p.what === "internal-flash-mirror");
	assert.equal(mirror.data.length, 4096);
	assert.ok(logs.some((l) => /internal flash read of 65535/.test(l)));
	const spi = res.parts.find((p) => p.what === "spi-flash");
	assert.equal(spi.data.length, board0.original_backup.quick);
	assert.equal(spi.complete, false);
	assert.equal(spi.file, `spi-flash-0-${board0.original_backup.quick / 0x100000}MB.bin`);
	assert.deepEqual(spi.data, r.spi.subarray(0, board0.original_backup.quick));
});

test("no SPI read: no program mode, no restart", async () => {
	const { r } = await radio({ spiSize: 0x200000 });
	const res = await readOriginal(r.dfuTransport(), board0, "original", { spi: "none" });
	assert.ok(!res.parts.some((p) => p.what === "spi-flash"));
	assert.equal(res.restarted, false);
	assert.equal(r.appMode, 0);
	assert.ok(!r.dfuRequests.some((q) => q[0] === "out" && q[3]?.[0] === 0x91));
});

test("the bootloader: its first megabyte only, and a write in the same session still works", async () => {
	const { sim, r } = await radio();
	r.enterBootloader();
	const res = await readOriginal(r.dfuTransport(), board0, "bootloader", { spi: "full" });
	assert.equal(res.version, null);
	assert.equal(res.loaderVersion, "021006");
	assert.equal(res.restarted, false);
	assert.equal(res.programMode, true);
	const spi = res.parts.find((p) => p.what === "spi-flash");
	assert.equal(spi.data.length, board0.original_backup.loader_spi);
	assert.equal(spi.file, "spi-flash-0-1MB.bin");
	assert.deepEqual(spi.data, r.spi.subarray(0, board0.original_backup.loader_spi));
	assert.equal(res.parts.find((p) => p.what === "info-01").data.every((x) => x === 0xFF), true);
	assert.equal(r.loaderMode, 0x30);
	// the bootloader takes 91 01 once: a write that sends it again stalls; one that starts at 91 31 erases and writes
	const image = new Uint8Array(4096).fill(0x5A);
	const plan = makePlan(sim.board, [[sim.board.app_base, image]], KEEP);
	await assert.rejects(new Dfu(r.dfuTransport(), noWait).run(plan, sim.key));
	r.state = 2;
	await new Dfu(r.dfuTransport(), noWait).run(plan, sim.key, () => {}, { programMode: true });
	assert.ok(r.erased.includes(sim.board.app_base));
	assert.ok(r.exited);
	assert.deepEqual(r.violations, []);
});

test("the bootloader is never sent 91 05, and asking it for more than 1 MB gets 1 MB", async () => {
	const { r } = await radio();
	r.enterBootloader();
	const spec = { ...board0.original_backup, quick: 0x200000 };
	const res = await readOriginal(r.dfuTransport(), { ...board0, original_backup: spec }, "bootloader", { spi: "quick" });
	assert.equal(res.parts.find((p) => p.what === "spi-flash").data.length, 0x100000);
	assert.equal(spiSize(spec, "bootloader", "full"), spec.loader_spi);
	assert.equal(spiSize(spec, "original", "none"), 0);
	assert.ok(spiSeconds(0x1000000) > 300 && spiSeconds(0x1000000) < 400);
});

test("the bundle opens as a backup with its SPI flash (what the Codeplug Editor imports)", async () => {
	const { r } = await radio({ spiSize: 0x200000 });
	const res = await readOriginal(r.dfuTransport(), board0, "original", { spi: "quick" });
	const meta = { radio: { board: board0.id, name: board0.name, model: board0.name, uid: res.uid }, firmware_now: { kind: "stock", version: res.version },
		firmware_before: { kind: "stock", version: res.version }, original_backup: { device: "original" } };
	const bundle = await openBundle(await makeBundle(meta, res.parts));
	assert.deepEqual(bundle.parts.get("spi-flash").data, r.spi.subarray(0, board0.original_backup.quick));
	assert.ok(bundle.parts.get("internal-flash-mirror"));
	assert.equal(bundle.parts.get("info-02").data.length, 4);
	assert.equal(bundle.meta.firmware_now.version, "06.09");
});
