// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware channels (flasher/channel.js): which build an install or an update gets with no tag, only a nightly
// tag, or a release with a beta and a nightly; the ?version= links nightly, main and a beta; the channel choice and
// the Codeplug Editor's update offer.

import { test } from "node:test";
import assert from "node:assert/strict";
import { resolveChannel, channelChoices, channelLink, channelOffer, nightlyOffer, offerLink } from "../static/js/flasher/channel.js";

const SHA = "0123456789abcdef0123456789abcdef01234567";
const NIGHTLY = { sha: SHA, date: "2026-11-21T03:12:00Z" };
const NIGHTLY_T = Date.parse(NIGHTLY.date) / 1000;
const RELS = [{ board: "board-a", version: "v26.11", git_time: NIGHTLY_T - 86400 * 30 }, { board: "board-a", version: "v26.12", git_time: NIGHTLY_T - 86400 * 5 }];
const ALL = { release: "v26.12", beta: "v27.1b1", nightly: NIGHTLY, branch: "main" };

test("no tag and no release: main, or a preview's own branch", () => {
	assert.deepEqual(resolveChannel({ site: { branch: "main" } }), { channel: "dev", ref: "main", name: "main", pinned: false });
	assert.equal(resolveChannel({ site: { branch: "feature/x" } }).ref, "feature/x");
	assert.equal(resolveChannel({ site: { branch: "feature/x", nightly: NIGHTLY } }).ref, "feature/x", "a preview tries its branch, not the nightly");
	assert.deepEqual(channelChoices({ branch: "main" }), [], "nothing to choose");
});

test("only a nightly tag: a radio with no release builds the nightly commit", () => {
	const c = resolveChannel({ site: { nightly: NIGHTLY, branch: "main" } });
	assert.deepEqual(c, { channel: "nightly", ref: SHA, name: "2026-11-21", pinned: false });
	assert.deepEqual(channelChoices({ nightly: NIGHTLY }), [], "one channel is no choice");
	assert.equal(resolveChannel({ site: { nightly: { sha: "short", date: "" } } }).ref, "main", "a malformed nightly is ignored");
});

test("release, beta and nightly: the default is the radio's newest release", () => {
	assert.deepEqual(resolveChannel({ site: ALL, boardReleases: RELS }), { channel: "stable", ref: "v26.12", name: "v26.12", pinned: false });
	assert.equal(resolveChannel({ site: ALL, boardReleases: [] }).channel, "nightly", "a radio without a release of its own");
	assert.deepEqual(channelChoices(ALL, RELS).map((c) => [c.channel, c.name, c.version]),
		[["stable", "v26.12", ""], ["beta", "v27.1b1", "v27.1b1"], ["nightly", "2026-11-21", "nightly"]]);
	assert.deepEqual(channelChoices(ALL).map((c) => c.channel), ["stable", "beta", "nightly"], "before a radio is picked: the site's release");
	assert.deepEqual(channelChoices(ALL, []), [], "a radio without a release has only the nightly");
});

test("?version=nightly, a beta, main, the release and any other ref", () => {
	assert.deepEqual(resolveChannel({ versionRef: "nightly", site: ALL, boardReleases: RELS }), { channel: "nightly", ref: SHA, name: "2026-11-21", pinned: true });
	assert.deepEqual(resolveChannel({ versionRef: "nightly", site: { release: "v26.12" }, boardReleases: RELS }),
		{ channel: "link", ref: "main", name: "main", pinned: true }, "no nightly tag yet: main");
	assert.deepEqual(resolveChannel({ versionRef: "v27.1b1", site: ALL, boardReleases: RELS }), { channel: "beta", ref: "v27.1b1", name: "v27.1b1", pinned: true });
	assert.deepEqual(resolveChannel({ versionRef: "main", site: ALL, boardReleases: RELS }), { channel: "link", ref: "main", name: "main", pinned: true });
	assert.equal(resolveChannel({ versionRef: "v26.12", site: ALL, boardReleases: RELS }).channel, "stable");
	assert.equal(resolveChannel({ versionRef: "v26.11", site: ALL, boardReleases: RELS }).channel, "link", "an older release is a version link");
	assert.equal(resolveChannel({ versionRef: SHA, site: ALL, boardReleases: RELS }).channel, "link");
});

test("an update stays on the radio's channel", () => {
	const at = (running) => resolveChannel({ site: ALL, boardReleases: RELS, running });
	assert.equal(at({ release: "v26.11" }).ref, "v26.12", "a release gets the release");
	assert.equal(at({ release: "v26.12b2" }).ref, "v27.1b1", "a beta tester gets the newer beta");
	assert.equal(at({ release: "v27.1b1" }).ref, "v27.1b1", "the newest beta itself stays on Beta");
	assert.equal(resolveChannel({ site: { ...ALL, beta: "" }, boardReleases: RELS, running: { release: "v26.12b2" } }).ref, "v26.12",
		"no beta any more: the release that followed");
	assert.equal(at({ git: "abc", git_time: NIGHTLY_T - 3600 }).ref, SHA, "a build newer than the release gets the nightly");
	assert.equal(at({ git: "abc", git_time: RELS[1].git_time - 10 }).ref, "v26.12", "an older developer build gets the release");
	assert.equal(resolveChannel({ versionRef: "main", site: ALL, boardReleases: RELS, running: { git_time: NIGHTLY_T } }).ref, "main", "a link wins");
});

test("a choice's link sets or drops ?version= and keeps the rest", () => {
	assert.equal(channelLink("https://radiofoundry.org/flash/?reporting=0#radio=x", "nightly"), "/flash/?reporting=0&version=nightly#radio=x");
	assert.equal(channelLink("https://radiofoundry.org/flash/?version=nightly", ""), "/flash/?version=");
});

test("the editor offers a release, a beta or the nightly by the radio's build", () => {
	assert.equal(channelOffer(ALL, RELS, "board-a", "v26.11")?.version, "v26.12");
	assert.equal(channelOffer(ALL, RELS, "board-a", "v26.12"), null, "up to date on Stable");
	assert.deepEqual(channelOffer(ALL, RELS, "board-a", "v26.12b3"), { version: "v27.1b1", channel: "beta" });
	assert.equal(channelOffer(ALL, RELS, "board-a", "v27.1b1"), null, "the newest beta");
	const night = channelOffer(ALL, RELS, "board-a", undefined, NIGHTLY_T - 3600);
	assert.equal(night.channel, "nightly");
	assert.equal(night.version, "nightly");
	assert.equal(channelOffer(ALL, RELS, "board-a", undefined, NIGHTLY_T), null, "the nightly itself");
	assert.equal(channelOffer({}, RELS, "board-a", "", RELS[0].git_time - 1)?.version, "v26.12", "no nightly: the release, as before");
	assert.equal(nightlyOffer({ nightly: { sha: SHA, date: "2026-01-01T00:00:00Z" } }, RELS, NIGHTLY_T - 86400 * 400), null,
		"a release newer than the nightly wins");
	assert.equal(offerLink("../", { channel: "nightly", version: "nightly" }), "../flash/index.html?version=nightly#update");
	assert.equal(offerLink("../", { channel: "beta", version: "v27.1b1" }), "../flash/index.html?version=v27.1b1#update");
	assert.equal(offerLink("", { channel: "stable", version: "v26.12" }), "flash/index.html#update");
});
