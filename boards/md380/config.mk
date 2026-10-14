# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# MD-380 feature defaults (config/features.mk lists them all). 275 KB of code space, 0x0800C000..0x08050C43: the stock
# codec runs in place from 0x08050C44 (md380.ld, docs/spec/codec-md380.md).
# 160x128 display, all rows visible (unconfirmed, hardware-md380.md 7)
CONFIG_FONT_XL ?= n
CONFIG_DISPLAY_INSET ?= 0
# No sub-audio tone decode or encode yet: the stock firmware decodes CTCSS / DCS in software from an ADC (PA2) and
# encodes it as PWM on PC7, neither is written (hardware-md380.md 6.4). Channels with a tone still open on carrier
CONFIG_SUBTONE ?= n
# The FM TX tones (roger beep, 1750 Hz burst) and APRS AFSK need a TX tone path through the HR_C5000 or the PC7 PWM:
# not written
CONFIG_FM_TX_TONES ?= n
CONFIG_APRS_RX ?= n
# The SFR and split duplex glue is HR_C6000 only (docs/spec/sfr.md: the HR_C5000 lacks CRC_MCU_Control)
CONFIG_SFR ?= n
CONFIG_HOTSPOT_SPLIT ?= n
# Size (docs/feature-matrix.md "Whole images", docs/build-config.md "Board defaults"): every other feature fits only with link time optimization, so
# it is on by default here, third-party code included (FreeRTOS marks what its port's assembly reaches
# portDONT_DISCARD; the vectors are referenced from the assembly start-up). Untested hardware either way: LTO=0 builds
# the plain way (and then needs more features off)
LTO ?= 1
LTO_3P ?= 1
# The one feature left out for flash: the MMDVM hotspot (12 KB). Least useful here: it needs a host running MMDVMHost,
# the MD-380's DMR is untested on air, and the same USB link carries IP over DMR and the radio's own services instead.
# With it off the default image has about 2 KB free; DMR_ROAMING (also 12 KB) is the alternative
CONFIG_HOTSPOT ?= n
# USRP voice over UDP (4.9 KB) is off by default too: it carries DMR only here (no FM audio path to the processor), and
# a build with it on needs another feature off. Users can still pick it
CONFIG_NET_USRP ?= n
# Compressed UDP/IP headers on transmit (about 460 B) are off by default for room (the APRS position page, 2026-10-13):
# IP over DMR still works, with full headers. Users can still pick it
CONFIG_DMR_UDP_COMPRESS ?= n
# The radio's own APRS-IS client (about 2.7 KB) is off by default for room (the APRS path row and the side key
# assignment, 2026-10-13): this radio sends no APRS over the air and has no iGate, so it would only forward DMR-side
# beacons. Users can still pick it
CONFIG_NET_APRSIS ?= n
# The hub's Channels list (about 1.9 KB) doesn't fit beside everything else: off by default here; a build with
# CONFIG_CHANNEL_LIST=y needs room made, as CONFIG_DMR_IP_GATEWAY=n does
CONFIG_CHANNEL_LIST ?= n
# Control station mode (PC dispatch software on the USB network link, 288 bytes) is off by default for room (the side
# key functions and the Channels list, 2026-10-14). Users can still pick it
CONFIG_DMR_CONTROL_STATION ?= n
# Not written for this radio (above), left out for flash, or no use on it (FONT_XL is for 240 px and wider panels,
# this one is 160 px; no air band receiver or broadcast receiver, docs/spec/airband-and-broadcast.md): tools/ci.sh's
# maximal configuration leaves these off
BOARD_UNSUPPORTED := SUBTONE FM_TX_TONES APRS_RX APRS_IGATE SFR HOTSPOT_SPLIT FONT_XL HOTSPOT AM_RX BCAST_FM
# Supported, but the maximal configuration leaves these off too: PPP stands in for the USB network adapter on UART-only
# radios (docs/spec/ppp.md), and this one has the adapter; USRP, UDP header compression and the APRS-IS client don't
# fit beside everything else (above), nor does control station mode or the Channels list
MAXIMAL_SKIP := NET_PPP NET_USRP DMR_UDP_COMPRESS NET_APRSIS CHANNEL_LIST DMR_CONTROL_STATION
