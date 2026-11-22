#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The web site's build on Cloudflare Pages (docs/dev/website.md, Hosting): the Pages project runs this from the
# repository root on each push to main and writes the site to build/site.
#   site/cloudflare-build.sh
set -eu
cd "$(dirname "$0")/.."
python3 -m pip install -q -r site/requirements.txt
# The channels (docs/dev/website.md, Channels), from the public API: the newest release's images for the flasher
# (Stable), a newer pre-release's name (Beta, a bN tag) and the commit of the `nightly` tag (Nightly). No release or
# no nightly tag yet is fine: the flasher then builds the nightly commit or main. Any other failure stops the build,
# so a deployment never loses the images by accident
mkdir -p build/dist build/channels
# Without a token the API allows 60 requests an hour per address, and the Pages builders share theirs, so a build can
# get 403 "rate limit exceeded". GITHUB_TOKEN (a read-only token in the Pages project's variables) raises that to 5000;
# without one, a rate limit is retried for about a minute before the build fails
python3 - build/dist build/channels <<'EOF'
import json, os, sys, time, urllib.error, urllib.request
sys.path.insert(0, "site")
from rfoundrysite import channels
api = "https://api.github.com/repos/RadioFoundry/RadioFoundry/"
headers = {"Accept": "application/vnd.github+json"}
if os.environ.get("GITHUB_TOKEN"):
	headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
def get(path, missing_ok=False):
	for attempt in range(4):
		try:
			return json.load(urllib.request.urlopen(urllib.request.Request(api + path, headers=headers), timeout=60))
		except urllib.error.HTTPError as e:
			if e.code == 404 and missing_ok:
				return None
			if e.code not in (403, 429) or attempt == 3:
				if e.code in (403, 429):
					print("GitHub API rate limit: set GITHUB_TOKEN in the Pages project (site/cloudflare-build.sh)")
				raise
			print("GitHub API %d, retrying" % e.code)
			time.sleep(10 * (attempt + 1))
dist, out = sys.argv[1], sys.argv[2]
rels = get("releases?per_page=30")
stable, beta = channels.stable_and_beta(rels)
ref = get("git/ref/tags/" + channels.NIGHTLY_TAG, missing_ok=True)
tag = get("git/tags/" + ref["object"]["sha"]) if ref and ref.get("object", {}).get("type") == "tag" else None
night = channels.nightly(ref, tag)
if night:
	night = channels.nightly(ref, tag, get("commits/" + night["sha"]))
channels.write(os.path.join(out, "stable"), stable)
channels.write(os.path.join(out, "beta"), beta)
channels.write(os.path.join(out, "nightly"), night and night["sha"])
channels.write(os.path.join(out, "nightly-date"), night and night["date"])
print("beta", beta or "none")
print("nightly", ("%s %s" % (night["sha"][:8], night["date"])) if night else "none")
if not stable:
	print("no release yet")
	sys.exit(0)
print("release", stable)
for a in next(r for r in rels if r["tag_name"] == stable)["assets"]:
	if a["name"].startswith("rfoundry-"):
		urllib.request.urlretrieve(a["browser_download_url"], os.path.join(dist, a["name"]))
		print("release asset", a["name"])
EOF
STABLE=$(cat build/channels/stable)
# The user manual of the Stable release goes at /docs/, main's at /nightly/docs/. The release's docs/ comes from its
# tag; a clone that can't fetch the tag builds main's manual at /docs/ as before the first release, with a warning
DOCS_RELEASE=""
if [ -n "$STABLE" ]; then
	rm -rf build/docs-release && mkdir -p build/docs-release
	if git fetch -q --depth=1 origin "+refs/tags/$STABLE:refs/tags/$STABLE" &&
		git archive "$STABLE" docs | tar -x -C build/docs-release; then
		DOCS_RELEASE=build/docs-release
	else
		echo "warning: could not read docs/ of $STABLE; /docs/ shows main's manual"
	fi
fi
python3 site/build.py --strict --out build/site --firmware build/dist --beta "$(cat build/channels/beta)" \
	--nightly "$(cat build/channels/nightly)" --nightly-date "$(cat build/channels/nightly-date)" \
	--docs-release "$DOCS_RELEASE" --ref "${CF_PAGES_COMMIT_SHA:-main}"
