// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The refs a /flash/?version= link may name (version.js), the default build ref of a preview site, the beta opt-in
// link and how a radio's firmware is named.

import { test } from "node:test";
import assert from "node:assert/strict";
import { validRef, refShort, defaultRef, firmwareName, betaLink } from "../static/js/flasher/version.js";

test("tags, full hashes and branch names are valid refs", () => {
	for (const ok of ["v0.3.0", "v1.2.3-rc1", "a".repeat(40), "main", "feature/mduv380_bringup", "fix-1.2", "a/b/c/d"])
		assert.ok(validRef(ok), ok);
});

test("anything else is refused", () => {
	for (const bad of ["", null, undefined, "..", "a..b", "feature/x.lock", "x.lock", "-lead", "/a", "a/", "a//b", "a b",
		"a/b/c/d/e", "x".repeat(41), "a;b", "feature/../x"])
		assert.ok(!validRef(bad), String(bad));
});

test("a hash is shortened, a branch is shown whole", () => {
	assert.equal(refShort("0123456789abcdef0123456789abcdef01234567"), "0123456");
	assert.equal(refShort("feature/mduv380_bringup"), "feature/mduv380_bringup");
});

test("a preview builds its own branch, main and an unset branch build main", () => {
	assert.equal(defaultRef("feature/mduv380_bringup"), "feature/mduv380_bringup");
	for (const b of ["main", "master", "", null, undefined, "bad branch", "a..b"]) assert.equal(defaultRef(b), "main", String(b));
});

test("a release names the radio's firmware with its commit, a plain build by its commit", () => {
	assert.equal(firmwareName({ git: "a1b2c3d4", release: "v26.11" }), "v26.11 (a1b2c3d4)");
	assert.equal(firmwareName({ git: "a1b2c3d4-dirty" }), "a1b2c3d4-dirty");
});

test("a beta is offered as a version link on this page, never over one in use", () => {
	assert.equal(betaLink("v26.12-beta.1", "https://radiofoundry.org/flash/?reporting=0#radio=x", null),
		"/flash/?reporting=0&version=v26.12-beta.1#radio=x");
	assert.equal(betaLink("v26.12-beta.1", "https://radiofoundry.org/flash/", "main"), null);
	for (const none of ["", null, undefined, "bad tag"]) assert.equal(betaLink(none, "https://radiofoundry.org/flash/", null), null);
});
