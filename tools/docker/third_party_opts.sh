# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Sourced by the container run scripts: tp_opts ROOT prints the container options that let a build of ROOT (mounted at
# its own path) read RFOUNDRY_THIRD_PARTY (platform/third_party.mk), nothing when it isn't set. It mounts the checkout
# that holds it and the git directories the pin check reads: that checkout's (a worktree's submodules live in its
# main checkout's .git) and the tree's own (git ls-tree HEAD). Paths under ROOT need no mount: there the variable is
# given as ROOT sees it, which may go through a symlink. No spaces in these paths.

# shellcheck disable=SC2039,SC3043
tp_opts() {
	[ -n "${RFOUNDRY_THIRD_PARTY:-}" ] || return 0
	local root="$1" root_p tp top mounts p m out
	root_p=$(cd "$root" && pwd -P)
	tp=$(cd "$RFOUNDRY_THIRD_PARTY" 2>/dev/null && pwd -P) || return 0
	top=$(git -C "$tp" rev-parse --show-toplevel 2>/dev/null || echo "$tp")
	case "$tp/" in
	"$root_p"/*) out="-e RFOUNDRY_THIRD_PARTY=$root${tp#"$root_p"}" ;;
	*) out="-e RFOUNDRY_THIRD_PARTY=$tp" ;;
	esac
	mounts=$root_p
	for p in "$top" "$(git -C "$top" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" \
		"$(git -C "$root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"; do
		[ -n "$p" ] && [ -d "$p" ] || continue
		p=$(cd "$p" && pwd -P)
		case " $mounts " in *" $p "*) continue ;; esac
		# under a directory already mounted: nothing to add
		for m in $mounts; do case "$p/" in "$m"/*) p=; break ;; esac; done
		[ -n "$p" ] || continue
		mounts="$mounts $p"
		out="$out -v $p:$p"
	done
	echo "$out"
}
