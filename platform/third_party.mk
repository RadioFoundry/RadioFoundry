# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Where the third_party/ submodules are read from (docs/dev/building.md "Worktrees").
#
# A clone with its submodules checked out builds from its own third_party/, as always. A git worktree can leave them
# uninitialized (empty directories, a clean git status) and build from another checkout's: set
# RFOUNDRY_THIRD_PARTY=<that checkout>/third_party, an absolute path. The tree's own copies win whenever they are
# there, so a branch that moves a submodule checks out its own (git submodule update --init) and builds from those.
#
# In: TP_ROOT, the top of the source tree as the including Makefile sees it (empty: the current directory).
# Out: THIRD_PARTY, the directory to read; TP_PREFIX_MAP, compiler flags that record its files as third_party/... in
# debug info and __FILE__, so an image built from a shared copy is the same as one built from the tree's own;
# TP_ERROR, why a build can't start, empty when it can.
TP_LOCAL := $(if $(TP_ROOT),$(TP_ROOT)/)third_party
# git leaves an empty directory for each submodule it hasn't checked out, so the directories name them all
TP_MODULES := $(notdir $(patsubst %/,%,$(wildcard $(TP_LOCAL)/*/)))
# A submodule is there when its directory holds files (a symlink to another checkout's copy counts too)
TP_MISSING = $(strip $(foreach m,$(TP_MODULES),$(if $(wildcard $(1)/$(m)/*),,$(m))))
TP_LOCAL_MISSING := $(call TP_MISSING,$(TP_LOCAL))
# The shared copies must be the commits this tree pins: a branch that moves a submodule would otherwise build against
# the old one without a word. Skipped where git can't answer (a tarball, a container without the other checkout's .git)
TP_PIN_MISMATCH = $(strip $(shell cd $(or $(TP_ROOT),.) && git ls-tree HEAD third_party/ 2>/dev/null | \
	while read -r mode type sha p; do m=$${p\#third_party/}; [ "$$mode" = 160000 ] || continue; \
	have=$$(git -C "$(THIRD_PARTY)/$$m" rev-parse -q --verify HEAD 2>/dev/null) || continue; \
	[ "$$have" = "$$sha" ] || printf '%s ' "$$m"; done))

# TP_ERROR stops a build. Recursively expanded, so only a Makefile about to compile pays for the pin check or fails on
# a missing submodule: the query targets (make print-vars) run without submodules for the site and the host tests
TP_SHARED_MISSING = $(call TP_MISSING,$(THIRD_PARTY))
ifeq ($(TP_LOCAL_MISSING),)
THIRD_PARTY := $(TP_LOCAL)
TP_PREFIX_MAP :=
TP_ERROR =
else ifneq ($(RFOUNDRY_THIRD_PARTY),)
THIRD_PARTY := $(abspath $(RFOUNDRY_THIRD_PARTY))
TP_PREFIX_MAP := -ffile-prefix-map=$(THIRD_PARTY)/=third_party/
TP_SHARED_ERROR = RFOUNDRY_THIRD_PARTY=$(THIRD_PARTY) lacks $(TP_SHARED_MISSING): point it at a checkout's \
	third_party/ with its submodules checked out
TP_PIN_ERROR = RFOUNDRY_THIRD_PARTY=$(THIRD_PARTY) has other commits than this tree pins ($(TP_PIN_MISMATCH)): \
	run git submodule update --init here, or update that checkout
TP_ERROR = $(if $(TP_SHARED_MISSING),$(TP_SHARED_ERROR),$(if $(TP_PIN_MISMATCH),$(TP_PIN_ERROR)))
else
THIRD_PARTY := $(TP_LOCAL)
TP_PREFIX_MAP :=
TP_ERROR = third_party/ is not checked out ($(TP_LOCAL_MISSING)): run git submodule update --init, or set \
	RFOUNDRY_THIRD_PARTY to another checkout's third_party/ (docs/dev/building.md "Worktrees")
endif
