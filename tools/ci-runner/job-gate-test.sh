#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Cases for job-gate.sh, the runner's job-started hook: the jobs it takes and the ones it refuses. tools/ci.sh runs it.
set -u
cd "$(dirname "$0")" || exit 1
R=RadioFoundry/RadioFoundry
W=$R/.github/workflows
fails=0 n=0
# $1 expect (allow|deny), $2 event, $3 workflow ref, $4 repository (default $R), $5 extra environment entry
check() {
	n=$((n + 1))
	if env -i RFOUNDRY_CI_REPO=$R GITHUB_REPOSITORY="${4-$R}" GITHUB_EVENT_NAME="$2" GITHUB_WORKFLOW_REF="$3" \
		${5:+"$5"} bash ./job-gate.sh 2> /dev/null; then got=allow; else got=deny; fi
	[ "$got" = "$1" ] || { echo "job-gate: $2 $3: expected $1, got $got"; fails=$((fails + 1)); }
}
check allow push "$W/ci.yml@refs/heads/main"
check allow schedule "$W/nightly.yml@refs/heads/main"
check allow workflow_dispatch "$W/hil.yml@refs/heads/main"
check allow pull_request_target "$W/pr-tests.yml@refs/heads/main"
check allow workflow_run "$W/ui-gallery-comment.yml@refs/heads/main"
check allow push "$W/release.yml@refs/tags/v1.2.0"
check allow push "$W/release.yaml@refs/tags/v0.0.1-rc1"
# a pull request's own workflow file, whatever it claims to be
check deny pull_request "$W/ui-gallery.yml@refs/pull/12/merge"
check deny pull_request "$W/x.yml@refs/heads/main"
check deny pull_request_review "$W/x.yml@refs/heads/main"
check deny issue_comment "$W/x.yml@refs/heads/main"
# branches other than main, and tags other than releases
check deny push "$W/ci.yml@refs/heads/feature/x"
check deny workflow_dispatch "$W/ci.yml@refs/heads/main2"
check deny workflow_dispatch "$W/ci.yml@refs/heads/x/main"
check deny push "$W/release.yml@refs/tags/test"
check deny workflow_dispatch "$W/release.yml@refs/tags/v1.0"
check deny push "$W/ci.yml@refs/heads/main
x"
check deny push "$W/../x.yml@refs/heads/main"
check deny push "$W/sub/x.yml@refs/heads/main"
# other repositories
check deny push "evil/RadioFoundry/.github/workflows/ci.yml@refs/heads/main" evil/RadioFoundry
check deny push "evil/RadioFoundry/.github/workflows/ci.yml@refs/heads/main"
check deny push "RadioFoundryX/RadioFoundry/.github/workflows/ci.yml@refs/heads/main" RadioFoundryX/RadioFoundry
check deny push "$W/ci.yml@refs/heads/main" ""
# a function named after the message's printf can't change the answer
check deny pull_request "$W/x.yml@refs/pull/1/merge" "$R" "BASH_FUNC_printf%%=() { :; }"
# no repository configured: nothing runs
n=$((n + 1))
env -i GITHUB_REPOSITORY=$R GITHUB_EVENT_NAME=push GITHUB_WORKFLOW_REF="$W/ci.yml@refs/heads/main" bash ./job-gate.sh 2> /dev/null &&
	{ echo "job-gate: no RFOUNDRY_CI_REPO: expected deny, got allow"; fails=$((fails + 1)); }
if [ "$fails" != 0 ]; then
	echo "job-gate: $fails of $n cases fail"
	exit 1
fi
echo "job-gate: $n cases pass"
