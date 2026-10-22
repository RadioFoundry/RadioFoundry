# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# GD-88 SoC A feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd88.md 13):
# written from the hardware notes, not yet run on a radio. Analog FM and DMR receive on the BK4819 pair and the C7000
# modem, the host link on UART1, settings on the SPI NAND, SoC B as the second receiver (dual watch). Transmit is off
# (GD88_TX). The switches that are off are listed below
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
CONFIG_NAND_STORAGE ?= y
CONFIG_DMR ?= y
# The IP link: PPP on the host port (UART1), with the IP gateway; CONFIG_DMR_IP_GATEWAY=n on the command line takes it
# along
ifeq ($(origin CONFIG_DMR_IP_GATEWAY)$(CONFIG_DMR_IP_GATEWAY),command linen)
CONFIG_NET_PPP ?= n
endif
CONFIG_NET_PPP ?= $(if $(and $(filter y,$(CONFIG_HOSTLINK)),$(filter y,$(CONFIG_DMR))),y,n)
# USRP voice needs more than the 115200 baud host port carries (as the DB25-D)
CONFIG_NET_USRP ?= n
TCP_CONNS ?= 3
MCU_DEFS += -DTCP_CONNS=$(TCP_CONNS)
# The split-duplex roles and the single frequency repeater need a transmitter
CONFIG_HOTSPOT_SPLIT ?= n
CONFIG_SFR ?= n
# The 320x240 panel takes the large fonts, as the DM-32UV
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
CONFIG_FONT_BIG ?= $(CONFIG_FONT_AA)
# With DMR: the linker script with the codec window at the top of SRAM and the modem SRAM table window (gd88_dmr.ld),
# its codec host (profile ids gd88-<version>), the core's CPU state in .ram_lo and the runtime stub's load span inside
# the codec window (0x45000..0x58000). Bench knobs (dmr_gd88.c): GD88_DMR_RF_MODE, GD88_DMR_AF_GAIN
ifeq ($(CONFIG_DMR),y)
LDSCRIPT := boards/gd88/gd88_dmr.ld
MCU_DEFS += -DHRC_CODEC_BOARD='"gd88"' -DODR_CPU_RAM_SECTION='".ram_lo"' -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
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
# Not wired yet: the RDA5807-class broadcast receiver (I2C 0x11 on PTA7 / PTA8) and the air band
CONFIG_BCAST_FM ?= n
CONFIG_AM_RX ?= n
# What the board can't do yet or lacks: ci's maximal configuration leaves these off
BOARD_UNSUPPORTED := USB USB_NETWORK NET_USRP HOTSPOT_SPLIT SFR APRS_RX APRS_IGATE BCAST_FM AM_RX AUDIO_PROBE
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
