#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# HIL-tests a batch of pull requests together (tests/hil/README.md, "Batch runs"): one combined tree, one flash and
# one --affected run per radio the batch reaches, and a culprit search when a test fails.
#
#   tools/hil_batch.sh [--run] [--comment] [--keep] [--radio BOARD[=PORT]]... [--jobs N] PR... [-- PYTEST_ARGS...]
#
#   1. fetches main and each PR's head from $RFOUNDRY_HIL_REMOTE (default origin) and merges them in order onto main in
#      a temporary worktree (build/hil-batch/<run>/tree); a PR that doesn't merge stops the batch
#   2. picks the radios: the bench's (--radio, else $RFOUNDRY_HIL_RADIOS "board=port ...", else $RFOUNDRY_HIL_BOARD with
#      $RFOUNDRY_PORT) that the combined change reaches (tools/affected.py boards)
#   3. per radio: builds it, then under the radio's lock (the file tools/bringup.py run takes) flashes it
#      (tools/hil_flash.sh: the site's $RFOUNDRY_HIL_FLASH_CMD) and runs
#      pytest tests/hil --hil-board BOARD --affected main PYTEST_ARGS, recorded in build/hil-batch/<run>/BOARD.json
#   4. when tests fail and the batch has more than one PR: per PR, main plus that PR alone is built, flashed and the
#      failed tests run again, so each failure is put down to the PRs that fail it alone (none: an interaction)
#
# Without --run it merges and prints the commands of steps 3 and 4: nothing is built, flashed or run. --comment (with
# --run) posts each PR's results as a comment (gh pr comment). --keep leaves the worktrees. PYTEST_ARGS are the run's
# opt-ins, e.g. -- --allow-rf; the rig's values come from its hil.toml ([board.<id>]). The tests lease the SDR bench
# themselves (README.md, "The bench board"). $PYTHON is the Python with pytest (default python3). Runs on bash 3.2.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PY=${PYTHON:-python3}
export PYTHON=$PY
REMOTE=${RFOUNDRY_HIL_REMOTE:-origin}
RUN=0 COMMENT=0 KEEP=0 JOBS=${JOBS:-8}
PRS=() RADIOS=() PYARGS=() TREES=()
usage() { sed -n '8p' "$0" | sed 's/^#   //' >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--run) RUN=1; shift ;;
	--comment) COMMENT=1; shift ;;
	--keep) KEEP=1; shift ;;
	--radio) RADIOS+=("$2"); shift 2 ;;
	--jobs) JOBS=$2; shift 2 ;;
	--) shift; PYARGS=("$@"); break ;;
	-h|--help) usage ;;
	[0-9]*) PRS+=("$1"); shift ;;
	*) echo "hil_batch: unknown argument $1" >&2; usage ;;
	esac
done
[ ${#PRS[@]} -gt 0 ] || usage
if [ ${#RADIOS[@]} -eq 0 ]; then
	if [ -n "${RFOUNDRY_HIL_RADIOS:-}" ]; then read -r -a RADIOS <<< "$RFOUNDRY_HIL_RADIOS"
	elif [ -n "${RFOUNDRY_HIL_BOARD:-}" ]; then RADIOS=("$RFOUNDRY_HIL_BOARD=${RFOUNDRY_PORT:-}")
	else echo "hil_batch: no radio: give --radio BOARD[=PORT] or set RFOUNDRY_HIL_RADIOS" >&2; exit 2; fi
fi

OUT=$ROOT/build/hil-batch/$(date -u +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
# shellcheck disable=SC2329  # the EXIT trap runs it
cleanup() {
	[ "$KEEP" = 1 ] && return
	local t
	for t in ${TREES[@]+"${TREES[@]}"}; do git worktree remove --force "$t" 2> /dev/null || true; done
}
trap cleanup EXIT
say() { echo "hil_batch: $*" >&2; }
show() { printf ' %q' "$@"; echo; }

# the radio lock: $RFOUNDRY_RADIO_LOCK with {port} replaced, else ~/.rfoundry-radio-<port>.lock (tools/bringup.py run)
lock_file() {
	local slug
	slug=$(printf '%s' "${1:-${RFOUNDRY_PORT:-usb}}" | sed -E 's/[^A-Za-z0-9.-]+/_/g; s/^[_.]+//; s/[_.]+$//')
	slug=${slug:-usb}
	if [ -n "${RFOUNDRY_RADIO_LOCK:-}" ]; then printf '%s\n' "${RFOUNDRY_RADIO_LOCK//\{port\}/$slug}"
	else printf '%s\n' "$HOME/.rfoundry-radio-$slug.lock"; fi
}
locked() {
	local lock=$1; shift
	if command -v lockf > /dev/null; then lockf -k "$lock" "$@"
	elif command -v flock > /dev/null; then flock "$lock" "$@"
	else say "neither lockf nor flock: no radio lock"; return 1; fi
}

# 1. main and the PRs (each PR's head in $OUT/sha-<n>)
git fetch -q --no-recurse-submodules "$REMOTE" main
BASE=$(git rev-parse FETCH_HEAD)
for n in "${PRS[@]}"; do
	git fetch -q --no-recurse-submodules "$REMOTE" "pull/$n/head"
	git rev-parse FETCH_HEAD > "$OUT/sha-$n"
done
# tree DIR PR...: main plus the PRs, merged in order
tree() {
	local dir=$1 n; shift
	git worktree add -q --detach "$dir" "$BASE"
	TREES+=("$dir")
	for n in "$@"; do
		if ! git -C "$dir" -c user.name=hil-batch -c user.email=hil-batch@localhost merge -q --no-ff --no-edit \
			-m "hil batch: #$n" "$(cat "$OUT/sha-$n")" > /dev/null; then
			git -C "$dir" merge --abort || true
			say "#$n does not merge onto main and the PRs before it: stopped"
			exit 1
		fi
	done
}
TREE=$OUT/tree
tree "$TREE" "${PRS[@]}"
HEAD=$(git -C "$TREE" rev-parse HEAD)
say "main ${BASE:0:8} + ${PRS[*]/#/#} = ${HEAD:0:8} in $TREE"

# 2. the radios the change reaches
REACHED=" $("$PY" tools/affected.py boards --base "$BASE" --head "$HEAD" | tr '\n' ' ') "
PICK=()
for r in "${RADIOS[@]}"; do
	case $REACHED in *" ${r%%=*} "*) PICK+=("$r") ;; esac
done
if [ ${#PICK[@]} -eq 0 ]; then
	say "the batch reaches none of the bench's radios (${RADIOS[*]%%=*}; it reaches:${REACHED:- none})"
	exit 0
fi
say "radios: ${PICK[*]%%=*}"

# submodules DIR: third_party for a tree, borrowing the objects of this checkout's copies (no download when they have
# the commits)
submodules() {
	local dir=$1 _ p
	[ -e "$dir/.hil-batch-submodules" ] && return 0
	git -C "$dir" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | while read -r _ p; do
		if [ -e "$ROOT/$p/.git" ]; then git -C "$dir" submodule update -q --init --reference "$ROOT/$p" -- "$p"
		else git -C "$dir" submodule update -q --init -- "$p"; fi
	done
	: > "$dir/.hil-batch-submodules"
}
# build DIR BOARD
build() {
	local cmd=(make -C "$1" -s -j"$JOBS" BOARD="$2" BUILD="$1/build/$2")
	if [ "$RUN" = 1 ]; then submodules "$1" && "${cmd[@]}"; else show "${cmd[@]}"; fi
}
# hil DIR BOARD PORT RECORD PYTEST_ARGS...: flash DIR's build of BOARD and run pytest in DIR, under the radio's lock.
# PYTEST_ARGS name the tests (tests/hil, or the failed tests' ids alone: with the directory too, pytest would run all of
# it, every tier)
hil() {
	local dir=$1 b=$2 port=$3 rec=$4 lock; shift 4
	lock=$(lock_file "$port")
	local flash=("$ROOT/tools/hil_flash.sh" "$b" "$dir/build/$b" "$port")
	local test=("$PY" -m pytest -v -rs -p no:cacheprovider --hil-board "$b" ${port:+--radio-port "$port"}
		--hil-record "$rec" --junitxml "${rec%.json}.xml" "$@")
	if [ "$RUN" != 1 ]; then
		echo "  under the lock $lock:"
		printf '   '; show "${flash[@]}"
		printf '    cd %q &&' "$dir"; show "${test[@]}" --firmware-image IMAGE
		return 0
	fi
	# shellcheck disable=SC2016
	locked "$lock" sh -c 'img=$("$1" "$2" "$3" "$4") && cd "$5" && shift 5 && exec "$@" --firmware-image "$img"' \
		sh "${flash[@]}" "$dir" "${test[@]}"
}
failed_of() {
	[ -f "$1" ] || return 0
	"$PY" -c 'import json, sys
for t in json.load(open(sys.argv[1]))["tests"]:
    if t["outcome"] == "failed": print(t["id"])' "$1"
}
summary_of() {
	[ -f "$1" ] || { echo "no record (the flash or the run stopped first)"; return 0; }
	"$PY" -c 'import collections, json, sys
d = json.load(open(sys.argv[1]))
c = collections.Counter(t["outcome"] for t in d["tests"])
fw = d.get("firmware", {}).get("git", "?")
print(", ".join(f"{n} {k}" for k, n in sorted(c.items())) + f" in {d.get(\"duration_s\", 0):.0f} s, firmware {fw}")' "$1"
}

# 3. the combined run, radio by radio (results in $OUT/<board>.result, culprits in $OUT/<board>.culprit)
status=0
for r in "${PICK[@]}"; do
	b=${r%%=*} port=${r#*=}
	[ "$port" = "$r" ] && port=""
	rec=$OUT/$b.json
	say "$b: build, flash, test (--affected main)"
	build "$TREE" "$b"
	rc=0
	hil "$TREE" "$b" "$port" "$rec" tests/hil --affected "$BASE" ${PYARGS[@]+"${PYARGS[@]}"} || rc=$?
	if [ "$RUN" != 1 ]; then
		[ ${#PRS[@]} -lt 2 ] || echo "  on a failure, per PR: main + the PR alone in $OUT/pr<n>, built, flashed and" \
			"the failed tests run again"
		continue
	fi
	summary_of "$rec" > "$OUT/$b.result"
	say "$b: $(cat "$OUT/$b.result")"
	[ "$rc" = 0 ] && continue
	status=1
	FAILED=()
	while IFS= read -r t; do FAILED+=("$t"); done < <(failed_of "$rec")
	if [ ${#PRS[@]} -lt 2 ] || [ ${#FAILED[@]} -eq 0 ]; then
		echo "pytest exit $rc${FAILED[0]+: ${FAILED[*]}}" > "$OUT/$b.culprit"
		continue
	fi
	# 4. which PR fails them alone
	: > "$OUT/$b.culprit"
	for n in "${PRS[@]}"; do
		dir=$OUT/pr$n
		[ -d "$dir" ] || tree "$dir" "$n"
		say "$b: the ${#FAILED[@]} failed tests on main + #$n"
		build "$dir" "$b"
		hil "$dir" "$b" "$port" "$OUT/$b-pr$n.json" "${FAILED[@]}" ${PYARGS[@]+"${PYARGS[@]}"} || true
		alone=$(failed_of "$OUT/$b-pr$n.json" | tr '\n' ' ')
		[ -z "$alone" ] || echo "#$n alone fails $alone" >> "$OUT/$b.culprit"
	done
	[ -s "$OUT/$b.culprit" ] || echo "no PR fails them alone: an interaction between the PRs" > "$OUT/$b.culprit"
	say "$b: $(tr '\n' ';' < "$OUT/$b.culprit")"
	say "$b: the radio runs main + #${PRS[${#PRS[@]}-1]} alone now; flash the build you need before other work"
done
[ "$RUN" = 1 ] || { say "dry run: nothing built, flashed or run (--run does)"; exit 0; }

# results per PR
for n in "${PRS[@]}"; do
	body=$OUT/comment-$n.md
	{
		echo "HIL batch: main ${BASE:0:8} with ${PRS[*]/#/#} merged in order (${HEAD:0:8}), \`--affected\` per radio."
		echo
		for r in "${PICK[@]}"; do
			b=${r%%=*}
			line="- **$b**: $(cat "$OUT/$b.result" 2> /dev/null || echo "not run")"
			[ -s "$OUT/$b.culprit" ] && line="$line. Failures: $(tr '\n' ';' < "$OUT/$b.culprit")"
			echo "$line"
		done
	} > "$body"
	if [ "$COMMENT" = 1 ]; then gh pr comment "$n" --body-file "$body"; fi
done
say "records and comments in $OUT"
exit $status
