# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-77 feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd77.md 12): the UI on
# the 128x64 display, keys, power and FM receive. What isn't wired yet is off and listed as unsupported below, so ci's
# maximal configuration leaves it off too; each comes back as its piece lands.
# DMR: the HR_C6000 slot engine, the voice port (SPI1) and the PCM stream are wired; the codec isn't hosted yet, so
# DMR carries data and signaling but no voice (docs/spec/hardware-gd77.md 5.6). Not yet: the single frequency
# repeater glue, the hotspot (its modem path is unchecked here) and the crash report over DMR
CONFIG_SFR ?= n
CONFIG_HOTSPOT ?= n
CONFIG_CRASH_REPORT ?= n
# Transmit, FM and DMR: off until the PA path and the calibration are confirmed on a radio. With y the bands allow it
# and tx() keys only with a valid calibration point (docs/build-config.md, "Board build variables")
CONFIG_GD77_TX ?= n
MCU_DEFS += -DCONFIG_GD77_TX=$(if $(filter y,$(CONFIG_GD77_TX)),1,0)
# USB serial (CDC-ACM) with the host link and the debug link (docs/spec/hardware-gd77.md 13). Not yet: the network
# adapter personality, and the open-firmware CPS (not checked against this radio's storage map)
CONFIG_USB_NETWORK ?= n
CONFIG_CPS_OPENFW ?= n
# APRS sends AFSK through the FM transmitter, which is refused until the PA path is confirmed
CONFIG_APRS ?= n
# 128x64 monochrome: no large font, no colors
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
# ci's maximal configuration leaves these off: not wired yet (the DMR IP gateway, the USB network adapter and its
# services, CPS, the SFR glue, the hotspot, APRS, the crash report over DMR), or not on this radio (air band, broadcast
# receiver, SPI NAND, motor, color and anti-aliased fonts)
BOARD_UNSUPPORTED := DMR_IP_GATEWAY NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP \
	NET_RIGCTL NET_APRSIS NET_USRP MDNS TFTP TFTP_IDDB DMR_CONTROL_STATION SNMP SFR \
	USB_NETWORK NET_PPP HOTSPOT HOTSPOT_SPLIT CPS_OPENFW CPS_FAST AUDIO_PROBE \
	CRASH_REPORT APRS APRS_RX APRS_IGATE AM_RX BCAST_FM NAND_STORAGE VIBRATE FONT_XL FONT_AA \
	FONT_BIG UI_COLOR
# FM is the one mode this build has
BOARD_REQUIRED := FM
CONFIG_DTMF ?= n
BOARD_UNSUPPORTED += DTMF	# transmit is off until the RF power path is verified (above)
