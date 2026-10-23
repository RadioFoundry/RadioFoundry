# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-32UV feature defaults (config/features.mk lists them all). 880 KB of code space before the codec slot, so RAM is
# the limit: 165,612 B above the codec's RAM window and 27,868 B below it (RAM_LO, which holds the core's RFOUNDRY_CPU_RAM
# state; boards/dm32/memory.ld, docs/spec/hardware-dm32.md 17.1). Every feature the radio has hardware and glue for
# is on: the image keeps about 11 KB of main RAM free (as much as before the network went in), RAM_LO about 3.5 KB
# The host link runs on the programming cable's UART0; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
# The 240x320 panel at full resolution in 16 colors (display_dm32.c, board.mk). The 16x32 font is the large class's
# channel name; text is anti-aliased, and lists and labels are one size up (9x18)
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
CONFIG_FONT_BIG ?= y
# The bezel hides the panel's edges, in panel pixels: text 6 px from the left or right edge was still clipped on the
# radio, so 10 px left and right (the 8 px measured on the right leaves no more 8 px columns), 5 px top and bottom
CONFIG_DISPLAY_INSET_LEFT ?= 10
CONFIG_DISPLAY_INSET_RIGHT ?= 10
CONFIG_DISPLAY_INSET_TOP ?= 5
CONFIG_DISPLAY_INSET_BOTTOM ?= 5
# Air band AM receive and the RDA5807M broadcast FM receiver (docs/spec/airband-and-broadcast.md, parked): the core
# is host tested, the board glue isn't written yet (no CAP_AM_RX / CAP_BCAST_FM, board.radio->rx_am, board.bcast)
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# VOX reads the microphone level through hal_power_t.mic_level, which this board doesn't have yet
CONFIG_VOX ?= n
# The platform's three TCP connections keep 11 KB of main RAM free; 4 leaves 9.6 KB. ci's maximal configuration has
# every switch but USB and USB_NETWORK (platform/hrc7000/config.mk), FONT_XL and AUDIO_PROBE too (9.9 KB of main RAM
# free), and NAND_STORAGE (below)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q128 class (docs/spec/hardware-dm32.md 10)
# What the hardware can do but this radio's driver or glue doesn't yet (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): off by default, and off in ci's maximal configuration too
BOARD_NOT_WRITTEN += AM_RX	# the stock firmware receives air band AM (hardware-dm32.md 8.7); board.radio->rx_am isn't written
BOARD_NOT_WRITTEN += BCAST_FM	# the RDA5807M is fitted (hardware-dm32.md 8.7); board.bcast isn't written
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
