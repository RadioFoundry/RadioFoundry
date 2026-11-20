# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## title: Commercial
## summary: For business and public service radios. Leaves out amateur-only features.
##
## A release variant (make VARIANT=commercial). Variants only turn features off; the board's own limits still apply.
## Each switch below carries the reason the site shows next to it. Draft: the owner decides the final list.

# The MMDVM hotspot links the radio to amateur networks (BrandMeister, TGIF) through a PC; split duplex goes with it
CONFIG_HOTSPOT ?= n
# APRS is an amateur radio service; KISS over TCP and the APRS-IS client go with it
CONFIG_APRS ?= n
# D-STAR is an amateur radio mode (the JARL standard); no business or public service system uses it
CONFIG_DSTAR ?= n
