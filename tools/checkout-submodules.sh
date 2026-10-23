#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Checks out third_party's submodules for a CI job (docs/dev/ci.md "Submodules").
#
# The self-hosted runner image keeps bare clones of them under $RFOUNDRY_GIT_CACHE (tools/ci-runner/Dockerfile), keyed by
# URL. A submodule with a clone there borrows its objects (git --reference), so only commits newer than the image
# come over the network. Without the cache (GitHub's hosted runners, a laptop), or for a submodule it lacks, this is
# a plain `git submodule update --init`. Each submodule gets three tries: a dropped TLS connection to GitHub used to
# fail the whole job.
#
# Nested submodules (FreeRTOS-Kernel's partner and community ports) are left out: no build uses them.
set -eu
cd "$(git rev-parse --show-toplevel)"
cache=${RFOUNDRY_GIT_CACHE:-}

git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | while read -r key path; do
	name=${key#submodule.}
	name=${name%.path}
	url=$(git config -f .gitmodules "submodule.$name.url")
	set --
	ref="$cache/$(printf '%s\n' "$url" | sed -E 's#^[a-z]+://##; s#\.git$##').git"
	if [ -n "$cache" ] && [ -d "$ref" ]; then
		set -- --reference "$ref"
		echo "$path: from $ref, the rest from $url"
	fi
	try=1
	until git submodule update --init "$@" -- "$path"; do
		[ "$try" -lt 3 ] || { echo "$path: failed $try times" >&2; exit 1; }
		echo "$path: try $try failed, retrying" >&2
		sleep $((try * 10))
		try=$((try + 1))
	done
done
