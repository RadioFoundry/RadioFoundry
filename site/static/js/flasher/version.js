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
