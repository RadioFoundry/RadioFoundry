#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Git worktrees that read third_party/ from another checkout (docs/dev/building.md "Worktrees").
#
#   tools/worktree.sh add <path> [git worktree add options and commit]
#                           a new worktree with its submodules left uninitialized, set up as below
#                           (e.g. tools/worktree.sh add ../rf-topic -b topic origin/main)
#   tools/worktree.sh setup the current worktree: submodules not checked out stay uninitialized (symlinks to another
#                           checkout's become empty directories again; the links go, not what they point to)
#   tools/worktree.sh own <name>...
#                           check out these submodules in the current worktree (a branch that moves one); the
#                           objects come from the main checkout's clone where it has one (git --reference)
#
# setup marks each submodule not checked out inactive in this worktree only (git config --worktree). Without that,
# submodule.recurse=true makes git checkout and git reset try to update the submodules the main checkout marks active,
# and they leave a .git file in the empty directory that breaks every git command there. The builds read
# RFOUNDRY_THIRD_PARTY (platform/third_party.mk): set it once, to a full checkout's third_party/.
set -eu

die() { echo "worktree: $*" >&2; exit 1; }

# The submodule paths, from .gitmodules
paths() { git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | while read -r _key p; do echo "$p"; done; }

setup() {
	cd "$(git rev-parse --show-toplevel)"
	[ "$(git rev-parse --git-dir)" != "$(git rev-parse --git-common-dir)" ] ||
		die "this is the main checkout: it keeps its own submodules"
	# git config --worktree needs the extension; harmless to the other worktrees
	[ "$(git config --bool extensions.worktreeConfig || echo false)" = true ] || git config extensions.worktreeConfig true
	for p in $(paths); do
		if [ -L "$p" ]; then
			rm "$p" && mkdir "$p"
			git update-index --no-skip-worktree "$p" 2>/dev/null || true
		fi
		if [ -e "$p/.git" ]; then
			echo "$p: checked out here, read from this tree"
		else
			git config --worktree "submodule.$p.active" false
			echo "$p: from RFOUNDRY_THIRD_PARTY"
		fi
	done
	[ -n "${RFOUNDRY_THIRD_PARTY:-}" ] ||
		echo "worktree: set RFOUNDRY_THIRD_PARTY to a full checkout's third_party/ (once, in the shell profile)"
}

[ $# -ge 1 ] || { sed -n '5,15p' "$0"; exit 2; }
cmd=$1; shift
case $cmd in
add)
	[ $# -ge 1 ] || die "add <path> [git worktree add options and commit]"
	dir=$1; shift
	git worktree add "$dir" "$@"
	(cd "$dir" && setup)
	;;
setup) setup ;;
own)
	[ $# -ge 1 ] || die "own <name>..."
	cd "$(git rev-parse --show-toplevel)"
	for n in "$@"; do
		p=third_party/${n#third_party/}
		git config --worktree --unset "submodule.$p.active" 2>/dev/null || true
		ref=$(git rev-parse --path-format=absolute --git-common-dir)/modules/$p
		if [ -d "$ref" ]; then git submodule update --init --reference "$ref" -- "$p"
		else git submodule update --init -- "$p"; fi
	done
	;;
*) die "unknown command $cmd (add, setup or own)" ;;
esac
