# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Download a release image and its manifest from GitHub (public releases, no token needed).

Assets per board and version (docs/dev/release.md "Release artifacts"): rfoundry-<board>-<version>.bin, .json, and the
release-wide SHA256SUMS. Files go to a cache directory and are checked against the manifest (and SHA256SUMS) before
use.
"""
import json
import os
import sys
import urllib.request

from . import manifest as mf

DEFAULT_REPO = "radiofoundry/radiofoundry"
API = "https://api.github.com/repos/%s/releases/%s"


class ReleaseError(Exception):
    pass


def cache_dir():
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~\\AppData\\Local")
        return os.path.join(base, "rfoundry", "cache")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Caches/rfoundry")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "rfoundry")


def _ssl_context():
    import ssl
    try:
        import certifi	# bundled with the executables; the system store may be missing in a frozen app
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def _get(url, accept="application/octet-stream"):
    req = urllib.request.Request(url, headers={"User-Agent": "rfoundry", "Accept": accept})
    try:
        with urllib.request.urlopen(req, timeout=60, context=_ssl_context()) as r:
            return r.read()
    except Exception as e:
        if getattr(e, "code", None) == 404:
            raise ReleaseError("not found: %s (no such release or file; the repository may also be private)" % url)
        raise ReleaseError("download of %s failed: %s" % (url, e))


def release_info(version, repo=DEFAULT_REPO):
    which = "latest" if version in (None, "latest") else "tags/" + version
    data = _get(API % (repo, which), accept="application/vnd.github+json")
    try:
        return json.loads(data.decode("utf-8"))
    except ValueError:
        raise ReleaseError("unexpected answer from GitHub for %s" % which)


def fetch(board_id, version=None, repo=DEFAULT_REPO, dest=None, log=print):
    """Download (or reuse from the cache) the image and manifest. Returns (image path, manifest path)."""
    info = release_info(version, repo)
    tag = info.get("tag_name")
    if not tag:
        raise ReleaseError("release %s not found in %s" % (version or "latest", repo))
    assets = {a["name"]: a["browser_download_url"] for a in info.get("assets", [])}
    want_bin, want_json = mf.image_name(board_id, tag), mf.image_name(board_id, tag, "json")
    if want_bin not in assets or want_json not in assets:
        boards_there = sorted({n.split("-")[1] for n in assets if n.startswith("rfoundry-") and n.endswith(".json")})
        raise ReleaseError("release %s has no image for %s (it has: %s)"
                           % (tag, board_id, ", ".join(boards_there) or "no firmware images"))
    dest = dest or os.path.join(cache_dir(), tag)
    os.makedirs(dest, exist_ok=True)
    paths = []
    for name in (want_bin, want_json):
        path = os.path.join(dest, name)
        if not os.path.exists(path):
            log("downloading %s" % name)
            data = _get(assets[name])
            with open(path + ".part", "wb") as f:
                f.write(data)
            os.replace(path + ".part", path)
        paths.append(path)
    if "SHA256SUMS" in assets:
        sums = _get(assets["SHA256SUMS"]).decode("ascii", "replace")
        check_sums(sums, paths)
    return paths[0], paths[1]


def check_sums(sums_text, paths):
    """Each file in paths must be listed in SHA256SUMS with its hash."""
    import hashlib
    table = {}
    for line in sums_text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            table[parts[1].lstrip("*")] = parts[0].lower()
    for p in paths:
        name = os.path.basename(p)
        with open(p, "rb") as f:
            got = hashlib.sha256(f.read()).hexdigest()
        if table.get(name) != got:
            raise ReleaseError("%s does not match SHA256SUMS (delete %s and try again)" % (name, p))
