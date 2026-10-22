# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-88 SoC B feature defaults. B runs no radio core: analog FM and DMR receive, the link to A and the watchdog. Every
# other switch is off and listed as unsupported, so ci's maximal configuration is this one
CONFIG_FM ?= y
CONFIG_SUBTONE ?= y
CONFIG_DMR ?= y
CONFIG_WATCHDOG ?= y
OFF_FEATURES := DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_TMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY \
	DMR_ROAMING DMR_IP_GATEWAY DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_RIGCTL \
	NET_APRSIS NET_USRP MDNS TFTP TFTP_IDDB DMR_CONTROL_STATION SNMP SFR TASK_STACKS USB HOSTLINK USB_NETWORK NET_PPP \
	HOTSPOT HOTSPOT_SPLIT DEBUGLINK CPS_OPENFW CPS_FAST VFO VFO_FM VFO_DMR DIAG FONT_XL FONT_AA FONT_BIG UI_COLOR \
	UI_SETTINGS SCAN DUAL_WATCH BEEP FM_TX_TONES VOX BATTERY_SAVE VIBRATE AUDIO_PROBE STUB_ENTRY NAND_STORAGE CRASH_LOG \
	CRASH_SCREEN CRASH_REPORT FAULT_TRACE AM_RX BCAST_FM APRS APRS_RX APRS_IGATE KEY_ASSIGN CHANNEL_LIST CHANNEL_EDIT
$(foreach f,$(OFF_FEATURES),$(eval CONFIG_$(f) ?= n))
BOARD_UNSUPPORTED := $(OFF_FEATURES)
# The receiver is the board: FM always (a configuration without DMR is still a receiver)
BOARD_REQUIRED := FM
# With DMR: the codec window at the top of SRAM and the modem SRAM table window (boards/gd88/gd88_dmr.ld, shared with A)
# and its codec host (profile ids gd88b-<version>)
ifeq ($(CONFIG_DMR),y)
LDSCRIPT := boards/gd88/gd88_dmr.ld
MCU_DEFS += -DHRC_CODEC_BOARD='"gd88b"' -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
MCU_DEFS += -DHRC7000_RX_NEXT_SLOT_XOR=0u
else
MCU_DEFS += -DHRC_STUB_BASE=0x00030000u -DHRC_STUB_SIZE=0x00020000u
endif
# Receive only: the band's TX switches stay low (paths_gd88b.c), and the shared radio code refuses tx()
MCU_DEFS += -DGD88_TX=0 -DGD88_BAND_LO_GPIO=0
