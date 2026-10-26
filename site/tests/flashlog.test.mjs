// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash log's id for a vendor file (vendorfile.js logId): short, from board ids and known build names only.

import { test } from "node:test";
import assert from "node:assert/strict";
import { logId } from "../static/js/flasher/vendorfile.js";

test("a recognized file", () => {
	assert.equal(logId({ match: { kind: "codec", board: "radio-a", known: "Build A 1.0" } }), "codec:Build A 1.0");
	assert.equal(logId({ match: { kind: "codec", board: "radio-a", how: "sha256" } }), "codec:sha256");
});

test("a build with no recipe, or only the key", () => {
	assert.equal(logId({ match: { kind: "codec-unmapped", board: "radio-b" } }), "no-recipe:radio-b");
	assert.equal(logId({ match: { kind: "key", board: "radio-b" } }), "key:radio-b");
});

test("no usable file inside, then what the page knows of the picked one", () => {
	assert.equal(logId({ problem: { problem: "wrong-key" }, info: {} }), "problem:wrong-key");
	assert.equal(logId({ info: { known: "Build B 2.0" } }), "known:Build B 2.0");
	assert.equal(logId({ info: {} }), "unknown");
});

test("never longer than the backend takes", () => {
	assert.equal(logId({ match: { kind: "codec", known: "x".repeat(200) } }).length, 64);
});
