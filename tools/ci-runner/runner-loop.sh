#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Host side of the self-hosted runner (README.md). Subcommands:
#
#   runner-loop.sh setup       create the internal network, the egress network and the proxy container (idempotent)
#   runner-loop.sh register    one-time registration, with a registration token on stdin (never on a command line)
#   runner-loop.sh             forever: one fresh container per job (`run.sh --once`, then `--rm`), repeat
#
# Settings come from runner.env next to this script ($ODR_CI_ENV overrides). Cron starts the loop at boot and every
# few minutes; flock keeps one. Touch $BASE/disabled to stop starting new containers (a running job finishes).
set -u
BASE=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1090
. "${ODR_CI_ENV:-$BASE/runner.env}"
: "${REPO:?}" "${IMAGE:?}" "${PROXY_IMAGE:?}" "${WORK_DIR:?}" "${STATE_DIR:?}" "${SUBNET:?}" "${PROXY_IP:?}"
NAME=${NAME:-odr-ci-1}
LABELS=${LABELS:-odr-ci}
NETWORK=${NETWORK:-odr-ci-internal}
EGRESS_NETWORK=${EGRESS_NETWORK:-odr-ci-egress}
PROXY_NAME=${PROXY_NAME:-odr-ci-proxy}
CPUS=${CPUS:-4}
MEMORY=${MEMORY:-4g}
PIDS=${PIDS:-4096}
PROXY="http://$PROXY_IP:8888"
LOG="$BASE/log/runner.log"
mkdir -p "$WORK_DIR" "$BASE/log"

log() { echo "$(date -u +%FT%TZ) $*" >> "$LOG"; }

# The job container: no capabilities, no host network or socket, only the internal network (no route out, no DNS
# beyond Docker's for container names) with the proxy as the way to GitHub. $1: the mode, $2: extra mount option.
runner() {
	# `docker stop` must reach the runner as SIGINT, so it closes its session on GitHub: a killed runner leaves one
	# behind, and the next container waits up to ten minutes for it to expire before it gets jobs. --init runs tini,
	# which forwards the stop signal to the whole process group (run.sh doesn't pass it on).
	docker run --rm -i --init -e TINI_KILL_PROCESS_GROUP=1 --stop-signal INT --stop-timeout 60 --name "$NAME" \
		--cpus "$CPUS" --memory "$MEMORY" --memory-swap "$MEMORY" --pids-limit "$PIDS" \
		--cap-drop ALL --security-opt no-new-privileges \
		--network "$NETWORK" \
		-e "https_proxy=$PROXY" -e "http_proxy=$PROXY" -e "HTTPS_PROXY=$PROXY" -e "HTTP_PROXY=$PROXY" \
		-e no_proxy= -e NO_PROXY= \
		-e "ODR_CI_REPO=$REPO" -e "ODR_CI_NAME=$NAME" -e "ODR_CI_LABELS=$LABELS" \
		--mount "type=bind,src=$WORK_DIR,dst=/work" \
		--mount "type=bind,src=$STATE_DIR,dst=/state$2" \
		"$IMAGE" "$1"
}

case "${1:-loop}" in
setup)
	set -e
	docker network inspect "$NETWORK" > /dev/null 2>&1 ||
		docker network create --internal --subnet "$SUBNET" "$NETWORK"
	docker network inspect "$EGRESS_NETWORK" > /dev/null 2>&1 || docker network create "$EGRESS_NETWORK"
	if ! docker container inspect "$PROXY_NAME" > /dev/null 2>&1; then
		docker create --name "$PROXY_NAME" --restart unless-stopped \
			--cpus 0.5 --memory 64m --memory-swap 64m --pids-limit 64 \
			--cap-drop ALL --security-opt no-new-privileges --read-only \
			--network "$NETWORK" --ip "$PROXY_IP" "$PROXY_IMAGE" > /dev/null
		docker network connect "$EGRESS_NETWORK" "$PROXY_NAME"
	fi
	docker start "$PROXY_NAME" > /dev/null
	echo "setup: $NETWORK (internal, $SUBNET), $EGRESS_NETWORK, $PROXY_NAME at $PROXY"
	;;
register)
	# the token comes on stdin and goes on to the container's stdin
	mkdir -p "$STATE_DIR" && chmod 700 "$STATE_DIR"
	runner register ""
	;;
loop)
	log "loop started"
	while :; do
		if [ -e "$BASE/disabled" ]; then sleep 60; continue; fi
		[ "$(stat -c %s "$LOG" 2> /dev/null || echo 0)" -lt 5000000 ] || mv -f "$LOG" "$LOG.1"
		if [ ! -s "$STATE_DIR/.runner" ]; then
			log "not registered (runner-loop.sh register)"
			sleep 300
			continue
		fi
		start=$(date +%s)
		runner run ",readonly" < /dev/null >> "$LOG" 2>&1
		rc=$?
		log "runner exited ($rc) after $(($(date +%s) - start)) s"
		# a container that fails at once (no image, no network) must not spin
		[ $(($(date +%s) - start)) -ge 30 ] || sleep 30
	done
	;;
*)
	echo "usage: runner-loop.sh [setup|register]" >&2
	exit 2
	;;
esac
