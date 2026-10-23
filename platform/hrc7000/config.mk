# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# HR_C7000 feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The platform has no USB device driver: the host link (CPS, hotspot, debug link) runs on the programming cable's UART
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
# The IP link: PPP on the host port (no USB network adapter), and with it the IP gateway and every service riding it.
# PPP is only the gateway's link here, so CONFIG_DMR_IP_GATEWAY=n on the command line takes it along
ifeq ($(origin CONFIG_DMR_IP_GATEWAY)$(CONFIG_DMR_IP_GATEWAY),command linen)
CONFIG_NET_PPP ?= n
endif
CONFIG_NET_PPP ?= $(if $(and $(filter y,$(CONFIG_HOSTLINK)),$(filter y,$(CONFIG_DMR))),y,n)
# TCP connections (core/net/tcp.h, 4 elsewhere): about 1.5 KB of RAM each across TCP, HTTP and rigctld, and RAM is the
# limit on these radios. Three hold a browser's two and rigctld (TIME_WAIT slots are reused)
TCP_CONNS ?= 3
MCU_DEFS += -DTCP_CONNS=$(TCP_CONNS)
# What no radio on the platform has, one switch per line with the reason (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): ci's maximal configuration leaves these off. The SoC has a USB controller, but no radio
# wires it to a connector (docs/spec/hardware-dm32.md 3.11)
BOARD_UNSUPPORTED += USB	# no USB data port: the host link runs on the programming cable's UART
BOARD_UNSUPPORTED += USB_NETWORK	# no USB data port for the network adapter: PPP on the host UART stands in
