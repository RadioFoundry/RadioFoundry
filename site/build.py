#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Builds the RadioFoundry web site from the repository (docs/dev/website.md).

    python3 site/build.py                         build/site/
    python3 site/build.py --out DIR --strict      fail on any warning (CI)
    python3 site/build.py --firmware build/dist   include release images for the web flasher
    python3 site/build.py --serve                 build, then serve on http://localhost:8000 (WebUSB needs
                                                  localhost or https)

Needs markdown-it-py (site/requirements.txt). Without it installed:
    uv run --no-project --with-requirements site/requirements.txt python3 site/build.py
"""
import argparse
import functools
import http.server
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rfoundrysite import repo  # noqa: E402

DEFAULT_REPO = "https://github.com/radiofoundry/radiofoundry"


def main(argv=None):
	ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	ap.add_argument("--out", default=repo.path("build", "site"))
	ap.add_argument("--repo-url", default=os.environ.get("RFOUNDRY_REPO_URL", DEFAULT_REPO),
			help="the repository's web address, for links to files outside docs/")
	ap.add_argument("--ref", default=None, help="branch or tag those links point at (default: main)")
	ap.add_argument("--firmware", help="a directory of release images and manifests (tools/release.py --out)")
	ap.add_argument("--beta", default="", help="a pre-release newer than those images, offered as an opt-in version link")
	ap.add_argument("--strict", action="store_true", help="exit 1 when the build warns")
	ap.add_argument("--serve", action="store_true", help="serve the result on localhost")
	ap.add_argument("--host", default="localhost", help="address for --serve (default: localhost)")
	ap.add_argument("--port", type=int, default=8000)
	args = ap.parse_args(argv)
	try:
		import markdown_it, resvg_py  # noqa: F401
	except ImportError:
		print("site/build.py: markdown-it-py or resvg_py is missing. pip install -r site/requirements.txt, or run:\n"
		      "  uv run --no-project --with-requirements site/requirements.txt python3 site/build.py", file=sys.stderr)
		return 2
	from rfoundrysite.build import Site
	site = Site(args.out, args.repo_url, args.ref or "main", firmware=args.firmware, beta=args.beta)
	try:
		warnings = site.build()
	except repo.SourceError as e:
		print("site/build.py: %s" % e, file=sys.stderr)
		return 1
	print("site: %s (%d warning%s)" % (args.out, len(warnings), "" if len(warnings) == 1 else "s"))
	if warnings and args.strict:
		return 1
	if args.serve:
		class SiteHandler(http.server.SimpleHTTPRequestHandler):
			def do_GET(self):
				if self.path.partition("?")[0] != "/api/amsat-tle":
					return super().do_GET()
				try:
					request = urllib.request.Request("https://www.amsat.org/tle/daily-bulletin.txt",
										 headers={"Accept": "text/plain", "User-Agent": "RadioFoundry site TLE updater"})
					with urllib.request.urlopen(request, timeout=20) as response:
						body = response.read(1000001)
						if len(body) > 1000000:
							self.send_error(502, "AMSAT bulletin exceeds the size limit")
							return
				except Exception:
					self.send_error(502, "AMSAT bulletin is temporarily unavailable")
					return
				self.send_response(200)
				self.send_header("Content-Type", "text/plain; charset=utf-8")
				self.send_header("Cache-Control", "public, max-age=3600")
				self.send_header("Content-Length", str(len(body)))
				self.end_headers()
				self.wfile.write(body)

		handler = functools.partial(SiteHandler, directory=os.path.abspath(args.out))
		print("serving http://%s:%d/ (Ctrl-C stops)" % (args.host, args.port))
		try:
			http.server.ThreadingHTTPServer((args.host, args.port), handler).serve_forever()
		except KeyboardInterrupt:
			pass
	return 0


if __name__ == "__main__":
	sys.exit(main())
