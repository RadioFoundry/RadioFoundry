// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The refs a /flash/?version= link may name (version.js), the default build ref of a preview site, the beta opt-in
// link, how a radio's firmware is named, and which release a radio is offered.

import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { validRef, refShort, defaultRef, firmwareName, betaLink, releaseKey, updateFor } from "../static/js/flasher/version.js";

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
	assert.equal(betaLink("v26.12b1", "https://radiofoundry.org/flash/?reporting=0#radio=x", null),
		"/flash/?reporting=0&version=v26.12b1#radio=x");
	assert.equal(betaLink("v26.12b1", "https://radiofoundry.org/flash/", "main"), null);
	for (const none of ["", null, undefined, "bad tag"]) assert.equal(betaLink(none, "https://radiofoundry.org/flash/", null), null);
});

const NAMES = ["v26.11", "v26.1", "v26.11.1", "v26.12b1", "v26.12b2", "v26.12", "v27.3.2b10", "v27.3.2", "v26.11.12"];

test("release names sort as tools/release_version.py sorts them", () => {
	for (const bad of ["v0.1.0", "v26.011", "v26.13", "v26.11.0", "v26.11b0", "v26.11-rc.1", "v25.12", "", null])
		assert.equal(releaseKey(bad), null, String(bad));
	const cmp = (a, b) => {
		const x = releaseKey(a), y = releaseKey(b);
		const i = x.findIndex((v, k) => v !== y[k]);
		return i < 0 ? 0 : x[i] - y[i];
	};
	for (const a of NAMES) for (const b of NAMES) {
		if (a === b) continue;
		const py = spawnSync("python3", [new URL("../../tools/release_version.py", import.meta.url).pathname, "pick", a, b], { encoding: "utf8" }).stdout.trim();
		assert.equal(cmp(a, b) > 0 ? a : b, py, `${a} vs ${b}`);
	}
});

test("a radio is offered the newest release of its own board, a pre-release only when it runs one", () => {
	const rels = [{ board: "board-a", version: "v26.11" }, { board: "board-a", version: "v26.12b1" }, { board: "board-a", version: "v26.11.2" },
		{ board: "board-b", version: "v27.1" }];
	assert.equal(updateFor(rels, "board-a", "v26.10")?.version, "v26.11.2");
	assert.equal(updateFor(rels, "board-a", "v26.11.2"), null, "up to date");
	assert.equal(updateFor(rels, "board-a", "v26.12"), null, "newer than any listed");
	assert.equal(updateFor(rels, "board-a", "v26.11b3")?.version, "v26.12b1", "a beta tester gets the newest beta");
	assert.equal(updateFor(rels, "board-a", ""), null, "a build that isn't a release");
	assert.equal(updateFor(rels, "board-a", undefined), null);
	assert.equal(updateFor(rels, "board-c", "v26.10"), null, "no release for the board");
	assert.equal(updateFor(null, "board-a", "v26.10"), null);
});

test("a build that isn't a release is offered the newest final release with a newer commit", () => {
	const rels = [{ board: "board-a", version: "v26.11", git_time: 2000 }, { board: "board-a", version: "v26.12b1", git_time: 3000 }];
	assert.equal(updateFor(rels, "board-a", "", 1000)?.version, "v26.11", "an older developer build");
	assert.equal(updateFor(rels, "board-a", undefined, 2500), null, "newer than the release, a pre-release isn't offered");
	assert.equal(updateFor(rels, "board-a", "", 2000), null, "the release's own commit");
	assert.equal(updateFor([{ board: "board-a", version: "v26.11" }], "board-a", "", 1000), null, "a release without git_time");
	assert.equal(updateFor(rels, "board-a", "v26.10", 9999)?.version, "v26.11", "a release is compared by name");
});
