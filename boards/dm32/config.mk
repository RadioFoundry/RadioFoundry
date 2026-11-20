# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-32UV feature defaults (config/features.mk lists them all). 880 KB of code space before the codec slot, so RAM is
# the limit: 211,456 B below the codec's RAM window (boards/dm32/memory.ld, docs/spec/hardware-dm32.md 17.1), the
# RTOS heap's 33 KB among them (freertos_board.h). Every feature the radio has hardware and glue for is on, and the
# image (P25, P25 trunking and NXDN among them) keeps about 2.4 KB of it free: a feature that needs more RAM cuts some first, or shrinks the heap if
# tasks.heap_min_free shows room (tests/hil/test_task_stacks.py). The codec's encoder and decoder share one state block
# (platform/hrc7000/vocoder.c), which P25 borrows while it runs
# The host link runs on the programming cable's UART0; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
# The 240x320 panel at full resolution in 16 colors (display_dm32.c, board.mk). The 16x32 font is the large class's
# channel name; text is anti-aliased, and lists and labels are one size up (9x18)
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
# FONT_BIG needs FONT_AA: off with it
CONFIG_FONT_BIG ?= $(CONFIG_FONT_AA)
# The bezel hides the panel's edges, in panel pixels: text 6 px from the left or right edge was still clipped on the
# radio, so 10 px left and right (the 8 px measured on the right leaves no more 8 px columns), 5 px top and bottom
CONFIG_DISPLAY_INSET_LEFT ?= 10
CONFIG_DISPLAY_INSET_RIGHT ?= 10
CONFIG_DISPLAY_INSET_TOP ?= 5
CONFIG_DISPLAY_INSET_BOTTOM ?= 5
# D-STAR DV and Yaesu System Fusion (docs/spec/dstar-modem.md, ysf-modem.md; both on the flat FM paths in
# flat_dm32.c): on only where the build leaves the IP gateway out, as the multimode variant does. The default image
# keeps its RAM for the features it has (about 1.2 KB free), and the gateway's 17 KB is the room the two modes' engine
# (one, lent to the mode that runs: radio_mode_ram.h), buffers and screens take; D-STAR's codec borrows the DMR
# vocoder's state RAM, Fusion runs on the vocoder itself. Tested on the switch that makes the room rather than on the
# variant's name, so a hand-made build without the gateway gets them too (docs/build-config.md, Variants)
ifeq ($(CONFIG_DMR_IP_GATEWAY),n)
CONFIG_DSTAR ?= y
CONFIG_YSF ?= y
endif
# ci's maximal build has every switch on, and D-STAR and Fusion beside the gateway overflow RAM: it leaves the gateway
# out instead (the default build covers it), so both modes are built with everything else
# USB_SERIAL_REQUEST is left out by hand: with the gateway gone ci.sh's dependency loop drops NET_PPP first, and once it
# has dropped one switch it can no longer match the first in its list (fixed separately in ci.sh)
MAXIMAL_VARS := CONFIG_DMR_IP_GATEWAY=n CONFIG_USB_SERIAL_REQUEST=n
# VOX reads the microphone level through hal_power_t.mic_level, which this board doesn't have yet
CONFIG_VOX ?= n
# The platform's three TCP connections; a fourth costs about 1.4 KB of main RAM. ci's maximal configuration has
# every switch but USB and USB_NETWORK (platform/hrc7000/config.mk), and NAND_STORAGE (below), and keeps about
# 2 KB free
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q128 class (docs/spec/hardware-dm32.md 10)
# What the hardware can do but this radio's driver or glue doesn't yet (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): off by default, and off in ci's maximal configuration too
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# board.audio_tap has no probe hooks yet (only the flat paths, with D-STAR)
