// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A build the service couldn't fit: its error is "too big: <region> overflowed by <N> bytes", the linker's region
// (FLASH, or a RAM region) and how far over (the build host's odr_build.py). No DOM here, so the host tests check it.

const TOO_BIG = /^too big: (\S+) overflowed by (\d+) bytes/;

// {region, bytes, kb (rounded up), memory: "flash" | "ram"} of a build error that says the image doesn't fit, else null
export function tooBig(error) {
	const m = TOO_BIG.exec(String(error || ""));
	if (!m) return null;
	const bytes = Number(m[2]);
	return { region: m[1], bytes, kb: Math.max(1, Math.ceil(bytes / 1024)), memory: /flash|rom/i.test(m[1]) ? "flash" : "ram" };
}
