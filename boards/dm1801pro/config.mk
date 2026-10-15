# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1801 Pro feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-dm1801pro.md 12):
# the UI, keys, data flash, battery, analog FM, DMR without voice (the codec isn't hosted yet) and the broadcast tuner,
# none of it run on a radio yet. Transmit needs the calibration to be found, so it is refused without it
# No USB data port: the host link (CPS, hotspot, debug link) runs on the programming cable's UART0
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
# The IP link is PPP on the host port, as on the DM-32UV, and the IP gateway with every service that rides it
ifeq ($(origin CONFIG_DMR_IP_GATEWAY)$(CONFIG_DMR_IP_GATEWAY),command linen)
CONFIG_NET_PPP ?= n
endif
CONFIG_NET_PPP ?= $(if $(and $(filter y,$(CONFIG_HOSTLINK)),$(filter y,$(CONFIG_DMR))),y,n)
# TCP connections: about 1.5 KB of RAM each across TCP, HTTP and rigctld
TCP_CONNS ?= 3
MCU_DEFS += -DTCP_CONNS=$(TCP_CONNS)
# The RDA5807-class broadcast tuner (CAP_BCAST_FM); the air band isn't wired yet
CONFIG_BCAST_FM ?= y
CONFIG_AM_RX ?= n
# 128x64 monochrome: no room for the large font
CONFIG_FONT_XL ?= n
# What the board can't do yet or lacks: ci's maximal configuration leaves these off
BOARD_UNSUPPORTED := USB USB_NETWORK AM_RX AUDIO_PROBE FONT_XL
