// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware channels (docs/dev/web-flasher.md, Channels): which build an install or an update gets, and the choice
// the page shows. site.json names them at build time (site/cloudflare-build.sh):
//   Stable   the newest release (site.release; a board's own in releases.json): the default
//   Beta     a newer pre-release (site.beta), picked with ?version=<beta>
//   Nightly  the commit of the git tag `nightly` (site.nightly.sha), picked with ?version=nightly; the default of a
//            radio with no release
// Any other version link (?version=main, a branch, a commit, an older tag) is a developer's link: built as asked,
// never listed.

import { defaultRef, releaseKey, updateFor } from "./version.js";

export const NIGHTLY = "nightly";

const nightlyOf = (site) => (site?.nightly && /^[0-9a-f]{40}$/.test(site.nightly.sha || "") ? site.nightly : null);

// The newest release name among a board's releases.json entries, or null
function newestRelease(boardReleases) {
	let best = null, key = null;
	for (const r of boardReleases || []) {
		const k = releaseKey(r.version);
		if (k && (!key || newerKey(k, key))) [best, key] = [r, k];
	}
	return best;
}

// What a radio running a non-release build is closest to: its commit time (VERSION git_time) against the newest
// release's and the nightly's (site.nightly.date)
const nightlyTime = (n) => Math.floor((Date.parse(n?.date || "") || 0) / 1000);

// The build to make: {channel, ref, name, pinned}
//   channel  "stable" | "beta" | "nightly" | "link" (a developer's version link) | "dev" (main, or a preview's branch)
//   ref      what the build service is asked for
//   name     what the page shows for it (a release name, the nightly date or the ref)
//   pinned   a version link chose it
// versionRef: the ?version= of the page (or the tab's), null without one. running: {release, git_time} of the radio
// an update starts from, so the update stays on the radio's channel: a beta tester gets the beta, a build newer than
// the release gets the nightly
export function resolveChannel({ versionRef = null, site = {}, boardReleases = [], running = null } = {}) {
	const nightly = nightlyOf(site), release = newestRelease(boardReleases);
	const asNightly = (pinned) => ({ channel: "nightly", ref: nightly.sha, name: (nightly.date || "").slice(0, 10) || nightly.sha.slice(0, 7), pinned });
	if (versionRef === NIGHTLY) return nightly ? asNightly(true) : { channel: "link", ref: "main", name: "main", pinned: true };
	if (versionRef) {
		if (site.beta && versionRef === site.beta) return { channel: "beta", ref: versionRef, name: versionRef, pinned: true };
		if (release && versionRef === release.version) return { channel: "stable", ref: versionRef, name: versionRef, pinned: true };
		return { channel: "link", ref: versionRef, name: versionRef, pinned: true };
	}
	if (running && release) {
		const now = releaseKey(running.release);
		if (now?.[3] === 0 && site.beta && releaseKey(site.beta) && !newerKey(now, releaseKey(site.beta)))
			return { channel: "beta", ref: site.beta, name: site.beta, pinned: false };
		if (!now && nightly && Number(running.git_time) > (Number(release.git_time) || Infinity)) return asNightly(false);
	}
	if (release) return { channel: "stable", ref: release.version, name: release.version, pinned: false };
	// no release for this radio: a preview builds its own branch, else the nightly, else main
	const dev = defaultRef(site.branch);
	if (dev === "main" && nightly) return asNightly(false);
	return { channel: "dev", ref: dev, name: dev, pinned: false };
}

function newerKey(a, b) {
	for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] > b[i];
	return false;
}

// The channels the page lists: [{channel, name, version}], the version link each sets ("" drops a link: the
// default). Stable only for a board with a release (any release before a radio is picked); the list is empty when
// there is nothing to choose
export function channelChoices(site = {}, boardReleases = null) {
	const nightly = nightlyOf(site);
	const stable = boardReleases ? newestRelease(boardReleases)?.version : site.release;
	const out = [];
	if (stable) out.push({ channel: "stable", name: stable, version: "" });
	if (stable && site.beta && releaseKey(site.beta)) out.push({ channel: "beta", name: site.beta, version: site.beta });
	if (nightly) out.push({ channel: "nightly", name: (nightly.date || "").slice(0, 10) || nightly.sha.slice(0, 7), version: NIGHTLY });
	return out.length > 1 ? out : [];
}

// A choice's link: this page with ?version= set (or dropped)
export function channelLink(href, version) {
	const u = new URL(href);
	u.searchParams.set("version", version);
	return u.pathname + u.search + u.hash;
}

// The editor's and the update's offer for a radio that runs a non-release build: the nightly when its commit is newer
// than the radio's build (git_time) and than the newest release's, else null
export function nightlyOffer(site, boardReleases, gitTime) {
	const nightly = nightlyOf(site), t = nightlyTime(nightly);
	if (!nightly || !(gitTime > 0) || !(t > gitTime)) return null;
	const release = newestRelease(boardReleases);
	if (release && Number(release.git_time) >= t) return null;
	return { version: NIGHTLY, channel: "nightly", name: (nightly.date || "").slice(0, 10), git: nightly.sha.slice(0, 8), git_time: t };
}

// What the Codeplug Editor offers a radio after a read, on the channel its build came from: a radio that runs a
// release gets a newer release (updateFor), a beta tester the newer beta of site.json too, and any other build
// (VERSION git_time) the nightly when the nightly is the newest, else a newer release. Each answer carries channel,
// and version is what the flasher's link asks for ("nightly" for the nightly build). null: nothing newer
export function channelOffer(site, releases, board, running, gitTime = 0) {
	const rel = updateFor(releases, board, running, gitTime);
	const now = releaseKey(running);
	if (now) {
		const beta = releaseKey(site?.beta);
		if (now[3] === 0 && beta && newerKey(beta, now) && (!rel || newerKey(beta, releaseKey(rel.version))))
			return { version: site.beta, channel: "beta" };
		return rel && { ...rel, channel: releaseKey(rel.version)[3] === 0 ? "beta" : "stable" };
	}
	return nightlyOffer(site, (releases || []).filter((r) => r.board === board), gitTime) || (rel && { ...rel, channel: "stable" });
}

// The flasher's address for an offer: its update, on the offer's channel
export function offerLink(home, offer) {
	const version = offer.channel === "nightly" ? NIGHTLY : offer.channel === "beta" ? offer.version : "";
	return `${home}flash/index.html${version ? `?version=${encodeURIComponent(version)}` : ""}#update`;
}
