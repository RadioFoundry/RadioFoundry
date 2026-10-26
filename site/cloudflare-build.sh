#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The web site's build on Cloudflare Pages (docs/dev/website.md, Hosting): the Pages project runs this from the
# repository root on each push to main and writes the site to build/site.
#   site/cloudflare-build.sh
set -eu
cd "$(dirname "$0")/.."
python3 -m pip install -q -r site/requirements.txt
# the newest release's images for the flasher, from the public API. No release yet is fine: the flasher then offers
# local images only. Any other failure stops the build, so a deployment never loses the images by accident
mkdir -p build/dist
# Without a token the API allows 60 requests an hour per address, and the Pages builders share theirs, so a build can
# get 403 "rate limit exceeded". GITHUB_TOKEN (a read-only token in the Pages project's variables) raises that to 5000;
# without one, a rate limit is retried for about a minute before the build fails
python3 - build/dist <<'EOF'
import json, os, sys, time, urllib.error, urllib.request
api = "https://api.github.com/repos/RadioFoundry/RadioFoundry/releases/latest"
headers = {"Accept": "application/vnd.github+json"}
if os.environ.get("GITHUB_TOKEN"):
	headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
for attempt in range(4):
	try:
		rel = json.load(urllib.request.urlopen(urllib.request.Request(api, headers=headers), timeout=60))
		break
	except urllib.error.HTTPError as e:
		if e.code == 404:
			print("no release yet")
			sys.exit(0)
		if e.code not in (403, 429) or attempt == 3:
			if e.code in (403, 429):
				print("GitHub API rate limit: set GITHUB_TOKEN in the Pages project (site/cloudflare-build.sh)")
			raise
		print("GitHub API %d, retrying" % e.code)
		time.sleep(10 * (attempt + 1))
for a in rel["assets"]:
	if a["name"].startswith("rfoundry-"):
		urllib.request.urlretrieve(a["browser_download_url"], os.path.join(sys.argv[1], a["name"]))
		print("release asset", a["name"])
EOF
python3 site/build.py --strict --out build/site --firmware build/dist --ref "${CF_PAGES_COMMIT_SHA:-main}"
