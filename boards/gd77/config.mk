# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-77 feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd77.md 12): the UI on
# the 128x64 display, keys, power and FM receive. What isn't wired yet is off and listed as unsupported below, so ci's
# maximal configuration leaves it off too; each comes back as its piece lands.
# DMR: the HR_C6000's DMR glue, the vocoder port (SPI1) and the codec are not wired
CONFIG_DMR ?= n
# USB serial (CDC-ACM) with the host link and the debug link (docs/spec/hardware-gd77.md 13). Not yet: the network
# adapter personality, and the open-firmware CPS until the storage map is wired
CONFIG_USB_NETWORK ?= n
CONFIG_CPS_OPENFW ?= n
# APRS sends AFSK through the FM transmitter, which is refused until the PA path is confirmed
CONFIG_APRS ?= n
# 128x64 monochrome: no large font, no colors
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
# ci's maximal configuration leaves these off: not wired yet (DMR and everything under it, the USB network adapter and
# its services, CPS, APRS, the crash report over DMR), or not on this radio (air band, broadcast receiver, SPI NAND,
# motor, color and anti-aliased fonts)
BOARD_UNSUPPORTED := DMR DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_TMS DMR_ARS DMR_LRRP DMR_SIGNALING \
	DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP \
	NET_RIGCTL NET_APRSIS NET_USRP MDNS TFTP TFTP_IDDB DMR_CONTROL_STATION SNMP SFR \
	USB_NETWORK NET_PPP HOTSPOT HOTSPOT_SPLIT CPS_OPENFW CPS_FAST VFO_DMR STUB_ENTRY AUDIO_PROBE \
	CRASH_REPORT APRS APRS_RX APRS_IGATE AM_RX BCAST_FM NAND_STORAGE VIBRATE FONT_XL FONT_AA \
	FONT_BIG UI_COLOR
# FM is the one mode this build has
BOARD_REQUIRED := FM
