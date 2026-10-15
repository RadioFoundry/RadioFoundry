# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1801 Pro feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-dm1801pro.md 12):
# the UI, keys, data flash, battery and analog FM receive. Transmit waits for the calibration to be located, DMR for
# the codec recipe and the modem glue, so the switches they hold are off and listed below
# No USB data port: the host link (CPS, hotspot, debug link) runs on the programming cable's UART0
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
CONFIG_DMR ?= n
CONFIG_NET_PPP ?= n
CONFIG_HOTSPOT ?= n
CONFIG_VFO_DMR ?= n
# Not wired yet: the RDA5807-class broadcast tuner (CAP_BCAST_FM) and the air band
CONFIG_BCAST_FM ?= n
CONFIG_AM_RX ?= n
# 128x64 monochrome: no room for the large font
CONFIG_FONT_XL ?= n
# What the board can't do yet or lacks: ci's maximal configuration leaves these off
BOARD_UNSUPPORTED := USB USB_NETWORK DMR DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_TMS DMR_ARS DMR_LRRP \
	DMR_SIGNALING DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP \
	NET_HTTP_GZIP NET_RIGCTL NET_APRSIS MDNS TFTP DMR_CONTROL_STATION SNMP SFR NET_PPP HOTSPOT HOTSPOT_SPLIT VFO_DMR \
	BCAST_FM AM_RX AUDIO_PROBE FONT_XL
# FM is the only mode until DMR is built, so the matrix has no FM-less configuration
BOARD_REQUIRED := FM
