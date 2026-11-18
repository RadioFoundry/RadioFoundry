#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Where a firmware build came from (docs/spec/support-info.md 1): the Makefile's RFOUNDRY_GIT_SOURCE and
RFOUNDRY_GIT_REPO, which About and the debug link show. It tells a support request about one of our builds from a
fork's, and is a hint only: anyone can set it. The image hash (support-info.md 3) is the proof.

  official   our CI (GitHub Actions in our repository, not a pull request): the images the web site installs
  service    our build service (it passes make BUILD_SOURCE=service, which reaches here as --source service)
  pr         our CI building a pull request
  local      a build from a clone whose origin is our repository
  fork       a build from another repository, in its CI or a clone of it
  unknown    no git, no origin remote

  tools/build_source.py [--source KIND] [--repo OWNER/NAME]   prints "KIND REPO" (REPO may be empty)
  tools/build_source.py --self-test

The repository is GitHub's owner/name (GITHUB_REPOSITORY in CI, else the origin remote's URL), or host/path for
another host, never with credentials. Only letters, digits, '.', '_', '-' and '/' are kept, so the Makefile can put
it in a C string.
"""
import os
import re
import subprocess
import sys

UPSTREAM = "radiofoundry/radiofoundry"
KINDS = ("official", "service", "pr", "local", "fork", "unknown")
REPO_MAX = 48


def clean(repo):
    return re.sub(r"[^A-Za-z0-9._/-]", "_", repo or "")[:REPO_MAX]


def repo_from_url(url):
    """owner/name of a GitHub remote, host/path of another, "" for none. Credentials and .git are dropped."""
    url = (url or "").strip()
    if not url:
        return ""
    m = re.match(r"^[A-Za-z][A-Za-z0-9+.-]*://(?:[^@/]*@)?([^/:]+)(?::\d+)?/(.*)$", url)
    if m is None:
        m = re.match(r"^(?:[^@/]+@)?([^/:]+):(.*)$", url)	# scp-like: git@github.com:owner/name.git
    if m is None:
        return ""
    host, path = m.group(1).lower(), m.group(2).strip("/")
    if path.endswith(".git"):
        path = path[:-4]
    if not path:
        return ""
    return clean(path if host == "github.com" else host + "/" + path)


def source(env, origin_url):
    """(kind, repo) for a build with this environment and origin remote URL"""
    if env.get("GITHUB_ACTIONS") == "true" and env.get("GITHUB_REPOSITORY"):
        repo = clean(env["GITHUB_REPOSITORY"])
        if repo.lower() != UPSTREAM:
            return "fork", repo
        return ("pr" if env.get("GITHUB_EVENT_NAME", "").startswith("pull_request") else "official"), repo
    repo = repo_from_url(origin_url)
    if not repo:
        return "unknown", ""
    return ("local" if repo.lower() == UPSTREAM else "fork"), repo


def origin_url():
    try:
        r = subprocess.run(["git", "remote", "get-url", "origin"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ""
    return r.stdout.strip() if r.returncode == 0 else ""


def self_test():
    ok = True

    def check(got, want, what):
        nonlocal ok
        if got != want:
            print("FAIL %s: %r, want %r" % (what, got, want))
            ok = False

    check(repo_from_url("https://github.com/RadioFoundry/RadioFoundry.git"), "RadioFoundry/RadioFoundry", "https")
    check(repo_from_url("https://user:ghp_secret@github.com/someone/radiofoundry"), "someone/radiofoundry", "credentials")
    check(repo_from_url("git@github.com:someone/fork.git"), "someone/fork", "scp-like")
    check(repo_from_url("ssh://git@gitea.example:2222/team/rf.git"), "gitea.example/team/rf", "other host")
    check(repo_from_url(""), "", "none")
    check(repo_from_url("/srv/git/rf"), "", "a local path")
    check(clean("a b\"c"), "a_b_c", "clean")
    up = "https://github.com/RadioFoundry/RadioFoundry"
    check(source({}, up), ("local", "RadioFoundry/RadioFoundry"), "local clone")
    check(source({}, "git@github.com:someone/rf.git"), ("fork", "someone/rf"), "fork clone")
    check(source({}, ""), ("unknown", ""), "no remote")
    ci = {"GITHUB_ACTIONS": "true", "GITHUB_REPOSITORY": "RadioFoundry/RadioFoundry", "GITHUB_EVENT_NAME": "push"}
    check(source(ci, "")[0], "official", "our CI")
    check(source(dict(ci, GITHUB_EVENT_NAME="pull_request"), "")[0], "pr", "a pull request")
    check(source(dict(ci, GITHUB_REPOSITORY="someone/rf"), up), ("fork", "someone/rf"), "a fork's CI")
    print("build_source self-test: %s" % ("passed" if ok else "FAILED"))
    return ok


def main(argv):
    if argv[:1] == ["--self-test"]:
        return 0 if self_test() else 1
    kind = repo = None
    while argv:
        if argv[0] == "--source" and len(argv) > 1:
            kind, argv = argv[1], argv[2:]
        elif argv[0] == "--repo" and len(argv) > 1:
            repo, argv = clean(argv[1]), argv[2:]
        else:
            print("usage: build_source.py [--source KIND] [--repo OWNER/NAME] | --self-test", file=sys.stderr)
            return 2
    if kind is not None and kind not in KINDS:
        print("build_source.py: BUILD_SOURCE=%s is not one of %s" % (kind, ", ".join(KINDS)), file=sys.stderr)
        return 2
    found_kind, found_repo = source(os.environ, origin_url())
    print("%s %s" % (kind or found_kind, found_repo if repo is None else repo))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
