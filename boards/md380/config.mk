# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# MD-380 feature defaults (config/features.mk lists them all). 336 KB of code space in flash sectors 3-6; sectors 7-11
# are kept for the stock codec until its windows are known (md380.ld, docs/spec/hardware-md380.md 14).
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
