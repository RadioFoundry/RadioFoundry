# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# RT10 feature defaults (config/features.mk lists them all). The host link runs on the accessory jack's UART0; USB, PPP
# and TCP_CONNS follow the platform (platform/hrc7000/config.mk)
# DMR (hardware-rt10.md 7.1): the original firmware's scheme on the shared RF-burst task; voice needs the codec merged (8)
CONFIG_DMR ?= y
CONFIG_HOTSPOT_SPLIT ?= n
# PPP's host sends at any time, and the radio sending while the host sends latched the cable line low until a power
# cycle (hardware-rt10.md 3.1): off, and the IP gateway and its services with it. It builds, for a check with
# another cable
CONFIG_NET_PPP ?= n
BOARD_DEFAULT_OFF += NET_PPP	# Off by default on this radio due to a hardware flaw: if the computer and the radio send at the same time, the cable's PTT and serial line can lock up until the radio is switched off and on.
# The MMDVM host sends when it likes, as pppd does, so the hotspot may latch the line the same way: off until that is
# resolved. It builds, for the check
CONFIG_HOTSPOT ?= n
BOARD_DEFAULT_OFF += HOTSPOT	# Off by default on this radio due to a hardware flaw: if the computer and the radio send at the same time, the cable's PTT and serial line may lock up until the radio is switched off and on.
CONFIG_NET_USRP ?= n
CONFIG_VOX ?= n
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
# No display: what only shows text or takes it on a screen is left out
CONFIG_FONT_AA ?= n
CONFIG_FONT_BIG ?= n
CONFIG_LANG_ES ?= n
CONFIG_LANG_IT ?= n
CONFIG_UI_SETTINGS ?= n
CONFIG_CHANNEL_LIST ?= n
CONFIG_DMR_SMS ?= n
CONFIG_ZONE_SHARE ?= n
CONFIG_CRASH_SCREEN ?= n
CONFIG_DIAG ?= n
CONFIG_GPS ?= n
CONFIG_GPS_SKY ?= n
# No GPS receiver, so no position of its own to report. Custom builds can still turn these on
CONFIG_DMR_GPS ?= n
BOARD_DEFAULT_OFF += DMR_GPS	# Off by default on this radio: it has no GPS receiver or display, so it has no position to send or show.
CONFIG_DMR_LRRP ?= n
BOARD_DEFAULT_OFF += DMR_LRRP	# Off by default on this radio: it has no GPS receiver, so it could only answer that it has no position.
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# No display or keypad: no frequency to type in, no A / B channel to pick, no digits to dial
CONFIG_VFO ?= n
CONFIG_DUAL_WATCH ?= n
CONFIG_DTMF ?= n
BOARD_UNSUPPORTED += VFO	# no display or keypad: the knob picks programmed channels only
BOARD_UNSUPPORTED += VFO_FM	# the VFO (above)
BOARD_UNSUPPORTED += VFO_DMR	# the VFO (above)
BOARD_UNSUPPORTED += DUAL_WATCH	# no display: channel B can't be picked or shown
BOARD_UNSUPPORTED += DTMF	# no keypad: no digits to dial
BOARD_UNSUPPORTED += FONT_XL	# no display
BOARD_UNSUPPORTED += UI_COLOR	# no display: no colors to set
BOARD_UNSUPPORTED += FONT_AA	# no display
BOARD_UNSUPPORTED += FONT_BIG	# no display
BOARD_UNSUPPORTED += LANG_ES	# no display: the languages translate the screen's text
BOARD_UNSUPPORTED += LANG_IT	# no display (above)
BOARD_UNSUPPORTED += LANG_DEFAULT_ES	# no display (above)
BOARD_UNSUPPORTED += LANG_DEFAULT_IT	# no display (above)
BOARD_UNSUPPORTED += UI_SETTINGS	# no display: no Settings menu; the CPS and the debug link set the settings
BOARD_UNSUPPORTED += CHANNEL_LIST	# no display: no Channels list
BOARD_UNSUPPORTED += CHANNEL_EDIT	# no display: no channel editor or text entry
BOARD_UNSUPPORTED += ZONE_EDIT	# no display: no zone editor
BOARD_UNSUPPORTED += CONTACT_EDIT	# no display: no contact editor
BOARD_UNSUPPORTED += DMR_SMS	# no display: text messages can't be read or written
BOARD_UNSUPPORTED += DMR_TMS	# the TMS format of text messages (above)
BOARD_UNSUPPORTED += ZONE_SHARE	# no display: an offered zone can't be shown or accepted
BOARD_UNSUPPORTED += DIAG	# no display: the diagnostics page is a screen
BOARD_UNSUPPORTED += CRASH_SCREEN	# no display: the debug link reads the crash log
BOARD_UNSUPPORTED += CRASH_REPORT	# sent from the crash log's screen
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q16 class (docs/spec/hardware-rt10.md 4)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor
BOARD_UNSUPPORTED += AM_RX	# no air band receive path
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver
BOARD_UNSUPPORTED += GPS	# no GPS module
BOARD_UNSUPPORTED += GPS_SKY	# no GPS module
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# the per-burst TX path has no continuous base-station outbound
BOARD_NOT_WRITTEN += NET_USRP	# USRP's 8 kHz PCM both ways needs more than the host link's 115200 baud
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
# Bench knobs (dmr_rt10.c, dmr_rt10.h): RT10_DMR_RF_MODE (the modem's RF_MODE word), RT10_DMR_MIC_DB (its ADC gain in
# DMR), RT10_DMR_DAC_C (the AF ADC's bias in DMR)
ifneq ($(RT10_DMR_DAC_C),)
MCU_DEFS += -DRT10_DMR_DAC_C=$(RT10_DMR_DAC_C)u
endif
ifneq ($(RT10_DMR_RF_MODE),)
MCU_DEFS += -DRT10_DMR_RF_MODE=$(RT10_DMR_RF_MODE)
endif
ifneq ($(RT10_DMR_MIC_DB),)
MCU_DEFS += -DRT10_DMR_MIC_DB=$(RT10_DMR_MIC_DB)u
endif
