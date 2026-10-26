// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher's modules, checked against rfoundry (fixtures from site/tests/fixtures.py) and run end to end
// against the simulated radio (fake.js). Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, sha256, crc32, u32le } from "../static/js/flasher/bytes.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import { makePlan, KEEP, MERGE, RESTORE, PlanError } from "../static/js/flasher/plan.js";
import { verify, segments, ManifestError } from "../static/js/flasher/manifest.js";
import { Dfu } from "../static/js/flasher/dfu.js";
import { Link, crc16, frame, parseFrames, probe } from "../static/js/flasher/link.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";
import { zip, unzip } from "../static/js/flasher/zip.js";
import * as vendorfile from "../static/js/flasher/vendorfile.js";
import * as explain from "../static/js/flasher/explain.js";
import { pictureSrc } from "../static/js/flasher/radiotop.js";
import { readRadio, makeBundle, openBundle, bundleName } from "../static/js/flasher/backup.js";
import { targets, planInternal, planStock, codeplugSectors, writeCodeplug } from "../static/js/flasher/restore.js";
import { Selection } from "../static/js/flasher/select.js";
import { decideCodec, DecisionError } from "../static/js/flasher/decide.js";
import { findKey, readBootKey } from "../static/js/flasher/bootkey.js";
import { codecSegments, readCodec } from "../static/js/flasher/codecsave.js";
import { detect, convertersFor } from "../static/js/flasher/codeplug.js";
import * as crumbs from "../static/js/flasher/trail.js";
import * as diag from "../static/js/diag.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const fx = json("fixtures.json");
const fb = json("flash-boards.json");
const stock = bin("stock.bin");
const image = bin("image.bin");
const board = fb.boards.dm1701;
const profiles = { [fx.stock_sha256]: fx.profile };
const noWait = { wait: async () => {} };

test("CRC-16/CCITT-FALSE check value and frames", () => {
	assert.equal(crc16(new TextEncoder().encode("123456789")), 0x29B1);
	const f = frame(0x02, 0x41, new Uint8Array([1, 2, 3]));
	const [p] = parseFrames(f);
	assert.equal(p.cmd, 0x02);
	assert.deepEqual([...p.payload], [1, 2, 3]);
});

test("crc32 check value", () => {
	assert.equal(crc32(new TextEncoder().encode("123456789")), 0xCBF43926);
});

test("key derivation matches rfoundry", async () => {
	const key = await stockfw.deriveKey(stock, await sha256(fromHex(fx.key)), fb);
	assert.equal(toHex(key), fx.key);
	assert.equal(toHex(key), fx.derived_key);
});

// The MD-380 image is far less zero-dominated: the column vote misses hundreds of columns and only the byte-pair
// refinement finds the key (issue 119). Synthetic file from fixtures.py, the same key as rfoundry gives
test("key derivation refines a bad vote (MD-380-like file)", async () => {
	const m = fx.md380like;
	assert.ok(m.vote_wrong > 200, `the vote alone should be far off (${m.vote_wrong} columns)`);
	const key = await stockfw.deriveKey(bin("stock-md380like.bin"), await sha256(fromHex(m.key)), fb);
	assert.equal(toHex(key), m.key);
	assert.equal(toHex(key), m.derived_key);
});

// Opt-in: a real TYT updater file (never in the repository), e.g. RFOUNDRY_MD380_STOCK="MD-380-D2.32(AD).bin" site/tests/run.sh
test("key derivation from a real MD-380 updater file", { skip: !process.env.RFOUNDRY_MD380_STOCK && "set RFOUNDRY_MD380_STOCK" }, async () => {
	const key = await stockfw.deriveKey(new Uint8Array(readFileSync(process.env.RFOUNDRY_MD380_STOCK)), fb.boards.md380.key_sha256, fb);
	assert.equal(await sha256(key), fb.boards.md380.key_sha256);
});

// A read of the internal flash of a radio on its stock firmware gives the codec without a key, placed as rfoundry places
// it (fixtures.py flash_readout: a synthetic MD-UV380 V19.26 build); a changed byte or a blank flash gives nothing
test("the codec from a flash readout matches rfoundry", async () => {
	const r = fx.readout;
	const rprofiles = { [r.stock_sha256]: r.profile };
	const flash = bin("flash-mduv380.bin");
	const got = await stockfw.fromFlash(flash, fb, rprofiles);
	assert.equal(got.profile, r.profile);
	assert.deepEqual(await Promise.all(got.segments.map(async ([a, d]) => [a, await sha256(d), d.length])), r.placed);
	const fromStock = await stockfw.extract(bin("stock-mduv380.bin"), fromHex(r.key), fb, rprofiles);
	assert.deepEqual(fromStock.segments, got.segments);
	const bad = flash.slice();
	bad[r.profile.windows[0][0] - 0x08000000 + 100] ^= 1;
	assert.equal(await stockfw.fromFlash(bad, fb, rprofiles), null);
	assert.equal(await stockfw.fromFlash(new Uint8Array(0x100000).fill(0xFF), fb), null);
	// picked on the page, a readout is named but not taken: the page asks for the manufacturer's file
	const withR = { ...fb, profiles: rprofiles };
	const opened = await vendorfile.openVendorFile("dump.bin", flash, withR, r.profile.board);
	assert.equal(opened.best.match.kind, "readout");
	for (const need of ["codec", "key"]) assert.equal(explain.pickProblem(opened, r.profile.board, { need }).problem, "not-vendor", need);
	assert.match(vendorfile.describe(opened, withR, r.profile.board)[0], /manufacturer's firmware file instead/);
});

// A build whose code doesn't fit after its tables (RT3S V219.60): the code at the start of sector 10, below the
// tables, the same from the stock file and from a readout as rfoundry places it
test("code placed before the tables matches rfoundry", async () => {
	const r = fx.readout_v21960;
	const rprofiles = { [r.stock_sha256]: r.profile };
	assert.equal(r.profile.placement.code, 0x080C0000);
	assert.ok(r.profile.placement.code + r.profile.windows[0][1] <= r.profile.windows[1][0]);
	const got = await stockfw.fromFlash(bin("flash-rt3s-v21960.bin"), fb, rprofiles);
	assert.deepEqual(await Promise.all(got.segments.map(async ([a, d]) => [a, await sha256(d), d.length])), r.placed);
	const fromStock = await stockfw.extract(bin("stock-rt3s-v21960.bin"), fromHex(r.key), fb, rprofiles);
	assert.deepEqual(fromStock.segments, got.segments);
	assert.deepEqual(fromStock.segments.map(([a]) => a), [0x080C0000, 0x080E322C, fb.descriptor.addr]);
});

// Every board with stock files: each file's profile in the flasher data, its codec the board's, its layout inside the
// flash the board leaves to the codec and below the descriptor
test("stock-file boards: profiles and layouts", () => {
	for (const b of Object.values(fb.boards).filter((x) => Object.values(fb.profiles).some((p) => p.board === x.id))) {
		const profs = Object.values(fb.profiles).filter((p) => p.board === b.id);
		assert.deepEqual(profs.map((p) => p.name).sort(), [...b.codec.sources].sort(), b.id);
		for (const p of profs) {
			assert.equal(p.codec_profile, b.codec.id, p.name);
			// a file for another radio (file_radio) gives the codec only, encrypted with that radio's key
			if (p.file_radio) assert.notEqual(p.key_sha256, b.key_sha256, p.name);
			else assert.equal(p.key_sha256, b.key_sha256, p.name);
			const lay = stockfw.layoutWindows(p, fb);
			for (const [a, n] of lay) assert.ok(a + n <= b.flash_end && b.codec.windows.some(([wa, wn]) => wa === a && wn === n), p.name);
			if (p.placement) assert.ok(lay.slice(0, -1).every(([a, n]) => a + n <= fb.descriptor.addr), p.name);
		}
	}
	const radio = json("radios.json").radios.find((r) => r.id === "mduv380");
	assert.ok(radio.flash.bootloader.length > 0 && radio.flash.build_options.some((o) => o.name === "POWER"));
});

// vendorfile.js: whatever the user picks, every file in it identified against every radio
const radioX = async (extra = {}) => ({ ...fb, profiles: {}, known_files: {}, c7000: null,
	boards: { x: { id: "x", name: "Radio X", key_sha256: await sha256(fromHex(fx.key)), app_base: fx.profile.segments.at(-1)[0] } }, ...extra });
const ole = () => { const d = new Uint8Array(70000); d.set([0xD0, 0xCF, 0x11, 0xE0]); return d; };

test("vendor file: a .bin, or the firmware out of a zip with updaters, documents and macOS copies", async () => {
	const fbX = await radioX({ profiles: { [fx.stock_sha256]: { ...fx.profile, board: "x" } } });
	const plain = await vendorfile.openVendorFile("x.bin", stock, fbX, "x");
	assert.equal(plain.best.data, stock);
	assert.equal(plain.best.match.kind, "codec");
	const exe = new Uint8Array(70000); exe.set([0x4D, 0x5A]);
	const z = await zip([
		{ name: "Firmware 2.32/Upgrade.exe", data: exe },
		{ name: "Firmware 2.32/Operation.doc", data: ole() },
		{ name: "Firmware 2.32/readme.txt", data: "x".repeat(70000) },
		{ name: "Firmware 2.32/MD-380-D2.32(AD).bin", data: stock },
		{ name: "__MACOSX/Firmware 2.32/._MD-380-D2.32(AD).bin", data: new Uint8Array(212) },
	]);
	const got = await vendorfile.openVendorFile("TYT-Tytera-MD-380-FW-v232.zip", z, fbX, "x");
	assert.equal(got.best.path, "Firmware 2.32/MD-380-D2.32(AD).bin");
	assert.equal(got.files.length, 1);
	assert.deepEqual(got.skipped.map((s) => s.why), ["a Windows program", "an Office document", "a text file"]);
	const lines = vendorfile.describe(got, fbX, "x");
	assert.ok(lines[0].endsWith("(used)") && lines.some((l) => /Upgrade\.exe: skipped, a Windows program/.test(l)), lines.join("\n"));
	// a zip in a zip, as some dealers repack the vendor's
	const nested = await vendorfile.openVendorFile("dealer.zip", await zip([{ name: "inner/fw.zip", data: z }]), fbX, "x");
	assert.equal(nested.best.data.length, stock.length);
});

test("vendor file: several builds of one radio, the one with the codec first", async () => {
	// TYT's MD-UV380 zips hold a CSV and a REC build
	const other = stock.slice();
	other[other.length - 1] ^= 0xFF;
	const two = await zip([{ name: "B-REC.bin", data: other }, { name: "A-CSV.bin", data: stock }]);
	const both = await radioX({ profiles: { [fx.stock_sha256]: { ...fx.profile, board: "x" }, [await sha256(other)]: { ...fx.profile, board: "x" } } });
	assert.equal((await vendorfile.openVendorFile("fw.zip", two, both, "x")).best.path, "A-CSV.bin");
	// only B mapped: B, whatever its name (A still gives the key)
	const onlyB = await radioX({ profiles: { [await sha256(other)]: { ...fx.profile, board: "x" } } });
	const got = await vendorfile.openVendorFile("fw.zip", two, onlyB, "x");
	assert.equal(got.best.path, "B-REC.bin");
	assert.equal(got.files.find((f) => f.path === "A-CSV.bin").match.kind, "codec-unmapped");
});

test("vendor file: an unknown build is told apart by the bootloader key, and its codec found without a recipe", async () => {
	const fbX = await radioX();
	const got = await vendorfile.openVendorFile("new.bin", stock, fbX, "x");
	const m = got.best.match;
	assert.equal(m.board, "x");
	assert.equal(m.kind, "codec-unmapped");
	assert.equal(toHex(m.key), fx.key);
	assert.ok(m.codecAt >= fbX.boards.x.app_base);
	assert.match(vendorfile.describe(got, fbX, "x")[0], /no recipe for this build yet \(the bootloader key works\) \(used\)/);
	// the same file for another radio: named, not used
	const fbY = { ...fbX, boards: { ...fbX.boards, y: { id: "y", name: "Radio Y" } } };
	const y = await vendorfile.openVendorFile("new.bin", stock, fbY, "y");
	assert.equal(y.best, null);
	assert.match(vendorfile.describe(y, fbY, "y")[0], /for the Radio X, not this radio/);
	// a container no known key opens
	const z = await vendorfile.openVendorFile("new.bin", stock, { ...fbX, boards: {} }, "x");
	assert.equal(z.best, null);
	assert.match(z.files[0].match.why, /bootloader key RadioFoundry doesn't know/);
});

test("vendor file: only a vendor container the page can't use is offered for analysis", async () => {
	const fbX = await radioX();
	// the codec of a build without a map: offered, with what was found
	const unmapped = vendorfile.analysisCandidate(await vendorfile.openVendorFile("new.bin", stock, fbX, "x"), fbX);
	assert.equal(unmapped.reason, "codec-unmapped");
	// a container no known key opens
	const nokey = { ...fbX, boards: {} };
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("new.bin", stock, nokey, "x"), nokey).reason, "key-unknown");
	// a mapped build: nothing to send
	const mapped = await radioX({ profiles: { [fx.stock_sha256]: { ...fx.profile, board: "x" } } });
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("x.bin", stock, mapped, "x"), mapped), null);
	// not a vendor container (a readout, anything else), or too large: never offered
	const other = new Uint8Array(100000).fill(0x5A);
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("dump.bin", other, fbX, "x"), fbX), null);
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("new.bin", stock, fbX, "x"), fbX, "x", stock.length - 1), null);
});

// OPENDIGITALRADIO-J: on the MD-380's page, with the MD-380's key, an MD-UV380 file matched its profile by SHA-256 and
// failed only when decrypted, as "decrypted code window does not match (wrong key?)". Now refused by name, before
// any decryption, whatever key is passed
test("a file for another radio is refused by name, never blamed on the key", async () => {
	const other = Object.keys(fb.boards).find((b) => b !== fx.profile.board && fb.boards[b].key_sha256);
	const err = await stockfw.extract(stock, new Uint8Array(fb.key_len), fb, profiles, other).catch((e) => e);
	assert.ok(err instanceof stockfw.StockError);
	assert.equal(err.code, "other-radio");
	assert.equal(err.params.board, fx.profile.board);
	assert.match(err.message, new RegExp(`for the ${fb.boards[fx.profile.board].name}, not the ${fb.boards[other].name}`));
	assert.doesNotMatch(err.message, /key/);
	assert.deepEqual(explain.errorProblem(err), { problem: "other-radio", board: fx.profile.board, file: fx.profile.name, known: null, raw: err.message });
	// the radio's own file goes through
	assert.equal((await stockfw.extract(stock, fromHex(fx.key), fb, profiles, fx.profile.board)).segments.length, 3);
});

// The same with the real files: RFOUNDRY_MD380_STOCK (any MD-380 file, for its key) and RFOUNDRY_MDUV380_STOCK (an
// MD-UV380 19.26 file)
test("OPENDIGITALRADIO-J with the real files", { skip: !(process.env.RFOUNDRY_MD380_STOCK && process.env.RFOUNDRY_MDUV380_STOCK) && "set RFOUNDRY_MD380_STOCK and RFOUNDRY_MDUV380_STOCK" }, async () => {
	const md380 = await vendorfile.openVendorFile("md380.bin", new Uint8Array(readFileSync(process.env.RFOUNDRY_MD380_STOCK)), fb, "md380");
	const key = md380.best.match.key || await stockfw.deriveKey(md380.best.data, fb.boards.md380.key_sha256, fb);
	const uv = new Uint8Array(readFileSync(process.env.RFOUNDRY_MDUV380_STOCK));
	await assert.rejects(stockfw.extract(uv, key, fb), /window does not match/);	// as the live page failed
	await assert.rejects(stockfw.extract(uv, key, fb, fb.profiles, "md380"), (e) => e.code === "other-radio" && /MD-UV380/.test(e.message));
	// and the page never gets there: the opener names the file's radio and uses nothing
	const opened = await vendorfile.openVendorFile("uv.bin", uv, fb, "md380");
	assert.equal(opened.best, null);
	assert.equal(explain.pickProblem(opened, "md380").problem, "other-radio");
	assert.equal(vendorfile.analysisCandidate(opened, fb, "md380"), null);
});

// explain.js: what the user picked sorted into the problem the page explains
test("a file that can't be used is explained: another radio, not a vendor file, damaged, unmapped, unknown key", async () => {
	const fbX = await radioX();
	const fbY = { ...fbX, boards: { ...fbX.boards, y: { id: "y", name: "Radio Y" } } };
	const pick = async (name, data, f = fbX, board = "x", opts) => explain.pickProblem(await vendorfile.openVendorFile(name, data, f, board), board, opts);
	// a build of this radio without a codec map: the codec can't be taken, but a restore of the original firmware can use it
	assert.deepEqual(await pick("new.bin", stock), { problem: "unmapped", file: "new.bin", known: null, key: true });
	assert.equal(await pick("new.bin", stock, fbX, "x", { need: "key" }), null);
	// a mapped build: nothing wrong
	assert.equal(await pick("x.bin", stock, await radioX({ profiles: { [fx.stock_sha256]: { ...fx.profile, board: "x" } } })), null);
	// the same file on another radio's page, for either need
	assert.deepEqual(await pick("new.bin", stock, fbY, "y"), { problem: "other-radio", file: "new.bin", board: "x", known: null });
	assert.equal((await pick("new.bin", stock, fbY, "y", { need: "key" })).problem, "other-radio");
	// a vendor container no known key opens: another model, or a modified build
	assert.equal((await pick("new.bin", stock, { ...fbX, boards: {} })).problem, "wrong-key");
	// another firmware project's image, a plain readout without a known codec, or a zip of documents
	assert.equal((await pick("other-project.bin", new Uint8Array(100000).fill(0x5A))).problem, "not-vendor");
	assert.deepEqual(await pick("docs.zip", await zip([{ name: "manual.doc", data: ole() }])), { problem: "not-vendor", file: "manual.doc" });
	// cut short (an interrupted download) or a damaged zip
	assert.equal((await pick("cut.bin", stock.slice(0, stock.length - 5000))).problem, "damaged");
	const z = await zip([{ name: "fw.bin", data: stock }]);
	const bad = z.slice();
	bad[60] ^= 0xFF;
	assert.equal((await pick("fw.zip", bad)).problem, "damaged");
	// the send offer: never for another radio's file, which the page names instead
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("new.bin", stock, fbY, "y"), fbY, "y"), null);
	assert.equal(vendorfile.analysisCandidate(await vendorfile.openVendorFile("new.bin", stock, fbX, "x"), fbX, "x").reason, "codec-unmapped");
	// a file identified for this radio that failed later: offered only if it is a vendor container within the limit
	assert.equal(vendorfile.offerFile({ path: "a.bin", data: stock }, fbX, "internal").reason, "internal");
	assert.equal(vendorfile.offerFile({ path: "a.bin", data: new Uint8Array(100000) }, fbX, "internal"), null);
	// errors that aren't about a file stay as they are
	assert.equal(explain.errorProblem(new Error("stopped")), null);
	assert.equal(explain.errorProblem(new stockfw.StockError("x", "internal")).problem, "internal");
});

// A known build of this radio without a codec map (stockfw KNOWN_FILES only): named, its key used, the codec refused
// with the version, the file offered for analysis, and every supported file listed to pick instead
test("a known build without a codec map: the version named, the file offered, the supported files listed", async () => {
	const sources = ["Radio A V1 (a-v1.bin)", "Radio A V2 (a-v2.bin)"];
	const fbX = await radioX({ known_files: { [fx.stock_sha256]: "Radio A V0.9" } });
	fbX.boards.x.codec = { sources };
	const opened = await vendorfile.openVendorFile("a-v09.bin", stock, fbX, "x");
	assert.equal(opened.best.match.kind, "key");
	const p = explain.pickProblem(opened, "x");
	assert.deepEqual(p, { problem: "unmapped", file: "a-v09.bin", known: "Radio A V0.9", key: true });
	assert.ok(explain.OFFER.has(p.problem));
	assert.equal(vendorfile.analysisCandidate(opened, fbX, "x").file, opened.best);
	assert.equal(explain.pickProblem(opened, "x", { need: "key" }), null);
	// the full list, a copy of the board's
	const got = explain.supportedFiles(fbX, "x");
	assert.deepEqual(got, { files: sources, also: [] });
	got.files.push("z");
	assert.equal(fbX.boards.x.codec.sources.length, 2);
	// C-SKY radios: the profiles that describe a whole stock file first, then the other files the sources name; a source
	// that isn't a file (a program flash read) on its own; none for an unknown radio
	const c = { c7000: { boards: { b: {}, d: {} }, profiles: [{ name: "B 001 build", board: "b", stock_file: {} },
		{ name: "B readout", board: "b", stock_file: null }, { name: "C 001", board: "c", stock_file: {} }] },
	boards: { b: { codec: { sources: ["b-002.bin", "B 001 build", "a program flash read of the radio"] } },
		d: { codec: { sources: ["a program flash read of the radio"] } } } };
	assert.deepEqual(explain.supportedFiles(c, "b"), { files: ["B 001 build", "b-002.bin"], also: ["a program flash read of the radio"] });
	assert.deepEqual(explain.supportedFiles(c, "d"), { files: [], also: ["a program flash read of the radio"] });
	assert.deepEqual(explain.supportedFiles({ boards: {} }, "q"), { files: [], also: [] });
	// every C-SKY radio in the data lists at least one stock file, profiles first
	for (const id of Object.keys(fb.c7000?.boards || {})) {
		const { files } = explain.supportedFiles(fb, id);
		const named = (fb.c7000.profiles || []).filter((p) => p.board === id && p.stock_file).map((p) => p.name);
		assert.deepEqual(files.slice(0, named.length), named, id);
	}
	// the notice folds a long list, never a short one
	assert.equal(explain.foldSupported(Array(explain.SUPPORTED_OPEN_MAX).fill("f")), false);
	assert.equal(explain.foldSupported(Array(explain.SUPPORTED_OPEN_MAX + 1).fill("f")), true);
});

// The install goes on without the picked file's codec (decide.js): what the codec step says DMR voice depends on
test("keeping the radio's codec with a file that gives none is spelled out", () => {
	const board = { name: "Radio A", keep_codec_supported: true, debuglink_names: ["a"], codec: { id: "a-codec", installed: ["V1"] } };
	const manifest = { codec: { profile: "a-codec" } };
	const un = { problem: "unmapped", file: "a.bin", known: "Radio A V0.9", key: true };
	const keep = (opts) => explain.keepNote(un, decideCodec(board, manifest, opts));
	// nothing verified it: selected, or a version the user named
	assert.equal(keep({ requested: KEEP }), "unverified");
	assert.equal(keep({ stockInstalled: "V1" }), "unverified");
	// the radio reported its codec, or that it has none and keeping was forced
	assert.equal(keep({ probe: { board: "a", codec: "a-codec" } }), "ok");
	assert.equal(keep({ requested: KEEP, probe: { board: "a", codec: null } }), "none");
	// the original firmware, version not given: "unverified" where a stock version keeps the codec in place, "none" on
	// a radio whose stock firmware never does (board.codec.installed empty), and the decision expects no codec, so the
	// done step doesn't call the outcome a failure
	assert.equal(keep({ requested: KEEP, runsStock: true }), "unverified");
	const never = { ...board, codec: { ...board.codec, installed: [] } };
	const kept = decideCodec(never, manifest, { requested: KEEP, runsStock: true });
	assert.equal(kept.mode, KEEP);
	assert.equal(kept.expectCodec, false);
	assert.match(kept.risk, /DMR voice is not available/);
	assert.equal(explain.keepNote(un, kept), "none");
	assert.equal(decideCodec(never, manifest, { requested: KEEP, stockInstalled: "V1" }).expectCodec, false, "a named version too");
	// not known to run its original firmware, or keeping not selected: unchanged
	assert.equal(explain.keepNote(un, decideCodec(never, manifest, { requested: KEEP })), "unverified");
	assert.equal(decideCodec(never, manifest, { requested: KEEP }).expectCodec, true);
	assert.equal(decideCodec(never, manifest, { runsStock: true, haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(never, manifest, { runsStock: true }));
	// the codec comes from elsewhere, nothing was decided, or the file is not the issue
	assert.equal(explain.keepNote(un, { mode: MERGE, source: "saved", expectCodec: true }), null);
	assert.equal(explain.keepNote(un, null), null);
	assert.equal(explain.keepNote(null, decideCodec(board, manifest, { requested: KEEP })), null);
	assert.equal(explain.keepNote({ problem: "not-vendor" }, decideCodec(board, manifest, { requested: KEEP })), null);
});

// The "current firmware" step: four choices; "Another open firmware" and "Other / Not sure" are both kind "unknown" to
// the install (the codec decision, the steps), told apart only for the pressed button and the report
test("the current firmware choices: four, two of them the same to the install", () => {
	assert.deepEqual(explain.NOW_CHOICES, ["rfoundry", "stock", "openfw", "unknown"]);
	assert.equal(explain.nowChoice(null), null);
	for (const id of ["stock", "openfw", "unknown"]) assert.equal(explain.nowChoice(explain.nowFor(id)), id);
	assert.equal(explain.nowChoice({ kind: "rfoundry", probe: {} }), "rfoundry");
	assert.deepEqual(explain.nowFor("stock"), { kind: "stock", version: null });
	assert.equal(explain.nowFor("openfw").kind, "unknown");
	assert.equal(explain.nowFor("unknown").kind, "unknown");
	// the same decision either way
	const b = { name: "Radio A", keep_codec_supported: true, debuglink_names: ["a"], codec: { id: "a-codec", installed: [] } };
	const decide = (now) => decideCodec(b, { codec: { profile: "a-codec" } }, { haveStock: true, probe: null,
		stockInstalled: now.kind === "stock" ? now.version : null, runsStock: now.kind === "stock" });
	assert.deepEqual(decide(explain.nowFor("openfw")), decide(explain.nowFor("unknown")));
	// every choice has its title and help line
	const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));
	for (const id of explain.NOW_CHOICES) assert.ok(en[`flash.now.${id}`] && en[`flash.now.${id}_text`], id);
});

// The update mode steps show the radio's own picture from the data (radiotop.js), never a list of radios: every radio
// gets one from the site build (site/tests/radio_images_test.py renders each)
test("the update mode picture comes from the radio's data", () => {
	assert.equal(pictureSrc({ id: "q", frame_image: "static/img/radios/q-frame.svg" }), "static/img/radios/q-frame.svg");
	assert.equal(pictureSrc({ id: "q" }), null);
	assert.equal(pictureSrc(null), null);
	const src = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	// every bootloader step list sits beside the picture
	const lists = [...src.matchAll(/S\.radio\.flash\.bootloader\.map/g)].length;
	assert.ok(lists >= 3);
	assert.equal([...src.matchAll(/withPicture\(h\("ol", \{ class: "steps[^"]*" \}, S\.radio\.flash\.bootloader\.map/g)].length, lists);
});

// app.js needs the DOM, so what it must keep is checked in its source: whyNoAnswer() is async, so a noAnswer() that
// isn't awaited throws a Promise and the user reads "[object Promise]"; a radio that answered with the open-firmware
// CPS gets the next step, not the cable and driver tips; every string it picks exists
test("the flasher page awaits noAnswer and explains an open-firmware answer", () => {
	const src = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));
	const calls = [...src.matchAll(/(\S+)\s+noAnswer\(/g)].filter((m) => m[1] !== "function");
	assert.ok(calls.length >= 4, "noAnswer calls found");
	for (const m of calls) assert.equal(m[1], "await", `noAnswer() not awaited: ${m[0]}`);
	// the connect failures outside the current firmware step go through connectHelp, with a key that exists
	const helps = [...src.matchAll(/connectHelp\(e, "([\w.]+)"\)/g)].map((m) => m[1]);
	assert.deepEqual(helps.sort(), ["flash.backup.openfw_next", "flash.done.openfw_next", "flash.radio.openfw_next"]);
	for (const k of [...helps, "flash.err.rfoundry_nodebug_next", "flash.now.openfw_switch", "flash.now.openfw_use",
		"flash.problem.unmapped.also", "flash.problem.unmapped.only"]) assert.ok(en[k], k);
	// the done step names the build it just wrote, so RadioFoundry without the debug link is told apart
	assert.match(src, /noAnswer\(link, \{ written: S\.firmware\?\.manifest \}\)/);
});

// A file known to hold no codec (stockfw.NO_CODEC_FILES), bare or zipped with small language files: named by its
// SHA-256 with rfoundry's words, never a codec source, a key or a restore. The real file stays out of the repository:
// a stand-in registered under its own hash (fixtures.py no_codec)
test("a file known to hold no codec is named and refused as rfoundry refuses it", async () => {
	const nc = fx.no_codec;
	assert.equal(fb.no_codec_files[nc.real_sha256].board, "dm1701");
	const data = bin("nocodec.bin");
	const f = { ...fb, no_codec_files: { ...fb.no_codec_files, [nc.sha256]: fb.no_codec_files[nc.real_sha256] } };
	const m = await vendorfile.identifyFile(data, f, "dm1701");
	assert.deepEqual([m.board, m.problem, m.how, m.why], [null, "not-vendor", "sha256", nc.message]);
	await assert.rejects(stockfw.extract(data, null, f), (e) => e.message === nc.message && e.code === "not-vendor");
	const gla = new Uint8Array(4772).fill(0x41);
	const z = await zip([{ name: "fw.bin", data }, { name: "German.gla", data: gla }, { name: "French.gla", data: gla }]);
	for (const [name, d] of [["fw.bin", data], ["fw.zip", z]]) {
		const opened = await vendorfile.openVendorFile(name, d, f, "dm1701");
		assert.equal(opened.best, null, name);
		for (const need of ["codec", "key"]) assert.equal(explain.pickProblem(opened, "dm1701", { need }).problem, "not-vendor", name);
		assert.equal(vendorfile.analysisCandidate(opened, f, "dm1701"), null, name);
		assert.ok(vendorfile.describe(opened, f, "dm1701")[0].includes(nc.message), name);
	}
	// without the entry it is still refused, only not named
	assert.equal((await vendorfile.identifyFile(data, fb, "dm1701")).problem, "not-vendor");
});

test("the known file without a codec, the real one", { skip: !process.env.RFOUNDRY_NOCODEC_FILE && "set RFOUNDRY_NOCODEC_FILE (the .bin or its .zip)" }, async () => {
	const d = new Uint8Array(readFileSync(process.env.RFOUNDRY_NOCODEC_FILE));
	const opened = await vendorfile.openVendorFile("picked", d, fb, "dm1701");
	const f = opened.files.find((x) => fb.no_codec_files[x.match.sha]);
	assert.ok(f, "no file in the pick has a NO_CODEC_FILES hash");
	assert.equal(f.match.problem, "not-vendor");
	assert.equal(explain.pickProblem(opened, "dm1701").problem, "not-vendor");
});

test("an error report names the picked file and what was found, never its bytes", async () => {
	const fbX = await radioX();
	const opened = await vendorfile.openVendorFile("fw.zip", await zip([{ name: "a/new.bin", data: stock }, { name: "a/setup.exe", data: (() => { const d = new Uint8Array(70000); d.set([0x4D, 0x5A]); return d; })() }]), fbX, "x");
	const ctx = explain.vendorContext({ boardId: "x", picked: { name: "fw.zip", size: 123, sha256: "ab".repeat(32) }, opened,
		problem: { problem: "unmapped", raw: "no codec map" }, keySource: "file", decision: { mode: "merge", source: "stock" } });
	assert.equal(ctx.board, "x");
	assert.deepEqual(ctx.picked, { name: "fw.zip", size: 123, sha256: "ab".repeat(32) });
	assert.deepEqual(ctx.files, [{ path: "a/new.bin", size: stock.length, sha256: await sha256(stock), board: "x", kind: "codec-unmapped", how: "key", known: null, problem: null }]);
	assert.deepEqual(ctx.skipped, [{ path: "a/setup.exe", problem: "not-firmware" }]);
	assert.equal(ctx.used, "a/new.bin");
	assert.equal(ctx.key, "file");
	assert.deepEqual(ctx.problem, { problem: "unmapped", board: null, raw: "no codec map" });
	const text = JSON.stringify(ctx);
	assert.ok(!text.includes("bytes>") && text.length < 2000, "no file contents");
	assert.equal(explain.vendorContext({}), null);
});

// Restoring the original firmware takes any build of the radio, mapped or not: the key and the application come from
// it, not the codec. A build without a profile still writes its whole application back
test("the original firmware goes back from a build without a codec profile", async () => {
	const fbX = await radioX();
	const opened = await vendorfile.openVendorFile("new.bin", stock, fbX, "x");
	const m = opened.best.match;
	assert.equal(m.kind, "codec-unmapped");
	const xb = { ...board, id: "x", app_base: fbX.boards.x.app_base };
	const plan = planStock(opened.best.data, m.key, xb, fbX);
	const [addr, app] = stockfw.stockApplication(opened.best.data, m.key, xb, fbX);
	assert.equal(addr, xb.app_base);
	assert.ok(plan.totalBlocks > 0 && plan.totalBlocks * 1024 >= app.length);
	// the same application the profiled file gives
	const [, known] = stockfw.stockApplication(stock, fromHex(fx.key), board, fb);
	assert.deepEqual(app, known);
	// not this radio's: the key can't come from it, and the page says why before it gets there
	await assert.rejects(stockfw.deriveKey(new Uint8Array(100000).fill(0x5A), fbX.boards.x.key_sha256, fbX), (e) => e.code === "not-vendor");
});

test("codec extraction checks every window", async () => {
	const { segments: segs } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.equal(segs.length, 3);	// code (moved), tables, descriptor
	const bad = fromHex(fx.key);
	bad[5] ^= 1;
	await assert.rejects(stockfw.extract(stock, bad, fb, profiles), stockfw.StockError);
});

test("keep and merge plans match plan.py", async () => {
	const { profile, segments: codec } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.deepEqual(makePlan(board, [[board.app_base, image]], KEEP).describe(), fx.keep);
	assert.deepEqual(makePlan(board, [[board.app_base, image]], MERGE, codec, stockfw.layoutWindows(profile, fb)).describe(),
		fx.merge);
	assert.throws(() => makePlan(board, [[board.app_base, image]], MERGE), PlanError);
	assert.throws(() => makePlan(board, [[0x08074000, image]], KEEP), PlanError);
});

test("the codec is placed as stockfw.py places it: code moved, tables cut, descriptor", async () => {
	const { segments: codec } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
	assert.deepEqual(await Promise.all(codec.map(async ([a, d]) => [a, await sha256(d), d.length])), fx.placed);
	const [, , [da, desc]] = codec;
	assert.equal(da, 0x080FFF80);
	assert.equal(u32le(desc, 0), 0x43534544);
	assert.equal(u32le(desc, 124), crc32(desc.subarray(0, 124)));
	const prof = Object.values(profiles)[0];
	const noMap = { ...prof, map: null, windows: [[prof.windows[0][0], prof.windows[0][1], "0".repeat(64)], ...prof.windows.slice(1)] };
	await assert.rejects(stockfw.place(noMap, [[0, new Uint8Array(4)], [0, new Uint8Array(4)]], fb), stockfw.StockError);
});

test("a manifest with a second range (an image from before the codec moved) is refused", async () => {
	const two = JSON.parse(JSON.stringify(fx.manifest));
	two.ranges.push({ addr: "0x080A0000", size: 16 });
	await assert.rejects(verify(two, image, fb), ManifestError);
	assert.throws(() => makePlan(board, [[board.app_base, image], [0x080A0000, new Uint8Array(16)]], KEEP), PlanError);
});

test("manifest from rfoundry verifies; tampering is refused", async () => {
	assert.equal((await verify(fx.manifest, image, fb)).id, "dm1701");
	await assert.rejects(verify(fx.manifest, image, fb, "md380"), ManifestError);
	const bad = image.slice();
	bad[100] ^= 1;
	await assert.rejects(verify(fx.manifest, bad, fb), ManifestError);
});

test("zip round trip", async () => {
	const big = new Uint8Array(100000).fill(0xFF);
	const z = await zip([{ name: "a.txt", data: "hello" }, { name: "b.bin", data: big }]);
	const m = await unzip(z);
	assert.equal(new TextDecoder().decode(m.get("a.txt")), "hello");
	assert.equal(m.get("b.bin").length, 100000);
	assert.ok(z.length < 2000, "0xFF fill compresses");
});

test("codec decision follows rfoundry's rules", () => {
	const m = fx.manifest;
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: board.codec.id } }).mode, KEEP);
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" }, haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(board, m, { probe: { board: "DM-1701", codec: "absent" } }), DecisionError);
	// no stock firmware keeps the codec where RadioFoundry does: the stock file is needed
	assert.throws(() => decideCodec(board, m, { stockInstalled: "V02.06 CSV" }), DecisionError);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.06 CSV", haveStock: true }).mode, MERGE);
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.03 CSV", haveStock: true }).mode, MERGE);
	// RadioFoundry from before the codec moved reports the old id: its codec is in flash this firmware overwrites
	assert.throws(() => decideCodec(board, m, { probe: { board: "DM-1701", codec: "dm1701-v0206-csv" } }), DecisionError);
	assert.equal(decideCodec(board, m, { probe: { board: "DM-1701", codec: "dm1701-v0206-csv" }, haveStock: true }).mode, MERGE);
	assert.throws(() => decideCodec(board, m, {}), DecisionError);
	assert.throws(() => decideCodec(fb.boards.md380, m, {}), DecisionError);
});

test("a la carte selection keeps dependencies and finds prebuilt variants", () => {
	const radio = json("radios.json").radios.find((r) => r.id === "dm1701");
	const s = new Selection(radio, json("features.json").features, json("variants.json").variants);
	s.useVariant("full");
	assert.equal(s.matches(), "full");
	s.set("DMR", false);
	assert.ok(!s.on.has("DMR_SMS") && !s.on.has("HOTSPOT"), "children follow DMR off");
	assert.equal(s.matches(), null);
	assert.match(s.makeCommand(), /CONFIG_DMR=n/);
	s.set("DMR_SMS", true);
	assert.ok(s.on.has("DMR"), "a child turns its parent back on");
	s.useVariant("amateur");
	assert.equal(s.matches(), "amateur");
	assert.ok(!s.on.has("DMR_PRIVACY"));
	s.set("FONT_XL", true);
	assert.ok(!s.on.has("FONT_XL"), "unsupported on this radio");
	// the MD-380's DTMF: the hardware can, the code isn't written (BOARD_NOT_WRITTEN), so it isn't offered either
	const md380 = json("radios.json").radios.find((r) => r.id === "md380");
	const m = new Selection(md380, json("features.json").features, json("variants.json").variants);
	m.useVariant("full");
	assert.ok(md380.not_written.includes("DTMF") && !md380.unsupported.includes("DTMF"), "not written, not unsupported");
	assert.ok(m.notWritten("DTMF") && !m.supported("DTMF"));
	m.set("DTMF", true);
	assert.ok(!m.on.has("DTMF"), "not written for this radio");
});

test("codeplug formats", () => {
	assert.equal(detect(stock).id, "stock-firmware");
	assert.equal(detect(new Uint8Array(262709)).id, "tyt-rdt");
	assert.equal(convertersFor("tyt-rdt")[0].status, "ready");
	assert.equal(convertersFor("dfuse")[0].status, "planned");
});

// ---- end to end against the simulated radio

async function simulated(state) {
	const sim = await simulation(fb, "dm1701", 7);
	return { sim, radio: new FakeRadio(sim, { state, spiSize: 0x200000 }) };
}

test("first install on stock: the codec merged from the stock file, then back up through RadioFoundry", async () => {
	const { sim, radio } = await simulated("stock");
	const m = { ...fx.manifest };
	assert.throws(() => decideCodec(sim.board, m, { stockInstalled: "V02.06 CSV" }), DecisionError);
	const d = decideCodec(sim.board, m, { stockInstalled: "V02.06 CSV", haveStock: true });
	const { profile, segments: codec } = await stockfw.extract(sim.stock, sim.key, sim.fb);
	// the merge plans against the merged file's layout, one of the board's (its windows are the union of every stock
	// file's layout)
	const lay = stockfw.layoutWindows(profile, sim.fb);
	assert.ok(lay.every(([a, n]) => sim.board.codec.windows.some(([wa, wn]) => wa === a && wn === n)));
	assert.ok(sim.board.codec.windows.length > lay.length);
	assert.deepEqual(codec.map(([a, d]) => [a, d.length]), stockfw.layoutWindows(profile, sim.fb));
	const plan = makePlan(sim.board, segments(m, image), d.mode, codec, stockfw.layoutWindows(profile, sim.fb));
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(plan, sim.key);
	assert.deepEqual(radio.violations, []);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + image.length), image);
	for (const [a, data] of sim.codecSegments) assert.deepEqual(radio.flash.subarray(a - 0x08000000, a - 0x08000000 + data.length), data);
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	assert.equal(p.codec, sim.board.codec.id);
	assert.equal(p.chip.rdp, 1);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const spi = parts.find((x) => x.what === "spi-flash");
	assert.ok(spi.complete);
	assert.deepEqual(spi.data, radio.spi);
	parts.push({ file: "stock-firmware.bin", what: "stock-firmware", addr: 0, data: sim.stock, complete: true });
	const meta = { radio: { board: "dm1701", name: sim.board.name, chip: p.chip }, firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec },
		firmware_before: { kind: "stock", version: "V02.06 CSV" } };
	const bundle = await openBundle(await makeBundle(meta, parts));
	// the memory chip still holds the stock codeplug: written back to RadioFoundry it would be garbage, so it isn't offered
	assert.deepEqual(targets(bundle), ["rfoundry", "stock"]);
	assert.deepEqual(bundle.parts.get("internal-flash").data, radio.flash.subarray(0xC000));
});

test("after the backup, the stock data in RadioFoundry's settings area is claimed; a wrong token changes nothing", async () => {
	const { radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const before = await link.storageInfo();
	assert.equal(before.region, "foreign");
	assert.deepEqual(before.journal, { addr: 0x19000, size: 0x2000 });
	const good = link.chip.bind(link);
	link.chip = async () => ({ ...(await good()), token: 0x12345678 });
	await assert.rejects(link.storageClaim({ timeout: 600 }), /status 7/);
	assert.equal((await link.storageInfo()).region, "foreign");
	link.chip = good;
	assert.equal(await link.storageClaim(), true);
	assert.ok(radio.spi.subarray(0x11000, 0x1B000).every((x) => x === 0xFF));
	assert.ok(radio.events.includes("storage claimed"));
});

test("a read whose reply lost its tail is asked again", async () => {
	const { radio } = await simulated("rfoundry");
	const answer = radio.serialAnswer.bind(radio);
	let reads = 0;
	radio.serialAnswer = (req) => {
		const r = answer(req);
		return req[0] === 0x52 && ++reads === 3 ? r.subarray(0, 1000) : r;	// the radio gave up on a stalled host
	};
	const link = new Link(radio.serialStream(), { timeout: 50 });
	const r = await link.cpsDump(5, 0xC000, 5 * 2045, { chunk: 2045 });
	assert.ok(r.complete);
	assert.equal(reads, 6);
	assert.deepEqual(r.data, radio.flash.subarray(0xC000, 0xC000 + 5 * 2045));
});

test("a RadioFoundry radio gives its bootloader key: found in the bootloader by its hash, nowhere else", async () => {
	const { sim, radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const key = await readBootKey(link, sim.board, sim.fb.key_len);
	assert.deepEqual(key, sim.key);
	// a key at an odd offset is found too; a hash that matches nothing gives nothing
	const region = new Uint8Array(0xC000);
	region.set(sim.key, 0x1235);
	assert.deepEqual(await findKey(region, sim.fb.key_len, sim.board.key_sha256), sim.key);
	assert.equal(await readBootKey(link, { ...sim.board, key_sha256: "00".repeat(32) }, sim.fb.key_len), null);
	// firmware without READ_MEM (or a radio that refuses it): no key, no error
	const answer = radio.serialAnswer.bind(radio);
	radio.serialAnswer = (req) => (req[0] === 0xA5 && req[2] === 0x0C ? frame(0x8C, req[3], new Uint8Array([1])) : answer(req));
	assert.equal(await readBootKey(new Link(radio.serialStream(), { timeout: 50 }), sim.board, sim.fb.key_len), null);
	// the key then flashes the radio with no stock file anywhere
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(sim.board, segments(fx.manifest, image), KEEP), key);
	assert.deepEqual(radio.violations, []);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + image.length), image);
});

test("big reads: 32 KB from RadioFoundry, and whatever an older build gives (2045 bytes)", async () => {
	const { radio } = await simulated("rfoundry");
	let reads = 0;
	const answer = radio.serialAnswer.bind(radio);
	radio.serialAnswer = (req) => { if (req[0] === 0x52) reads++; return answer(req); };
	const link = new Link(radio.serialStream());
	let r = await link.cpsDump(1, 0, 0x40000);
	assert.ok(r.complete);
	assert.equal(reads, 8);
	radio.readMax = 2045;
	reads = 0;
	r = await link.cpsDump(1, 0x1000, 0x10000);
	assert.ok(r.complete);
	assert.equal(reads, Math.ceil(0x10000 / 2045));
	assert.deepEqual(r.data, radio.spi.subarray(0x1000, 0x11000));
});

test("merge on a radio without a codec, then restore stock from the bundle's stock file", async () => {
	const { sim, radio } = await simulated("rfoundry-nocodec");
	radio.flash.fill(0xFF, 0x60000, 0x100000);		// no codec anywhere
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	assert.equal(p.codec, "absent");
	const key = await stockfw.deriveKey(sim.stock, sim.board.key_sha256, sim.fb);
	const { profile, segments: codec } = await stockfw.extract(sim.stock, key, sim.fb);
	const d = decideCodec(sim.board, fx.manifest, { probe: p, haveStock: true });
	assert.equal(d.mode, MERGE);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(makePlan(sim.board, segments(fx.manifest, image), MERGE, codec,
		stockfw.layoutWindows(profile, sim.fb)), key);
	assert.equal((await probe(new Link(radio.serialStream()))).codec, sim.board.codec.id);
	// back to stock: the whole stock application, codec included, through a restore plan
	const plan = planStock(sim.stock, key, sim.board, sim.fb);
	assert.equal(plan.mode, RESTORE);
	radio.enterBootloader();
	radio.afterExit = "stock";
	await new Dfu(radio.dfuTransport(), noWait).run(plan, key);
	const [, app] = stockfw.stockApplication(sim.stock, key, sim.board, sim.fb);
	assert.deepEqual(radio.flash.subarray(0xC000, 0xC000 + app.length), app);
	assert.deepEqual(radio.violations, []);
});

test("a backup made before the project was renamed still opens, as a RadioFoundry backup", async () => {
	const { sim, radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const bundle = await openBundle(await makeBundle({ format: "odr-backup/1", radio: { board: "dm1701", name: "x", chip: p.chip },
		firmware_now: { kind: "odr", git: p.git, codec: p.codec } }, parts));
	assert.equal(bundle.meta.firmware_now.kind, "rfoundry");
});

test("restore a RadioFoundry backup exactly, codeplug included", async () => {
	const { sim, radio } = await simulated("rfoundry");
	const link = new Link(radio.serialStream());
	const p = await probe(link);
	const parts = await readRadio(link, sim.board, { backup: { spi_size: 0x200000 } });
	const bundle = await openBundle(await makeBundle({ radio: { board: "dm1701", name: "x", chip: p.chip }, firmware_now: { kind: "rfoundry", git: p.git, codec: p.codec } }, parts));
	const flashBefore = radio.flash.slice(), spiBefore = radio.spi.slice();
	// the radio changes: a new firmware and an edited codeplug
	radio.flash.fill(0x00, 0xC000, 0x20000);
	radio.spi.fill(0x11, 0x3000, 0x5000);
	radio.spi.fill(0x22, 0x9B000, 0x9C000);
	radio.enterBootloader();
	await new Dfu(radio.dfuTransport(), noWait).run(planInternal(bundle, sim.board), sim.key);
	assert.deepEqual(radio.flash, flashBefore);
	const sectors = codeplugSectors(bundle.parts.get("spi-flash").data, radio.spi);
	assert.deepEqual(sectors.map(([a]) => a), [0x3000, 0x4000, 0x9B000]);
	assert.deepEqual(await writeCodeplug(new Link(radio.serialStream()), sectors), []);
	assert.deepEqual(radio.spi, spiBefore);
});

test("where an update erases the codec, it is read back from the radio and checked window by window", async () => {
	// No STM32 radio's update erases the codec since it moved; a board that can't keep it, with the MD-380's windows
	const board = { ...fb.boards.md380, keep_codec_supported: false };
	const flash = new Uint8Array(0x100000);
	for (let i = 0; i < flash.length; i++) flash[i] = (i * 131 + 7) & 0xFF;
	const windows = [];
	for (const [addr, length] of [[0x08050C44, 0x400], [0x0808FC64, 0x200], [0x080D0FC8, 0x24]]) {
		const o = addr - 0x08000000;
		if (!windows.length) flash.set([fb.codec_sig & 0xFF, (fb.codec_sig >>> 8) & 0xFF, (fb.codec_sig >>> 16) & 0xFF, fb.codec_sig >>> 24], o);
		windows.push([addr, length, await sha256(flash.subarray(o, o + length))]);
	}
	const fbx = { ...fb, profiles: { ["f".repeat(64)]: { board: "md380", codec_profile: board.codec.id, windows } } };
	const link = (mem, refuse = false) => ({ async readMem(addr, n) { if (refuse) throw new Error("refused"); return mem.slice(addr - 0x08000000, addr - 0x08000000 + n); } });
	const got = await readCodec(link(flash), board, fbx);
	assert.deepEqual(got.segments.map(([a]) => a), windows.map(([a]) => a));
	const bad = flash.slice();
	bad[0x0808FC64 - 0x08000000 + 3] ^= 1;
	assert.equal(await readCodec(link(bad), board, fbx), null);
	assert.equal(await readCodec(link(flash, true), board, fbx), null);
	// the decision then needs no stock file, and says where the codec comes from
	const m = { codec: { profile: board.codec.id } };
	assert.equal(decideCodec(board, m, { haveSaved: true }).source, "saved");
	assert.equal(decideCodec(board, m, { haveStock: true }).source, "stock");
	assert.throws(() => decideCodec(board, m, {}), DecisionError);
});

test("a radio that keeps its codec hands over a copy too, from where the merge placed it or where stock keeps it", async () => {
	const key = fromHex(fx.key);
	const { segments: placed } = await stockfw.extract(stock, key, fb, profiles);
	const prof = Object.values(profiles)[0];
	const fbx = { ...fb, profiles };
	const link = (mem) => ({ async readMem(addr, n) { return mem.slice(addr - 0x08000000, addr - 0x08000000 + n); } });
	// a radio on the new layout: the code in sector 11 after the tables
	const now = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, d] of placed) now.set(d, a - 0x08000000);
	const got = await readCodec(link(now), board, fbx);
	assert.deepEqual(got.segments.map(([a, d]) => [a, d.length]), stockfw.placedWindows(prof).map(([a, n]) => [a, n]));
	assert.deepEqual(await codecSegments(got.profile, got.segments, fbx), placed, "written as the stock file's merge writes it");
	// the stock layout (the windows where stock firmware keeps them): placed the same way
	const cont = stockfw.parseContainer(stock, fb);
	const old = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, n] of prof.windows) old.set(stockfw.decrypt(stock, stockfw.appFileOffset(cont, a, fb), n, key, fb), a - 0x08000000);
	const fromOld = await readCodec(link(old), board, fbx);
	assert.equal(fromOld.segments[0][0], prof.windows[0][0]);
	assert.deepEqual(await codecSegments(fromOld.profile, fromOld.segments, fbx), placed);
	now[placed[0][0] - 0x08000000 + 9] ^= 1;
	assert.equal(await readCodec(link(now), board, fbx), null, "a changed byte: no copy");
	// with a copy, a radio whose codec can't be kept or checked needs no stock file
	const m = { codec: { profile: board.codec.id } };
	const rfoundry = (codec) => ({ board: "DM-1701", codec });
	assert.equal(decideCodec(board, m, { probe: rfoundry(board.codec.id), haveSaved: true }).mode, KEEP, "a radio with its codec keeps it");
	assert.equal(decideCodec(board, m, { haveSaved: true }).source, "saved", "no radio answers");
	assert.equal(decideCodec(board, m, { stockInstalled: "V02.06 CSV", haveSaved: true }).source, "saved", "back on stock firmware");
	assert.equal(decideCodec(board, m, { probe: rfoundry("absent"), haveSaved: true }).source, "saved", "RadioFoundry without its codec");
	assert.equal(decideCodec(board, m, { haveSaved: true, requested: KEEP }).mode, KEEP, "keep, as asked");
});

test("backup file names: the model and the local date, letters and digits only, always .zip", () => {
	const when = new Date(2026, 9, 6, 23, 5);
	assert.equal(bundleName({ radio: { board: "dm1801pro", name: "Baofeng DM-1801 Pro", model: "DM-1801 Pro" } }, when), "DM1801Pro_Backup_2026-10-06.zip");
	assert.equal(bundleName({ radio: { board: "dm1701" } }, when), "dm1701_Backup_2026-10-06.zip");
	assert.equal(bundleName({ radio: { board: "x", model: 'A/B: "C"' } }, when), "ABC_Backup_2026-10-06.zip");
});

test("trail of a merge from the stock file: the file by name, size and hash, the decision and why, the key, the plan", async () => {
	diag.reset();
	const debug = console.debug;
	console.debug = () => {};
	try {
		const picked = { name: "DM-1701(CSV)-V02.06.bin", size: stock.length, sha256: await sha256(stock) };
		const opened = await vendorfile.openVendorFile(picked.name, stock, { ...fb, profiles }, "dm1701");
		const b = opened.best;
		const got = { name: picked.name, data: b.data, match: b.match, info: await stockfw.identify(b.data, { ...fb, profiles }) };
		diag.note(...crumbs.filePicked(picked, got));
		const d = decideCodec(board, fx.manifest, { stockInstalled: "V02.06 CSV", haveStock: true });
		diag.note(...crumbs.decision(d));
		diag.note(...crumbs.key("file (opener)"));
		const { profile, segments: codec } = await stockfw.extract(stock, fromHex(fx.key), fb, profiles);
		const plan = makePlan(board, [[board.app_base, image]], MERGE, codec, stockfw.layoutWindows(profile, fb));
		diag.note(...crumbs.plan(plan, profile));
		const tr = diag.trail(200);
		const line = (re) => tr.find((l) => re.test(l));
		assert.match(line(/file picked/), new RegExp(`"size":${stock.length}`));
		assert.match(line(/file picked/), new RegExp(`"sha":"${picked.sha256.slice(0, 12)}"`));
		assert.match(line(/file picked/), /"id":"[a-z-]+:/);
		assert.match(line(/codec decision/), /"mode":"merge","source":"stock","why":"[^"]+"/);
		assert.match(line(/key /), /"source":"file \(opener\)"/);
		assert.match(line(/write plan merge: erase [\d,]+; write [\d:a-z,]+; keep -; \d+ blocks; codec /), new RegExp(profile.name.replace(/[()]/g, "\\$&")));
		assert.ok(tr.every((l) => l.length <= 300));
		// names, sizes and hashes only: no run of the file's bytes
		assert.ok(!tr.join("\n").includes(toHex(stock.subarray(0x100, 0x110))));
	} finally {
		console.debug = debug;
	}
});
