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
# The TCP services (docs/spec/tcp-services.md) are off for flash: TCP and HTTP alone fit the default image (97.3 %),
# but not tools/ci.sh's maximal configuration (it overflowed by 180 bytes); KISS over TCP and APRS-IS would carry APRS,
# which has no RF path here (no AFSK, above)
CONFIG_NET_TCP ?= n
# Not written for this radio (above), or left out for flash: tools/ci.sh's maximal configuration leaves these off
BOARD_UNSUPPORTED := SUBTONE FM_TX_TONES APRS_RX APRS_IGATE SFR HOTSPOT_SPLIT NET_TCP NET_KISS NET_HTTP NET_RIGCTL NET_APRSIS
