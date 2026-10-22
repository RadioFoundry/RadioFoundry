# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-88 SoC A feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd88.md 13):
# written from the hardware notes, not yet run on a radio. Analog FM and DMR receive on the BK4819 pair and the C7000
# modem, the host link on UART1, settings on the SPI NAND, SoC B as the second receiver (dual watch). Transmit is off
# (GD88_TX). The switches that are off are listed below; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
CONFIG_NAND_STORAGE ?= y
CONFIG_DMR ?= y
# USRP voice needs more than the 115200 baud host port carries (as the DB25-D)
CONFIG_NET_USRP ?= n
# The split-duplex roles and the single frequency repeater need a transmitter
CONFIG_HOTSPOT_SPLIT ?= n
CONFIG_SFR ?= n
# The 320x240 panel takes the large fonts, as the DM-32UV
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
CONFIG_FONT_BIG ?= $(CONFIG_FONT_AA)
# With DMR: the platform's linker script with the codec window at the top of SRAM and the modem SRAM table window
# (memory.ld), its codec host (profile ids gd88-<version>), the core's CPU state in .ram_lo and the runtime stub's load
# span inside the codec window (0x45000..0x58000). Without DMR: no codec window, the CPU state in .bss and the stub in
# the 128 KB above it (fm_only.ld). Bench knobs (dmr_gd88.c): GD88_DMR_RF_MODE, GD88_DMR_AF_GAIN
ifeq ($(CONFIG_DMR),y)
MCU_DEFS += -DHRC_CODEC_BOARD='"gd88"' -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
ifneq ($(GD88_DMR_RF_MODE),)
MCU_DEFS += -DGD88_DMR_RF_MODE=$(GD88_DMR_RF_MODE)
endif
ifneq ($(GD88_DMR_AF_GAIN),)
MCU_DEFS += -DGD88_DMR_AF_GAIN=$(GD88_DMR_AF_GAIN)u
endif
# The voice-access flag on the slot the RX timeslot interrupt shows, as on the DB25-D and the DM-32UV (same modem)
MCU_DEFS += -DHRC7000_RX_NEXT_SLOT_XOR=0u
GD88_DMR_TX_INVERT ?= 0
MCU_DEFS += -DGD88_DMR_TX_INVERT=$(GD88_DMR_TX_INVERT)
else
LDSCRIPT := boards/gd88/fm_only.ld
CPU_RAM_SECTION :=
MCU_DEFS += -DHRC_STUB_BASE=0x00030000u -DHRC_STUB_SIZE=0x00020000u
endif
# Transmit (common/gd88_rf.h): off until A's power control is known. ci's maximal configuration builds it
GD88_TX ?= 0
MCU_DEFS += -DGD88_TX=$(GD88_TX)
MAXIMAL_VARS := GD88_TX=1
# A bench sweep of the reference trim: GD88_TX_CENTER=116 replaces the page's TX center byte
GD88_TX_CENTER ?=
ifneq ($(GD88_TX_CENTER),)
MCU_DEFS += -DGD88_TX_CENTER_OVERRIDE=$(GD88_TX_CENTER)
endif
# APRS receive needs FM receive audio as PCM, which this radio doesn't give the core (its PCM path is the modem's)
CONFIG_APRS_RX ?= n
CONFIG_APRS_IGATE ?= n
# The RDA5807-class broadcast tuner (CAP_BCAST_FM, I2C 0x11 on PTA7 / PTA8); the air band isn't wired yet
CONFIG_BCAST_FM ?= y
CONFIG_AM_RX ?= n
# What the board lacks or can't do yet, one switch per line with the reason (tools/boards.py checks there is one): ci's
# maximal configuration leaves these off. USB and USB_NETWORK are the platform's
BOARD_UNSUPPORTED += NET_USRP	# 8 kHz PCM both ways is more than the 115200 baud host port carries
BOARD_UNSUPPORTED += HOTSPOT_SPLIT	# needs a transmitter, and A's is off until its power control is known
BOARD_UNSUPPORTED += SFR	# needs a transmitter, and A's is off until its power control is known
BOARD_UNSUPPORTED += APRS_RX	# no FM receive audio as PCM (the PCM path is the DMR modem's)
BOARD_UNSUPPORTED += APRS_IGATE	# needs APRS receive, which needs the FM receive audio as PCM
BOARD_UNSUPPORTED += AM_RX	# the air band isn't wired yet
BOARD_UNSUPPORTED += AUDIO_PROBE	# a bench diagnostic for an audio path this radio doesn't have
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
