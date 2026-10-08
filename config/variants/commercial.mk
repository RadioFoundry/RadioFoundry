# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## title: Commercial
## summary: For business and public service fleets: text messages, location, presence, dispatch and network
## summary: management. Amateur-only features (hotspot, APRS) are left out.
##
## A release variant (make VARIANT=commercial). Variants only turn features off; the board's own limits still apply.
## Each switch below carries the reason the site shows next to it. Draft: the owner decides the final list.

# The MMDVM hotspot links the radio to amateur networks (BrandMeister, TGIF) through a PC; split duplex goes with it
CONFIG_HOTSPOT ?= n
# APRS is an amateur radio service; KISS over TCP and the APRS-IS client go with it
CONFIG_APRS ?= n
