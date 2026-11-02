// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Which source a build is asked for: a release tag, a full commit hash, or a branch of the main repository. The branch
// rule is the build backend's, so a name that passes here is not refused for its form there.

const TAG_RE = /^v[0-9][0-9A-Za-z.\-+]{0,31}$/;
const HASH_RE = /^[0-9a-f]{40}$/;
const BRANCH_PART = "[A-Za-z0-9][A-Za-z0-9_.-]{0,39}";
const BRANCH_RE = new RegExp(`^${BRANCH_PART}(/${BRANCH_PART}){0,3}$`);

export function isBranch(ref) {
	return typeof ref === "string" && BRANCH_RE.test(ref) && !ref.includes("..")
		&& !ref.split("/").some((p) => p.endsWith(".lock"));
}

// A tag, a full hash (lowercase) or a branch name
export function validRef(ref) {
	return typeof ref === "string" && (TAG_RE.test(ref) || HASH_RE.test(ref) || isBranch(ref));
}

// What the page shows for a ref: a hash shortened, a tag or branch as is
export function refShort(ref) {
	return HASH_RE.test(ref) ? ref.slice(0, 7) : ref;
}

// The ref a board with no release is built from: the site's own branch when it is a preview of another branch, so a
// radio that exists only there can be tried, else main
export function defaultRef(siteBranch) {
	return siteBranch && siteBranch !== "main" && siteBranch !== "master" && isBranch(siteBranch) ? siteBranch : "main";
}

// What a radio running RadioFoundry says it runs (its VERSION reply): the release name and the commit, or the commit
export function firmwareName(version) {
	return version.release ? `${version.release} (${version.git})` : version.git;
}

// The link that opts in to a pre-release newer than the release (site.json beta): this page with ?version=<beta>, or
// null when there is none or a version link is already in use
export function betaLink(beta, href, versionRef) {
	if (!beta || !validRef(beta) || versionRef) return null;
	const u = new URL(href);
	u.searchParams.set("version", beta);
	return u.pathname + u.search + u.hash;
}

// A release name's sort key (newer is larger), or null when it isn't one: tools/release_version.py's parse(). vYY.M,
// vYY.M.N, either with bN for a pre-release, which sorts before its final
const RELEASE_RE = /^v(2[6-9]|[3-9][0-9])\.(1[0-2]|[1-9])(?:\.([1-9][0-9]{0,2}))?(?:b([1-9][0-9]{0,2}))?$/;

export function releaseKey(name) {
	const m = RELEASE_RE.exec(name || "");
	if (!m) return null;
	return [+m[1], +m[2], +(m[3] || 0), m[4] ? 0 : 1, +(m[4] || 0)];
}

const newer = (a, b) => {
	for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] > b[i];
	return false;
};

// The release to offer a radio: the newest of the board's full releases (releases.json) when it is newer than what
// the radio runs, else null. A radio that runs a release (VERSION's release) is compared by name, and gets
// pre-releases only when it runs one. Any other build (a developer's, one from a version link) is compared by commit
// time (VERSION's git_time against the release's) and gets only final releases. A build that says neither gets none
export function updateFor(releases, board, running, gitTime = 0) {
	const now = releaseKey(running);
	if (!now && !(gitTime > 0)) return null;
	const beta = now?.[3] === 0;
	let best = null, bestKey = null;
	for (const r of releases || []) {
		const k = releaseKey(r.version);
		if (r.board !== board || (r.variant && r.variant !== "full") || !k || (k[3] === 0 && !beta)) continue;
		if (!bestKey || newer(k, bestKey)) [best, bestKey] = [r, k];
	}
	if (!best) return null;
	return (now ? newer(bestKey, now) : best.git_time > gitTime) ? best : null;
}
