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
python3 - build/dist <<'EOF'
import json, os, sys, urllib.error, urllib.request
api = "https://api.github.com/repos/RadioFoundry/RadioFoundry/releases/latest"
try:
	rel = json.load(urllib.request.urlopen(api, timeout=60))
except urllib.error.HTTPError as e:
	if e.code == 404:
		print("no release yet")
		sys.exit(0)
	raise
for a in rel["assets"]:
	if a["name"].startswith("rfoundry-"):
		urllib.request.urlretrieve(a["browser_download_url"], os.path.join(sys.argv[1], a["name"]))
		print("release asset", a["name"])
EOF
python3 site/build.py --strict --out build/site --firmware build/dist --ref "${CF_PAGES_COMMIT_SHA:-main}"
