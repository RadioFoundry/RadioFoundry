# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# GD-88 SoC A feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd88.md 13):
# written from the hardware notes, not yet run on a radio. Analog FM and DMR receive on the BK4819 pair and the C7000
# modem, the host link on UART1, settings on the SPI NAND, SoC B as the second receiver (dual watch). Transmit is off
# (GD88_TX). The switches that are off are listed below; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
CONFIG_NAND_STORAGE ?= y
CONFIG_DMR ?= y
# Not written yet (the list below)
CONFIG_NET_USRP ?= n
CONFIG_HOTSPOT_SPLIT ?= n
CONFIG_SFR ?= n
# The 320x240 panel takes the large fonts (they are for 240 px and wider)
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
CONFIG_FONT_BIG ?= $(CONFIG_FONT_AA)
# The bezel hides the panel's right edge: 5 px still cut the status bar's % sign in half on the radio (2026-10-26)
CONFIG_DISPLAY_INSET_RIGHT ?= 10
# With DMR: the platform's linker script with the codec window at the top of SRAM and the modem SRAM table window
# (memory.ld), its codec host (profile ids gd88-<version>), the core's CPU state in .ram_lo and the runtime stub's load
# span inside the codec window (0x45000..0x58000). Without DMR: no codec window, the CPU state in .bss and the stub in
# the 128 KB above it (fm_only.ld). Bench knobs (dmr_gd88.c): GD88_DMR_RF_MODE, GD88_DMR_AF_GAIN
ifeq ($(CONFIG_DMR),y)
MCU_DEFS += -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
ifneq ($(GD88_DMR_RF_MODE),)
MCU_DEFS += -DGD88_DMR_RF_MODE=$(GD88_DMR_RF_MODE)
endif
ifneq ($(GD88_DMR_AF_GAIN),)
MCU_DEFS += -DGD88_DMR_AF_GAIN=$(GD88_DMR_AF_GAIN)u
endif
# The voice-access flag on the slot the RX timeslot interrupt shows, as on the other HR_C7000 radios (same modem)
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
# APRS receive needs FM receive audio as PCM, not written yet (the list below)
CONFIG_APRS_RX ?= n
CONFIG_APRS_IGATE ?= n
# The RDA5807-class broadcast tuner (CAP_BCAST_FM, I2C 0x11 on PTA7 / PTA8); the air band isn't wired yet
CONFIG_BCAST_FM ?= y
CONFIG_AM_RX ?= n
# Off by default (docs/build-config.md, "Unsupported, not written, unconfirmed"), one switch a line with the reason.
# The hardware has each of these; the code for it on this radio isn't written yet. Transmit itself is off until A's
# power control is worked out (GD88_TX), so the transmit-only features wait for it. USB and USB_NETWORK are the
# platform's (no USB data port)
CONFIG_VOX ?= n
CONFIG_DTMF ?= n
BOARD_NOT_WRITTEN += NET_USRP	# the host link stalls under USRP's PCM; another HR_C7000 board carries it at the same 115200 baud
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs transmit, which waits for A's power control (GD88_TX)
BOARD_NOT_WRITTEN += SFR	# needs transmit, which waits for A's power control (GD88_TX)
BOARD_NOT_WRITTEN += DTMF	# no hal_radio_t.tx_dtmf hook yet, and transmit waits for A's power control (GD88_TX)
BOARD_NOT_WRITTEN += APRS_RX	# FM receive audio isn't taken from the modem's AF ADC as PCM yet
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive (above)
BOARD_NOT_WRITTEN += AM_RX	# the air band isn't wired yet (board.radio->rx_am)
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet (the mic level is on ADC channel 0)
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe's audio tap (board.audio_tap) isn't written for this radio
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
