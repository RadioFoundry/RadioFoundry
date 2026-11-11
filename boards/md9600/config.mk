# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-9600 feature defaults (config/features.mk lists them all). State: docs/spec/hardware-md9600.md 19. Every
# board revision is built; V1-V3 is the one checked on a radio. Transmit is refused until a person arms it for the power
# cycle, the revision is a stored one and the PA calibration is plausible. DMR receive is built (the HR_C6000 glue, the radio's own stock codec where the user's
# machine merged it); DMR transmit is refused like FM. The switches that are off are listed below
#
# Every board revision's synthesizer driver, so one image runs on any MD-9600; turn revisions off for a smaller image
# for one board (rfplan_md9600.h)
CONFIG_MD9600_V1V3 ?= y
CONFIG_MD9600_V4 ?= y
CONFIG_MD9600_V5 ?= y
ifeq ($(CONFIG_MD9600_V1V3)$(CONFIG_MD9600_V4)$(CONFIG_MD9600_V5),nnn)
$(error the MD-9600 needs at least one of CONFIG_MD9600_V1V3, CONFIG_MD9600_V4 and CONFIG_MD9600_V5)
endif
# Transmit: FM and DMR, both bands, checked off air (docs/spec/hardware-md9600.md 19.8, 19.9). Built, it still needs
# the run-time arm for the power cycle, a stored board revision and plausible PA rows. n leaves a receive-only image
CONFIG_MD9600_TX ?= y
CONFIG_MD9600_DMR_TX ?= y
# No DMR, no DMR transmit: its per-burst keying uses the DMR key-up rule (core/radio/pa_keyup.c)
ifeq ($(CONFIG_DMR),n)
override CONFIG_MD9600_DMR_TX := n
endif
# This board's own switches (config_md9600.h), not feature switches: no other radio has them
MCU_DEFS += $(foreach s,V1V3 V4 V5 TX DMR_TX,-DCONFIG_MD9600_$(s)=$(if $(filter y,$(CONFIG_MD9600_$(s))),1,0))
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V1V3)),drivers/rf/sky72310)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V4)),drivers/rf/pll1590)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V5)),drivers/rf/lmx2571)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V1V3) $(CONFIG_MD9600_V4)),drivers/rf/ak2365a)
# The hotspot, SFR, the DMR IP gateway and its services, the crash report and the USB network adapter are built as on
# the other HR_C6000 radios; without CONFIG_MD9600_DMR_TX the transmitter refuses their transmissions
# The TX tones (the 1750 Hz burst, the roger beep) and DTMF come from the HR_C6000's tone generator during FM TX
# (c6000_md9600.c); without CONFIG_MD9600_TX they have no transmitter. APRS receives from the audio on PC3
# (afsk_md9600.c)
# Sub-audio: decoded on PA2, sent as PWM on PC7 (subtone_md9600.c)
# VOX reads the mic level on PA3 (power_md9600.c). The reference trim retunes the synthesizers (rfseq_md9600.c): the
# TCXO has no control line
# A 128x64 monochrome panel: no colors. No air band or broadcast receiver. The fault trace, the large font and LTO
# follow the platform (platform/stm32f4/config.mk)
CONFIG_UI_COLOR ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# What the hardware can't do, and what the hardware can do but this board's code doesn't yet, one switch per line with
# the reason (docs/build-config.md, "Unsupported, not written, unconfirmed"; tools/boards.py checks there is one). ci's
# maximal configuration leaves both lists off
BOARD_UNSUPPORTED += UI_COLOR	# a 128x64 monochrome panel
BOARD_UNSUPPORTED += FONT_XL	# the large font is for 240 px and wider panels; this one is 128x64
BOARD_UNSUPPORTED += FONT_AA	# a monochrome panel draws anti-aliased fonts as plain ink and gains nothing
BOARD_UNSUPPORTED += FONT_BIG	# for 240 px and wider color panels (and needs FONT_AA)
BOARD_UNSUPPORTED += AM_RX	# no air band receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the GPS receive line
# FM receive is the checked path: the matrix has no FM-less configuration
BOARD_REQUIRED := FM
# CONFIG_STUB_ENTRY is on with the host link (config/features.mk): the STM32F4 flash stub serves this radio through its
# board record (board.toml [flash.stub], docs/spec/stub-stm32f4.md)
# Supported, but the maximal configuration leaves these off too: PPP stands in for the USB network adapter on UART-only
# radios (docs/spec/ppp.md), and this one has the adapter
MAXIMAL_SKIP := NET_PPP
# English only (the owner's choice, 2026-11-03)
CONFIG_LANG_ES ?= n
