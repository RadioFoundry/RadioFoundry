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
# Size (docs/build-config.md "The MD-380's image"): every other feature fits only with link time optimization, so
# it is on by default here, third-party code included (FreeRTOS marks what its port's assembly reaches
# portDONT_DISCARD; the vectors are referenced from the assembly start-up). Untested hardware either way: LTO=0 builds
# the plain way (and then needs more features off)
LTO ?= 1
LTO_3P ?= 1
# The one feature left out for flash: the MMDVM hotspot (12 KB). Least useful here: it needs a host running MMDVMHost,
# the MD-380's DMR is untested on air, and the same USB link carries IP over DMR and the radio's own services instead.
# With it off the default image has about 3 KB free; DMR_ROAMING (also 12 KB) is the alternative
CONFIG_HOTSPOT ?= n
# Not written for this radio (above), left out for flash, or no use on it (FONT_XL is for 240 px and wider panels,
# this one is 160 px): tools/ci.sh's maximal configuration leaves these off
BOARD_UNSUPPORTED := SUBTONE FM_TX_TONES APRS_RX APRS_IGATE SFR HOTSPOT_SPLIT FONT_XL HOTSPOT
