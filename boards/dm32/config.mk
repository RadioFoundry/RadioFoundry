# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-32UV feature defaults (config/features.mk lists them all). 880 KB of code space before the codec slot, so RAM is
# the limit: 165,612 B above the codec's RAM window and 27,868 B below it (RAM_LO, which holds the core's ODR_CPU_RAM
# state; dm32.ld, docs/spec/hardware-dm32.md 17.1). Every feature the radio has hardware for is on: the default image
# keeps about 11 KB of main RAM free (as much as before the network went in), RAM_LO about 3.5 KB
# No USB on the radio: the host link (CPS, hotspot, debug link) runs on the programming cable's UART0
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
# The 240x320 panel is driven at half resolution (120x160 logical, every pixel doubled): the core's frame buffer
# is 40 KB (core/ui/gfx.h), so CONFIG_FONT_XL buys nothing yet
CONFIG_FONT_XL ?= n
# The bezel hides the panel's edges: 5 px on the left, 4 px on the right, 3 px at the top and 2 px at the bottom,
# checked on the radio
CONFIG_DISPLAY_INSET_LEFT ?= 5
CONFIG_DISPLAY_INSET_RIGHT ?= 4
CONFIG_DISPLAY_INSET_TOP ?= 3
CONFIG_DISPLAY_INSET_BOTTOM ?= 2
# Air band AM receive and the RDA5807M broadcast FM receiver (docs/spec/airband-and-broadcast.md, parked): the core
# is built and host tested, the board glue isn't written yet (no CAP_AM_RX / CAP_BCAST_FM), so nothing runs yet
CONFIG_AM_RX ?= $(CONFIG_FM)
CONFIG_BCAST_FM ?= y
# The IP link: PPP on the host port (no USB network adapter here), and with it the IP gateway and every service.
# PPP is only the gateway's link here, so CONFIG_DMR_IP_GATEWAY=n on the command line takes it along
ifeq ($(origin CONFIG_DMR_IP_GATEWAY)$(CONFIG_DMR_IP_GATEWAY),command linen)
CONFIG_NET_PPP ?= n
endif
CONFIG_NET_PPP ?= $(if $(and $(filter y,$(CONFIG_HOSTLINK)),$(filter y,$(CONFIG_DMR))),y,n)
# TCP connections (core/net/tcp.h, 4 elsewhere): about 1.5 KB RAM each across TCP, HTTP and rigctld. Three hold a
# browser's two and rigctld (TIME_WAIT slots are reused) and keep 11 KB free; 4 leaves 9.6 KB
TCP_CONNS ?= 3
MCU_DEFS += -DTCP_CONNS=$(TCP_CONNS)
# What the radio has no hardware for (no USB data port): ci's maximal configuration leaves these off. Everything else
# is in it, FONT_XL and AUDIO_PROBE too (9.9 KB of main RAM free)
BOARD_UNSUPPORTED := USB USB_NETWORK
