# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Where each third_party/ submodule is read from (docs/dev/building.md "Worktrees").
#
# A submodule checked out in this tree is read from here, as always. A git worktree can leave its submodules
# uninitialized (empty directories, a clean git status) and read them from another checkout: set
# RFOUNDRY_THIRD_PARTY to that checkout's third_party/, as an absolute path. The choice is per submodule, so a branch
# that moves one checks out only that one (git submodule update --init third_party/<name>).
#
# Makefiles name third-party files by their tree path (third_party/<name>/...). Sources are found through vpath, and
# TP_INCLUDES maps -I flags. $(wildcard) does not search vpath: it sees only this tree's copies.
#
# In: TP_ROOT, the top of the source tree as the including Makefile sees it (empty: the current directory).
# Out: TP_INCLUDES (call TP_INCLUDES,<flags>), the flags with -I third_party/<name>... mapped; TP_PREFIX_MAP, compiler
# flags that record shared files as third_party/... in debug info and __FILE__, so the image matches a build from this
# tree's copies; TP_ERROR, why a build can't start, empty when it can; TP_CHECK, expanded once by the including
# Makefile, which stops make on TP_ERROR unless every goal is a query. The query targets (make print-vars) run without
# submodules for the site and the host tests, and only a build pays for the pin check.
TP_LOCAL := $(if $(TP_ROOT),$(TP_ROOT)/)third_party
TP_SHARED := $(patsubst %/,%,$(RFOUNDRY_THIRD_PARTY))
# git leaves an empty directory for each submodule it hasn't checked out, so the directories name them all
TP_MODULES := $(notdir $(patsubst %/,%,$(wildcard $(TP_LOCAL)/*/)))
# A submodule is there when its directory holds files (a symlink to another checkout's copy counts too)
TP_HAS = $(wildcard $(1)/$(2)/*)
TP_LOCAL_MISSING := $(strip $(foreach m,$(TP_MODULES),$(if $(call TP_HAS,$(TP_LOCAL),$(m)),,$(m))))
TP_FROM_SHARED := $(if $(TP_SHARED),$(TP_LOCAL_MISSING))

# The shared checkout stands in only for the paths under its own third_party/ (vpath searches <dir>/third_party/...)
TP_SHARED_TOP := $(patsubst %/third_party,%,$(TP_SHARED))
$(foreach m,$(TP_FROM_SHARED),$(eval vpath third_party/$(m)/% $(TP_SHARED_TOP)))
TP_PREFIX_MAP := $(foreach m,$(TP_FROM_SHARED),-ffile-prefix-map=$(TP_SHARED)/$(m)/=third_party/$(m)/)
TP_MAP1 = $(patsubst -I$(TP_LOCAL)/$(2),-I$(TP_SHARED)/$(2),$(patsubst -I$(TP_LOCAL)/$(2)/%,-I$(TP_SHARED)/$(2)/%,$(1)))
TP_MAP = $(if $(2),$(call TP_MAP,$(call TP_MAP1,$(1),$(firstword $(2))),$(wordlist 2,99,$(2))),$(1))
TP_INCLUDES = $(call TP_MAP,$(1),$(TP_FROM_SHARED))

# The shared copies must be the commits this tree pins: a branch that moves a submodule would otherwise build against
# the old one without a word. Checked only where the shared copy is a git checkout of its own (a plain copy can't say)
TP_PIN_MISMATCH = $(strip $(shell cd $(or $(TP_ROOT),.) && \
	git ls-tree HEAD $(addprefix third_party/,$(TP_FROM_SHARED)) 2>/dev/null | \
	while read -r mode type sha p; do m=$${p\#third_party/}; [ "$$mode" = 160000 ] || continue; \
	[ -e "$(TP_SHARED)/$$m/.git" ] || continue; \
	have=$$(git -C "$(TP_SHARED)/$$m" rev-parse -q --verify HEAD 2>/dev/null) || continue; \
	[ "$$have" = "$$sha" ] || printf '%s ' "$$m"; done))
TP_SHARED_MISSING = $(strip $(foreach m,$(TP_FROM_SHARED),$(if $(call TP_HAS,$(TP_SHARED),$(m)),,$(m))))

TP_ERR_RELATIVE = RFOUNDRY_THIRD_PARTY=$(RFOUNDRY_THIRD_PARTY) is not an absolute path
TP_ERR_NAME = RFOUNDRY_THIRD_PARTY=$(TP_SHARED) must be a checkout's third_party/ directory
TP_ERR_LOCAL = third_party/ is not checked out ($(TP_LOCAL_MISSING)): run git submodule update --init, or set \
	RFOUNDRY_THIRD_PARTY to another checkout's third_party/ (docs/dev/building.md "Worktrees")
TP_ERR_SHARED = neither this tree nor RFOUNDRY_THIRD_PARTY=$(TP_SHARED) has $(TP_SHARED_MISSING): run \
	git submodule update --init $(addprefix third_party/,$(TP_SHARED_MISSING)) here
TP_ERR_PIN = RFOUNDRY_THIRD_PARTY=$(TP_SHARED) has other commits than this tree pins ($(TP_PIN_MISMATCH)): run \
	git submodule update --init $(addprefix third_party/,$(TP_PIN_MISMATCH)) here, or update that checkout
TP_ERR_COPIES = $(if $(TP_SHARED_MISSING),$(TP_ERR_SHARED),$(if $(TP_PIN_MISMATCH),$(TP_ERR_PIN)))
TP_ERR_FROM_SHARED = $(if $(filter /%,$(TP_SHARED)),$(if $(filter %/third_party,$(TP_SHARED)),$(TP_ERR_COPIES),\
	$(TP_ERR_NAME)),$(TP_ERR_RELATIVE))
TP_ERROR = $(strip $(if $(TP_LOCAL_MISSING),$(if $(TP_SHARED),$(TP_ERR_FROM_SHARED),$(TP_ERR_LOCAL))))
# The goals that need no third-party sources
TP_CHECK = $(if $(filter-out clean test registry print-%,$(or $(MAKECMDGOALS),all)),\
	$(if $(TP_ERROR),$(error $(TP_ERROR))))
