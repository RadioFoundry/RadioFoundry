# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-88 SoC B feature defaults. B runs no radio core: analog FM and DMR receive, the link to A and the watchdog. Every
# other switch is off and listed as unsupported, one a line with the reason, so ci's maximal configuration is this one.
# USB and USB_NETWORK are the platform's (platform/hrc7000/config.mk)
CONFIG_FM ?= y
CONFIG_SUBTONE ?= y
CONFIG_DMR ?= y
CONFIG_WATCHDOG ?= y
BOARD_UNSUPPORTED += DMR_PRIVACY	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_TALKER_ALIAS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_GPS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_SMS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_TMS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_ARS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_LRRP	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_SIGNALING	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_EMERGENCY	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_ROAMING	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_IP_GATEWAY	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_UDP_COMPRESS	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_CONFIRMED	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += DMR_CONTROL_STATION	# B only hands A the received bursts: SoC A runs the DMR services
BOARD_UNSUPPORTED += NET_TCP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_KISS	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_HTTP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_HTTP_GZIP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_RIGCTL	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_APRSIS	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_USRP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += NET_PPP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += MDNS	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += TFTP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += TFTP_IDDB	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += SNMP	# B has no host port, only the link to A: SoC A runs the network
BOARD_UNSUPPORTED += HOSTLINK	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += DEBUGLINK	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += CPS_OPENFW	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += CPS_FAST	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += HOTSPOT	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += HOTSPOT_SPLIT	# B has no host port, only the link to A (UART3)
BOARD_UNSUPPORTED += SFR	# B is receive only
BOARD_UNSUPPORTED += VFO	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += VFO_FM	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += VFO_DMR	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += SCAN	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += DUAL_WATCH	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += CHANNEL_LIST	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += CHANNEL_EDIT	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += KEY_ASSIGN	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += UI_COLOR	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += UI_SETTINGS	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += FONT_XL	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += FONT_AA	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += FONT_BIG	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += DIAG	# B has no display or keys: SoC A runs the user interface
BOARD_UNSUPPORTED += BEEP	# B has no speaker path or transmitter of its own: SoC A does this
BOARD_UNSUPPORTED += FM_TX_TONES	# B has no speaker path or transmitter of its own: SoC A does this
BOARD_UNSUPPORTED += VOX	# B has no speaker path or transmitter of its own: SoC A does this
BOARD_UNSUPPORTED += VIBRATE	# B has no speaker path or transmitter of its own: SoC A does this
BOARD_UNSUPPORTED += AUDIO_PROBE	# B has no speaker path or transmitter of its own: SoC A does this
BOARD_UNSUPPORTED += BATTERY_SAVE	# SoC A powers B down and up
BOARD_UNSUPPORTED += STUB_ENTRY	# B's stub comes through its first stage and A's bridge
BOARD_UNSUPPORTED += NAND_STORAGE	# B keeps no settings: SoC A stores them
BOARD_UNSUPPORTED += TASK_STACKS	# B has no storage or display to report on: SoC A does this
BOARD_UNSUPPORTED += CRASH_LOG	# B has no storage or display to report on: SoC A does this
BOARD_UNSUPPORTED += CRASH_SCREEN	# B has no storage or display to report on: SoC A does this
BOARD_UNSUPPORTED += CRASH_REPORT	# B has no storage or display to report on: SoC A does this
BOARD_UNSUPPORTED += FAULT_TRACE	# B has no storage or display to report on: SoC A does this
BOARD_UNSUPPORTED += AM_RX	# the air band, broadcast tuner and APRS are SoC A's
BOARD_UNSUPPORTED += BCAST_FM	# the air band, broadcast tuner and APRS are SoC A's
BOARD_UNSUPPORTED += APRS	# the air band, broadcast tuner and APRS are SoC A's
BOARD_UNSUPPORTED += APRS_RX	# the air band, broadcast tuner and APRS are SoC A's
BOARD_UNSUPPORTED += APRS_IGATE	# the air band, broadcast tuner and APRS are SoC A's
# Every unsupported switch defaults off, read before the platform's defaults (CONFIG_HOSTLINK among them)
$(foreach f,$(BOARD_UNSUPPORTED),$(eval CONFIG_$(f) ?= n))
# The receiver is the board: FM always (a configuration without DMR is still a receiver)
BOARD_REQUIRED := FM
# With DMR: the platform's linker script with the codec window at the top of SRAM and the modem SRAM table window
# (memory.ld) and its codec host (profile ids gd88b-<version>). Without DMR: A's FM-only script (the same layout)
ifeq ($(CONFIG_DMR),y)
MCU_DEFS += -DHRC_CODEC_BOARD='"gd88b"' -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
MCU_DEFS += -DHRC7000_RX_NEXT_SLOT_XOR=0u
else
LDSCRIPT := boards/gd88/fm_only.ld
CPU_RAM_SECTION :=
MCU_DEFS += -DHRC_STUB_BASE=0x00030000u -DHRC_STUB_SIZE=0x00020000u
endif
# Receive only: the band's TX switches stay low (paths_gd88b.c), and the shared radio code refuses tx()
MCU_DEFS += -DGD88_TX=0 -DGD88_BAND_LO_GPIO=0
