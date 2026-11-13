// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Saved file names in the Codeplug Editor (static/js/editor/filename.js): the date and time in the name, and a
// name that already ends in one gets it replaced, not stacked.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

import { timestampedName } from "../static/js/editor/filename.js";

// 10 November 2026, 22:15 local time: the Date is built from local parts and the helper reads local parts, so
// these hold in any time zone
const at = (y, mo, d, h, mi) => new Date(y, mo - 1, d, h, mi);

test("a plain name gets the local date and time before its extension", () => {
	assert.equal(timestampedName("codeplug.yaml", at(2026, 11, 10, 22, 15)), "codeplug-2026-11-10-2215.yaml");
	assert.equal(timestampedName("codeplug.bin", at(2026, 11, 10, 22, 15)), "codeplug-2026-11-10-2215.bin");
	assert.equal(timestampedName("dm32-codeplug-channels.csv", at(2026, 11, 10, 22, 15)), "dm32-codeplug-channels-2026-11-10-2215.csv");
	assert.equal(timestampedName("dm32-codeplug-csv.zip", at(2026, 11, 10, 22, 15)), "dm32-codeplug-csv-2026-11-10-2215.zip");
});

test("a name that already ends in a date and time gets it replaced, not stacked", () => {
	assert.equal(timestampedName("dm32-codeplug-2026-11-10-2215.yaml", at(2026, 11, 11, 9, 5)), "dm32-codeplug-2026-11-11-0905.yaml");
	assert.equal(timestampedName("codeplug-2026-11-10-2215.bin", at(2026, 11, 11, 9, 5)), "codeplug-2026-11-11-0905.bin");
});

test("months, days, hours and minutes are zero padded", () => {
	assert.equal(timestampedName("codeplug.yaml", at(2026, 1, 2, 3, 4)), "codeplug-2026-01-02-0304.yaml");
});

test("with no time given it stamps the current time, in the same shape", () => {
	assert.match(timestampedName("codeplug.yaml"), /^codeplug-\d{4}-\d{2}-\d{2}-\d{4}\.yaml$/);
});

test("only a stamp in the exact format is replaced", () => {
	assert.equal(timestampedName("codeplug-2026-11-10.yaml", at(2026, 11, 11, 9, 5)), "codeplug-2026-11-10-2026-11-11-0905.yaml");
});

test("a name without an extension gets the stamp at the end", () => {
	assert.equal(timestampedName("codeplug", at(2026, 11, 10, 22, 15)), "codeplug-2026-11-10-2215");
});
