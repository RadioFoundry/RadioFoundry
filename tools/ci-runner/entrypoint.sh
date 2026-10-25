#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The runner container's entrypoint (README.md).
#
#   rfoundry-ci-entrypoint run        one job: the runner's registration from /state (read-only), `run.sh --once`
#   rfoundry-ci-entrypoint register   one-time registration: a registration token on stdin, writes the runner's
#                                configuration and credentials to /state (mounted writable for this only)
#
# runner-loop.sh starts a new container for every job, so each job starts from the image. /work is the only other
# mount; it is emptied first, except /work/.cache (the runner's release tarball, checked against RUNNER_SHA256
# before every use). The runner, $HOME and the job's workspace live there.
set -eu
MODE=${1:-run}
[ "$(id -u)" != 0 ] || { echo "rfoundry-ci-entrypoint: refusing to run as root" >&2; exit 2; }
# the runner's registration files
STATE_FILES=".runner .credentials .credentials_rsaparams"

find /work -mindepth 1 -maxdepth 1 ! -name .cache -exec rm -rf {} + || true
mkdir -p /work/.cache /work/home/opt /work/runner
export HOME=/work/home
# install-arm-toolchain.sh and the Makefile look in ~/opt first: point them at the image's copy
ln -s "/opt/$ARM_GCC_NAME" "$HOME/opt/$ARM_GCC_NAME"

tgz="/work/.cache/actions-runner-linux-x64-$RUNNER_VERSION.tar.gz"
if ! echo "$RUNNER_SHA256  $tgz" | sha256sum -c --status - 2> /dev/null; then
	find /work/.cache -mindepth 1 -delete
	curl -fsSL --retry 3 -o "$tgz" \
		"https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-linux-x64-$RUNNER_VERSION.tar.gz"
	echo "$RUNNER_SHA256  $tgz" | sha256sum -c --status -
fi
tar -xzf "$tgz" -C /work/runner
cd /work/runner

case "$MODE" in
register)
	: "${RFOUNDRY_CI_REPO:?}" "${RFOUNDRY_CI_NAME:?}" "${RFOUNDRY_CI_LABELS:?}"
	IFS= read -r token
	# --disableupdate: every job starts from the image, so an update would be downloaded again for every job;
	# update RUNNER_VERSION in the Dockerfile instead
	./config.sh --unattended --url "https://github.com/$RFOUNDRY_CI_REPO" --token "$token" --name "$RFOUNDRY_CI_NAME" \
		--labels "$RFOUNDRY_CI_LABELS" --work _work --disableupdate --replace
	for f in $STATE_FILES; do cp "$f" /state/; done
	chmod 600 /state/.runner /state/.credentials*
	echo "rfoundry-ci-entrypoint: registered $RFOUNDRY_CI_NAME"
	;;
run)
	for f in $STATE_FILES; do
		[ -f "/state/$f" ] || { echo "rfoundry-ci-entrypoint: /state/$f missing: register first (README.md)" >&2; exit 2; }
		cp "/state/$f" .
	done
	# one job, then exit: runner-loop.sh starts the next container
	exec ./run.sh --once
	;;
*)
	echo "rfoundry-ci-entrypoint: unknown mode $MODE (run, register)" >&2
	exit 2
	;;
esac
