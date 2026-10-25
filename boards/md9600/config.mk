# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-9600 feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-md9600.md 19):
# display, keys, encoder, power key and standby, supply reading, data flash, USB host link and the FM receive path for
# every board revision. Nothing has run on a radio yet. Transmit is refused until the revision is confirmed and the
# PA calibration can be read. DMR receive is built (the HR_C6000 glue, the radio's own stock codec where the user's
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
# Transmit: off until a radio confirms the RF sequence; built, it still needs the run-time arm. DMR transmit (the
# HR_C6000's per-burst RF keying) stays off until the radio code keys the RF chain (docs/spec/hardware-md9600.md 4.4)
CONFIG_MD9600_TX ?= n
CONFIG_MD9600_DMR_TX ?= n
# This board's own switches (config_md9600.h), not feature switches: no other radio has them
MCU_DEFS += $(foreach s,V1V3 V4 V5 TX DMR_TX,-DCONFIG_MD9600_$(s)=$(if $(filter y,$(CONFIG_MD9600_$(s))),1,0))
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V1V3)),drivers/rf/sky72310)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V4)),drivers/rf/pll1590)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V5)),drivers/rf/lmx2571)
BOARD_DIRS += $(if $(filter y,$(CONFIG_MD9600_V1V3) $(CONFIG_MD9600_V4)),drivers/rf/ak2365a)
# DMR receive on the shared HR_C6000 glue. The DMR transmit glue (CONFIG_MD9600_DMR_TX) stays off, and the hotspot,
# which needs the transmitter, too
CONFIG_HOTSPOT ?= n
# The USB network adapter carries the DMR IP gateway's services, which wait for DMR transmit
CONFIG_USB_NETWORK ?= n
CONFIG_NET_PPP ?= n
# FM transmit is refused for now, so the TX tones and DTMF have no path. APRS receive: the radio has the receive audio
# on PC3, but the board doesn't feed it samples yet
CONFIG_FM_TX_TONES ?= n
CONFIG_DTMF ?= n
CONFIG_APRS_RX ?= n
# No sub-audio decode or encode path written yet (the tone PWM on PC7, the discriminator samples on PC3)
CONFIG_SUBTONE ?= n
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
BOARD_NOT_WRITTEN += SUBTONE	# the sub-audio paths (the tone PWM on PC7, the discriminator samples on PC3)
BOARD_NOT_WRITTEN += FM_TX_TONES	# FM transmit is refused until a radio confirms the RF sequence
BOARD_NOT_WRITTEN += DTMF	# the tones go out through the FM transmitter, refused for now (above)
BOARD_NOT_WRITTEN += APRS_RX	# the receive audio (PC3) isn't fed to the AFSK decoder
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive (above)
BOARD_NOT_WRITTEN += HOTSPOT	# needs the transmitter (CONFIG_MD9600_TX, CONFIG_MD9600_DMR_TX), refused for now
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs the hotspot (above)
BOARD_NOT_WRITTEN += SFR	# repeats on air: waits for DMR transmit
BOARD_NOT_WRITTEN += CRASH_REPORT	# sends over DMR packet data: waits for DMR transmit
BOARD_NOT_WRITTEN += DMR_IP_GATEWAY	# the IP services send over DMR: waits for DMR transmit
BOARD_NOT_WRITTEN += DMR_UDP_COMPRESS	# compresses transmitted headers: waits for DMR transmit
BOARD_NOT_WRITTEN += USB_NETWORK	# carries the DMR IP gateway's services (above)
BOARD_NOT_WRITTEN += NET_PPP	# carries the DMR IP gateway's services (above)
BOARD_NOT_WRITTEN += NET_TCP	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += NET_KISS	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += NET_HTTP	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += NET_HTTP_GZIP	# needs NET_HTTP (above)
BOARD_NOT_WRITTEN += NET_RIGCTL	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += NET_APRSIS	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += NET_USRP	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += MDNS	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += TFTP	# an IP service: waits for the DMR IP gateway (above)
BOARD_NOT_WRITTEN += TFTP_IDDB	# needs TFTP (above)
BOARD_NOT_WRITTEN += DMR_CONTROL_STATION	# needs the DMR IP gateway (above)
BOARD_NOT_WRITTEN += SNMP	# needs the DMR IP gateway (above)
# FM receive is the checked path: the matrix has no FM-less configuration
BOARD_REQUIRED := FM
# CONFIG_STUB_ENTRY is on with the host link (config/features.mk): the STM32F4 flash stub serves this radio through its
# board record (board.toml [flash.stub], docs/spec/stub-stm32f4.md)
