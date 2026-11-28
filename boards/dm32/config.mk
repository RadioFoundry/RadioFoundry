# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-32UV feature defaults (config/features.mk lists them all). 880 KB of code space before the codec slot, so RAM is
# the limit: 211,456 B below the codec's RAM window (boards/dm32/memory.ld, docs/spec/hardware-dm32.md 17.1), the
# RTOS heap's 33 KB among them (freertos_board.h). Every feature the radio has hardware and glue for is on, and the
# image (P25, P25 trunking and NXDN among them) keeps about 920 B of it free: a feature that needs more RAM cuts some first, or shrinks the heap if
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
# The frame buffer holds only the safe area inside those insets, 4 bits a pixel with each row rounded up to whole bytes:
# 220 x 310, 34,100 B at the defaults instead of the whole panel's 38,400. Nothing is drawn in the insets; the panel's
# own memory keeps their background. It is worked out here rather than written in board.mk because board.mk is read
# before this file, so a fixed number there fitted only the default insets: larger ones wasted RAM and smaller ones
# failed display_dm32.c's build-time check. tools/boards.py reads the number from MCU_DEFS, so it must be a literal
DM32_FB_BYTES := $(shell echo $$(( (240 - $(CONFIG_DISPLAY_INSET_LEFT) - $(CONFIG_DISPLAY_INSET_RIGHT) + 1) / 2 * \
	(320 - $(CONFIG_DISPLAY_INSET_TOP) - $(CONFIG_DISPLAY_INSET_BOTTOM)) )))
MCU_DEFS += -DGFX_FB_BYTES=$(DM32_FB_BYTES)u
# USRP voice over UDP: received audio alone is 352 B every 20 ms (about 17.6 KB/s before PPP, IP and UDP headers),
# more than the programming cable's 115200 baud PPP link carries (about 11.5 KB/s each way)
CONFIG_NET_USRP ?= n
# VOX reads the microphone level through hal_power_t.mic_level, which this board doesn't have yet
CONFIG_VOX ?= n
# The platform's three TCP connections; a fourth costs about 1.4 KB of main RAM. ci's maximal configuration has
# every switch but USB and USB_NETWORK (platform/hrc7000/config.mk), and NAND_STORAGE (below), and keeps about
# 520 B free
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q128 class (docs/spec/hardware-dm32.md 10)
# What the hardware can do but this radio's driver or glue doesn't yet (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): off by default, and off in ci's maximal configuration too
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
BOARD_NOT_WRITTEN += NET_USRP	# USRP's 8 kHz PCM both ways needs more than the host link's 115200 baud
