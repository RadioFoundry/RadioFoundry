#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The runner's job-started hook (ACTIONS_RUNNER_HOOK_JOB_STARTED, README.md "Which jobs it takes"). The runner runs
# it before the first step of every job; a nonzero exit fails the job before anything from the workflow runs. It
# takes a job only when the workflow file is the default branch's copy (or a release tag's) in this repository:
#
#   push to main, schedule, workflow_dispatch on main   <repo>/.github/workflows/<file>@refs/heads/main
#   pull_request_target, workflow_run                   the same: they always run main's copy
#   a v* tag                                            <repo>/.github/workflows/<file>@refs/tags/v...
#
# Everything else is refused, above all a pull_request run (@refs/pull/N/merge), whose workflow file is the pull
# request's own: a fork could otherwise name this runner's label and run anything here. It is what a runner group
# restricted to selected workflows does, enforced on the runner, where a workflow can't change it.
#
# A failed hook alone isn't enough: the runner still runs the job's `if: always()` steps after it. So a refusal also
# kills every process of the runner's user in the container (the runner, its worker, this script): no later step
# runs, the container ends, and runner-loop.sh starts a fresh one. GitHub marks the job failed once it notices the
# runner is gone. The kill happens only where the image has /usr/local/lib/rfoundry/job-gate.kill, so the tests can run
# the gate anywhere. /usr/bin/kill by its path: an imported function's name can't contain a slash.
#
# The decision reads GITHUB_* variables, which a workflow can't set, and uses only [[ ]] tests: no command, builtin
# or function decides, so nothing a job defines can stand in for one. printf only writes the message; the script's
# status is the last line's test. job-gate-test.sh checks the cases (tools/ci.sh runs it).

repo=${RFOUNDRY_CI_REPO-}
ref=${GITHUB_WORKFLOW_REF-}
event=${GITHUB_EVENT_NAME-}
wf="^${repo//./\\.}/\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml@refs/"
ok=

[[ -n $repo && $GITHUB_REPOSITORY == "$repo" ]] && {
	[[ $event =~ ^(push|schedule|workflow_dispatch|pull_request_target|workflow_run)$ && $ref =~ ${wf}heads/main$ ]] && ok=1
	[[ $event == push && $ref =~ ${wf}tags/v[0-9][A-Za-z0-9._-]*$ ]] && ok=1
}
# the message, then the kill; the status is the test on the last line
[[ -n $ok ]] || {
	printf 'rfoundry job gate: refused the %s run of %s (only %s workflows from main or a v* tag run here)\n' \
		"$event" "$ref" "$repo" >&2
	[[ -e /usr/local/lib/rfoundry/job-gate.kill ]] && /usr/bin/kill -KILL -1
}
[[ -n $ok ]]
