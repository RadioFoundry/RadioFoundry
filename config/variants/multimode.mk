# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## title: Multimode
## summary: For amateur radio with more digital voice modes. Leaves out business features, the IP gateway and the translations to make room.
##
## A release variant (make VARIANT=multimode). Variants only turn features off; the board's own limits still apply.
## Everything amateur leaves out, plus the features whose RAM the extra modes need: on a radio whose RAM is the limit
## this frees about 31 KB for more digital voice modes and their software codecs. A board turns those modes on where
## the gateway is left out (docs/build-config.md, Variants). The hotspot, APRS, roaming, SMS and single frequency
## repeating stay. The radio's text is English only: the translations and the fonts'
## accented letters are flash the extra modes need.

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
# Network management for fleets of radios (it runs on the IP gateway below, so it goes with it anyway)
CONFIG_SNMP ?= n
# IP over DMR and every service on it (PPP, TCP, status page, web control, SNMP, TFTP, mDNS, rigctld, USRP, KISS and
# APRS-IS over TCP): the largest single user of RAM after the screen, about 17 KB
CONFIG_DMR_IP_GATEWAY ?= n
CONFIG_NET_PPP ?= n
# Two radios as one full duplex repeater: experimental, and its buffers cost about 7 KB
CONFIG_HOTSPOT_SPLIT ?= n
# Sending zones to other radios over the air: about 4 KB, and a codeplug file does the same job
CONFIG_ZONE_SHARE ?= n
# Emergency alarm and hot mic: a business and public safety feature, rarely used on amateur DMR
CONFIG_DMR_EMERGENCY ?= n
# English only. The translations stay in the source (core/ui/lang/*.po), and Settings > Display > Language lists them
# dimmed. Spanish is the one built by default, and the first language also brings the accented letters of every font
# (CONFIG_LANG_ANY): about 61 KB of flash on a 240x320 panel's faces, where the extra modes and the next ones need
# room. Without the accented glyphs a name or a message with an accented letter shows the letter without its accent
# (core/ui/fonts.c)
CONFIG_LANG_ES ?= n
# Italian: off by default already; off here too, so the image stays English only if the default changes
CONFIG_LANG_IT ?= n
# French: as Italian
CONFIG_LANG_FR ?= n
# Chinese: as Italian, and its 16x16 face would cost about 16 KB more
CONFIG_LANG_ZH ?= n
