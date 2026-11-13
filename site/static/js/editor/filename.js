// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Saved file names in the Codeplug Editor: every file the editor offers for download gets the local date and time
// in its name, so saving twice never hands the browser two files with the same name. A name that already ends in
// a date and time (a file saved this way, then opened) gets the new one in its place, not a second one after it.

const STAMP = /-\d{4}-\d{2}-\d{2}-\d{4}$/;
const pad = (n) => String(n).padStart(2, "0");

// The file name with the local date and time in it: <stem>-YYYY-MM-DD-HHMM<ext>, the time as the clock on this
// computer reads it. Only digits and dashes go in, so the name is safe on Windows, macOS and Linux.
export function timestampedName(name, when = new Date()) {
	if (!name) return name;
	const dot = name.lastIndexOf(".");
	const stem = dot > 0 ? name.slice(0, dot) : name;
	const ext = dot > 0 ? name.slice(dot) : "";
	const stamp = `-${when.getFullYear()}-${pad(when.getMonth() + 1)}-${pad(when.getDate())}-${pad(when.getHours())}${pad(when.getMinutes())}`;
	return `${stem.replace(STAMP, "")}${stamp}${ext}`;
}
