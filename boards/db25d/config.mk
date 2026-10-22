# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DB25-D feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-db25d.md 9): analog FM
# on the BK4819s and the host link on UART1, settings stored on the SPI NAND, DMR receive and transmit on the shared
# modem glue with the codec recipe. The open-firmware CPS (qdmr, tools/cps_client.py) runs on the host link over the
# stored codeplug (hardware-db25d.md 6.6, 9). RAM is the limit, as on the DM-32UV: ODR has 216,576 B below the codec
# window (db25d_dmr.ld). The switches that are off are listed below
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
# The core's storage over ODR's NAND block range (hardware-db25d.md 6.6)
CONFIG_NAND_STORAGE ?= y
# DMR (hardware-db25d.md 4.5, 9): receive and transmit checked on the SDR bench. Voice needs the codec merged (8). With
# it the hotspot (the MMDVM host port on UART1) is on: simplex whole calls through the same per-burst TX path as a call
# (hardware-db25d.md 9). CONFIG_VFO_DMR follows the default (the core's DMR VFO, checked on this radio)
CONFIG_DMR ?= y
# The IP link: PPP on the host port (UART1), and with it the IP gateway and every service riding it. PPP is only the
# gateway's link here, so CONFIG_DMR_IP_GATEWAY=n on the command line takes it along
ifeq ($(origin CONFIG_DMR_IP_GATEWAY)$(CONFIG_DMR_IP_GATEWAY),command linen)
CONFIG_NET_PPP ?= n
endif
CONFIG_NET_PPP ?= $(if $(and $(filter y,$(CONFIG_HOSTLINK)),$(filter y,$(CONFIG_DMR))),y,n)
# USRP voice over UDP needs 8 kHz 16-bit PCM both ways (128 kbit/s plus headers): more than the 115200 baud host port carries.
# On the radio it filled the link whenever a call was heard and the host link task stopped answering (core/usbaudio/usrp_net.c)
CONFIG_NET_USRP ?= n
# TCP connections (core/net/tcp.h, 4 elsewhere): about 1.5 KB RAM each
TCP_CONNS ?= 3
MCU_DEFS += -DTCP_CONNS=$(TCP_CONNS)
# The split-duplex roles need a continuous base-station outbound that the per-burst TX path doesn't have: the core
# would refuse the role, so the 1.8 KB of RAM stays free
CONFIG_HOTSPOT_SPLIT ?= n
# The single frequency repeater: switches RX and TX every slot, which this radio's per-burst retune hasn't been timed for
CONFIG_SFR ?= n
# With DMR: the linker script with the codec window at the top of SRAM and the modem SRAM table window (db25d_dmr.ld),
# its codec host (the profile ids are db25d-2501, db25d-2409, db25d-2405, db25d-020; hardware-db25d.md 8), the core's
# CPU state in .ram_lo and the runtime stub's load span inside the codec window (0x45000..0x58000; the universal stub
# copies itself to its run address). Bench knobs (dmr_db25d.c, radio_db25d.c): DB25D_DMR_RF_MODE (the modem's RF_MODE word), DB25D_DMR_AF_GAIN
# (the main chip's AF DAC gain in DMR, 0-15), DB25D_DMR_SPK_SWITCH=1 (PTC18 stays high in DMR, as before)
ifeq ($(CONFIG_DMR),y)
LDSCRIPT := boards/db25d/db25d_dmr.ld
MCU_DEFS += -DODR_CPU_RAM_SECTION='".ram_lo"' -DHRC_STUB_BASE=0x00045000u -DHRC_STUB_SIZE=0x00013000u
ifneq ($(DB25D_DMR_RF_MODE),)
MCU_DEFS += -DDB25D_DMR_RF_MODE=$(DB25D_DMR_RF_MODE)
endif
ifneq ($(DB25D_DMR_AF_GAIN),)
MCU_DEFS += -DDB25D_DMR_AF_GAIN=$(DB25D_DMR_AF_GAIN)u
endif
DB25D_DMR_SPK_SWITCH ?= 0
MCU_DEFS += -DDB25D_DMR_SPK_SWITCH=$(DB25D_DMR_SPK_SWITCH)
# DMR transmit (hardware-db25d.md 4.5), checked on the SDR bench; DB25D_DMR_TX=0 builds receive only. DB25D_DMR_TX_INVERT=1
# flips the 4FSK polarity (the modem's RF_MODE bit 7) for a bench check against the stock setting
DB25D_DMR_TX ?= 1
DB25D_DMR_TX_INVERT ?= 0
MCU_DEFS += -DDB25D_DMR_TX=$(DB25D_DMR_TX) -DDB25D_DMR_TX_INVERT=$(DB25D_DMR_TX_INVERT)
else
MCU_DEFS += -DHRC_STUB_BASE=0x00030000u -DHRC_STUB_SIZE=0x00020000u
endif
# ci's maximal configuration (CONFIG_DMR on) builds the DMR transmitter too
MAXIMAL_VARS := DB25D_DMR_TX=1
# Bench sweep of the reference trim: make BOARD=db25d DB25D_TX_CENTER=116 replaces the page's TX center byte (the bias the C7000
# modem drives into the TCXO, hardware-db25d.md 4.6). Unset: the page's value
DB25D_TX_CENTER ?=
ifneq ($(DB25D_TX_CENTER),)
MCU_DEFS += -DDB25D_TX_CENTER_OVERRIDE=$(DB25D_TX_CENTER)
endif
# APRS receive needs the FM receive audio as PCM, which this radio doesn't give the core (its PCM path is the DMR
# modem's), so the demodulator, the audio mux and the iGate would only take RAM: the application's RAM window ends where
# the runtime stub's begins (db25d.ld), and the CPS's request buffer needs the room (2 KB)
CONFIG_APRS_RX ?= n
CONFIG_APRS_IGATE ?= n
# Not wired yet: the RDA5802N (CAP_BCAST_FM) and the air band
CONFIG_BCAST_FM ?= n
CONFIG_AM_RX ?= n
# 160x128 like the DM-1701, which builds without the large font
CONFIG_FONT_XL ?= n
# What the board lacks or can't do yet, one switch per line with the reason (tools/boards.py checks there is
# one): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += USB	# no USB data port: the host link runs on the programming cable's UART
BOARD_UNSUPPORTED += USB_NETWORK	# no USB data port for the network adapter: PPP on the host UART stands in
BOARD_UNSUPPORTED += NET_USRP	# 8 kHz PCM both ways is more than the 115200 baud host port carries (above)
BOARD_UNSUPPORTED += HOTSPOT_SPLIT	# the per-burst TX path has no continuous base-station outbound (above)
BOARD_UNSUPPORTED += SFR	# RX and TX switch every slot: the per-burst retune isn't timed for it (above)
BOARD_UNSUPPORTED += APRS_RX	# no FM receive audio as PCM (the PCM path is the DMR modem's, above)
BOARD_UNSUPPORTED += APRS_IGATE	# needs APRS receive, which needs the FM receive audio as PCM
BOARD_UNSUPPORTED += BCAST_FM	# the RDA5802N broadcast receiver isn't wired yet
BOARD_UNSUPPORTED += AM_RX	# the air band isn't wired yet
BOARD_UNSUPPORTED += AUDIO_PROBE	# a bench diagnostic for an audio path this radio doesn't have
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
