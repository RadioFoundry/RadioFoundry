// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's original firmware version, read over WebUSB before an install, and the hardware option it settles: a
// port of tools/rfoundry/rfoundry/origversion.py. A board that declares it (board.toml [flash.original_version],
// flash-boards.json original_version) has revisions RadioFoundry can't tell apart while it runs, and its original
// firmware's major version follows the revision. The page reads the version before it asks for the bootloader and
// stores the choice over the debug link's HWOPT (docs/spec/debuglink.md 3.25) once RadioFoundry runs.
//
// Read "dfu-info" (docs/spec/hardware-md9600.md 15.1): the original firmware, running normally, is a DFU device with
// the bootloader's USB IDs. Its interface string (original_version.interface) tells it from the bootloader before
// anything is sent (runsOriginal); only then these requests, none of which changes anything on the radio: GETSTATUS
// (CLRSTATUS in dfuERROR, ABORT in any other state but dfuIDLE); DNLOAD block 0 `A2 02`; GETSTATUS (dfuDNBUSY);
// GETSTATUS (dfuDNLOAD-IDLE); ABORT; UPLOAD block 0, 1024 bytes: `56 0M m0 mm`, version Mm.mm. The bootloader answers
// `54 ...` ('T'); anything else, a stall included: unknown. No DOM here.

export const READS = ["dfu-info"];
export const ORIGINAL = "original", BOOTLOADER = "bootloader";
const DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6;
const ST_IDLE = 2, ST_DNBUSY = 4, ST_DNIDLE = 5, ST_ERROR = 10;

// The version text ("06.09") in a dfu-info answer, or null
export function parseVersion(data) {
	if (!data || data.length !== 4 || data[0] !== 0x56) return null;
	const n = [data[1] >> 4, data[1] & 15, data[2] >> 4, data[2] & 15, data[3] >> 4, data[3] & 15];
	// 0M m0 mm: the major's tens in byte 1, its units in byte 2's high nibble, the minor in byte 3, all decimal
	if (n[0] || n[3] || n.some((x) => x > 9)) return null;
	return `${n[1]}${n[2]}.${n[4]}${n[5]}`;
}

// {kind, version} of a dfu-info answer: ORIGINAL with the version, BOOTLOADER for the bootloader's 'T' answer, else nulls
export function answerKind(data) {
	const version = parseVersion(data);
	if (version) return { kind: ORIGINAL, version };
	if (data?.length === 4 && data[0] === 0x54) return { kind: BOOTLOADER, version: null };
	return { kind: null, version: null };
}

const norm = (x) => String(x || "").split(/\s+/).filter(Boolean).join(" ");

// Whether a DFU device's interface strings (dfu.js fingerprint().interfaces) are the original firmware's, running
// normally (spaces not counted). False for the bootloader's and for strings the browser didn't give
export function runsOriginal(spec, interfaces) {
	const want = norm(spec?.interface);
	return !!want && (interfaces || []).some((i) => i && norm(i) === want);
}

// The hardware option choice a version settles (spec: the board's original_version), or null
export function choiceFor(spec, version) {
	if (!spec || !version) return null;
	return spec.major[version.split(".")[0]] ?? null;
}

// {kind, version} from a device (answerKind; nulls for a stall or odd states). transport: dfu.js's (out(request, value,
// data), in(request, length)). The caller checks runsOriginal first. Never throws
export async function readDfuInfo(transport, { log = () => {} } = {}) {
	const status = async () => {
		const r = await transport.in(GETSTATUS, 6);
		if (r.length !== 6) throw new Error("short GETSTATUS reply");
		return { status: r[0], state: r[4] };
	};
	const idle = async () => {
		const { state } = await status();
		if (state === ST_ERROR) await transport.out(CLRSTATUS, 0);
		else if (state !== ST_IDLE) await transport.out(ABORT, 0);
	};
	let data;
	try {
		await idle();
		await transport.out(DNLOAD, 0, new Uint8Array([0xA2, 0x02]));
		const a = await status(), b = await status();
		if (a.status || a.state !== ST_DNBUSY || b.status || b.state !== ST_DNIDLE) {
			log(`no version: the device answered states ${a.state}, ${b.state}`);
			try { await idle(); } catch (e) { /* gone */ }
			return { kind: null, version: null };
		}
		await transport.out(ABORT, 0);
		data = await transport.in(UPLOAD, 1024);
	} catch (e) {	// a stall: not the original firmware
		log(`no version: ${e?.message || e}`);
		return { kind: null, version: null };
	}
	const r = answerKind(data);
	if (!r.kind) log(`no version: the answer is ${Array.from(data.subarray(0, 16), (x) => x.toString(16).padStart(2, "0")).join("")}`);
	return r;
}

// What to do with a choice for the hardware option `name`, given the radio's HWOPT list (link.js hwopts()):
// { action, option, choice, held }. action: "store" (nothing stored yet), "same" (already held), "mismatch" (another
// choice held: leave it, tell the user), "missing" (no such option or choice in this RadioFoundry)
export function hwoptAction(opts, name, choice) {
	const option = opts.findIndex((o) => o.name === name);
	if (option < 0) return { action: "missing", option: null, choice: null, held: null };
	const o = opts[option], c = o.choices.indexOf(choice);
	if (c < 0) return { action: "missing", option, choice: null, held: null };
	if (o.value < 0) return { action: "store", option, choice: c, held: null };
	if (o.value >= o.choices.length) return { action: "missing", option, choice: c, held: null };
	return { action: o.value === c ? "same" : "mismatch", option, choice: c, held: o.choices[o.value] };
}

// The first hardware option the radio still needs from a person (HWOPT state "ask": transmit waits for it) or that
// waits for a restart ("restart"), as { option (its index), ...the option }, or null (debuglink.md 3.25)
export function pendingHwopt(opts) {
	const i = opts.findIndex((o) => o.state === "ask" || o.state === "restart");
	return i < 0 ? null : { option: i, ...opts[i] };
}

// The original firmware's major versions that mean `choice` (board.toml [flash.original_version] major), for a person
// who remembers the version but can't read it any more (another firmware runs now)
export function majorsFor(spec, choice) {
	return Object.entries(spec?.major || {}).filter(([, c]) => c === choice).map(([m]) => m).sort();
}
