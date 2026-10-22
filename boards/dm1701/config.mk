# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9;
# the codec sectors 10 and 11 are never linked into (dm1701.ld, docs/spec/codec-placement.md).
# 160x120 display
# The fault trace needs a host UART: this radio's host port is USB, and its one UART is the GPS receive line
CONFIG_FAULT_TRACE ?= n
CONFIG_FONT_XL ?= n
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
# Link time optimization on our sources (about 17 KB smaller): the default image no longer fits without it since USRP
# (CONFIG_NET_USRP, 2026-10-13), when the code had sectors 3-6 only. LTO=0 builds the plain way
LTO ?= 1
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above)
MAXIMAL_SKIP := NET_PPP APRS_IGATE
# No use on a 160x120 panel (the 16x32 font is for 240 px and wider), and no hardware for the air band (the AT1846S is
# FM only) or a broadcast receiver (docs/spec/airband-and-broadcast.md), so the maximal configuration leaves them off
BOARD_UNSUPPORTED := FONT_XL AM_RX BCAST_FM FAULT_TRACE
# Stub entry stays off here for now: its 8 KB staging buffer takes main RAM to 95 % (docs/spec/stub-stm32f4.md), not yet
# run on a DM-1701 with every default feature on. CONFIG_STUB_ENTRY=y builds it
CONFIG_STUB_ENTRY ?= n
