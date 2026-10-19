# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# MD-380 feature defaults (config/features.mk lists them all). 464 KB for code and read-only data: flash sectors 3-7;
# the codec sectors 8-11 are never linked into (md380.ld, docs/spec/codec-placement.md).
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
# Link time optimization, third-party code included (FreeRTOS marks what its port's assembly reaches
# portDONT_DISCARD; the vectors are referenced from the assembly start-up). The image needed it while the application
# had 275 KB, before the codec moved; it stays on for the smaller image. Untested hardware either way: LTO=0 builds
# the plain way
LTO ?= 1
LTO_3P ?= 1
# Not written for this radio (above), or no use on it (FONT_XL is for 240 px and wider panels, this one is 160 px; no
# air band receiver or broadcast receiver, docs/spec/airband-and-broadcast.md): tools/ci.sh's maximal configuration
# leaves these off
BOARD_UNSUPPORTED := SUBTONE FM_TX_TONES APRS_RX APRS_IGATE SFR HOTSPOT_SPLIT FONT_XL AM_RX BCAST_FM
# Supported, but the maximal configuration leaves these off too: PPP stands in for the USB network adapter on UART-only
# radios (docs/spec/ppp.md), and this one has the adapter. Stub entry and NAND storage aren't used on this radio
MAXIMAL_SKIP := NET_PPP STUB_ENTRY NAND_STORAGE
