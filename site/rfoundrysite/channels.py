# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The site's firmware channels (docs/dev/website.md, Channels), picked at build time by site/cloudflare-build.sh.

  Stable   the newest release that is not a pre-release: what the flasher installs by default
  Beta     the newest pre-release (a bN tag) when it is newer than Stable
  Nightly  the commit of the git tag `nightly`, which the nightly radio tests move when main passes them

Nothing here reaches the network: the caller hands in what the GitHub API returned, so the rules can be tested.
"""
import importlib.util
import os

from . import repo

NIGHTLY_TAG = "nightly"


def _release_version():
	spec = importlib.util.spec_from_file_location("release_version", repo.path("tools", "release_version.py"))
	mod = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(mod)
	return mod


release_version = _release_version()


def stable_and_beta(releases):
	"""(stable tag or None, beta tag or None) from the API's release list. By release name (tools/release_version.py),
	never drafts; a tag that isn't a release name is never offered."""
	named = {r["tag_name"]: r for r in releases or [] if not r.get("draft") and release_version.parse(r["tag_name"]) is not None}
	stable = release_version.newest([t for t, r in named.items() if not r.get("prerelease")])
	beta = release_version.newest([t for t, r in named.items() if r.get("prerelease")])
	if beta and stable and release_version.parse(beta) < release_version.parse(stable):
		beta = None
	return stable, beta


def nightly(ref, tag_object=None, commit=None):
	"""{"sha", "date"} of the nightly tag, or None without one.

	ref: GET /git/ref/tags/nightly (None for a 404). tag_object: GET /git/tags/<sha> when the tag is annotated.
	commit: GET /commits/<sha> of the commit, for its date (ISO 8601, UTC)."""
	if not ref:
		return None
	obj = ref.get("object") or {}
	if obj.get("type") == "tag":
		obj = (tag_object or {}).get("object") or {}
	sha = obj.get("sha") or ""
	if obj.get("type") != "commit" or len(sha) != 40:
		return None
	date = (((commit or {}).get("commit") or {}).get("committer") or {}).get("date") or ""
	return {"sha": sha, "date": date}


def parse_nightly(sha, date):
	"""site/build.py's --nightly and --nightly-date: the same dictionary, or None when sha is empty."""
	if not sha:
		return None
	if len(sha) != 40 or any(c not in "0123456789abcdef" for c in sha):
		raise repo.SourceError("--nightly %s is not a full commit hash" % sha)
	return {"sha": sha, "date": date or ""}


def write(path, value):
	"""One channel's answer for the shell script: a line, empty for none."""
	os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
	with open(path, "w", encoding="utf-8") as f:
		f.write(value or "")
