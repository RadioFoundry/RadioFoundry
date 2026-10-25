# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## title: Amateur
## summary: For amateur radio. Leaves out business features.
##
## A release variant (make VARIANT=amateur). Variants only turn features off; the board's own limits still apply.
## Each switch below carries the reason the site shows next to it. Draft: the owner decides the final list.

# Encryption that hides the meaning of a message isn't allowed on the amateur bands in most countries
CONFIG_DMR_PRIVACY ?= n
# Motorola text message format: business radio fleets and dispatch software
CONFIG_DMR_TMS ?= n
# Registration with a presence server: dispatch systems
CONFIG_DMR_ARS ?= n
# Location request and response for dispatch software (APRS covers positions on the amateur bands)
CONFIG_DMR_LRRP ?= n
# Control station mode for PC dispatch software
CONFIG_DMR_CONTROL_STATION ?= n
# Network management for fleets of radios
CONFIG_SNMP ?= n
