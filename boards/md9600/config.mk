# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
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
# Transmit: off (config/features.mk) until a radio confirms the RF sequence; built, it still needs the run-time arm
CONFIG_MD9600_TX ?= n
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
# FM transmit is refused for now, so the TX tones, VOX and APRS (AFSK out through the FM transmitter) have no path
CONFIG_FM_TX_TONES ?= n
# No sub-audio decode or encode path written yet (the tone PWM on PC7, the discriminator samples on PC3)
CONFIG_SUBTONE ?= n
CONFIG_APRS_RX ?= n
# A 128x64 monochrome panel: no colors, no large font. No air band or broadcast receiver
CONFIG_UI_COLOR ?= n
CONFIG_FONT_XL ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# Not written for this radio yet (above) or no use on it: tools/ci.sh's maximal configuration leaves these off
BOARD_UNSUPPORTED := SUBTONE DMR_IP_GATEWAY DMR_UDP_COMPRESS NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP \
	NET_RIGCTL NET_APRSIS NET_USRP MDNS TFTP TFTP_IDDB DMR_CONTROL_STATION SNMP SFR HOTSPOT HOTSPOT_SPLIT \
	CRASH_REPORT USB_NETWORK NET_PPP FM_TX_TONES APRS_RX APRS_IGATE UI_COLOR FONT_XL FONT_AA FONT_BIG AM_RX BCAST_FM \
	VIBRATE NAND_STORAGE
# FM receive is the checked path: the matrix has no FM-less configuration
BOARD_REQUIRED := FM
# Supported but not needed on this radio: the stub entry (no flash route uses it here yet)
MAXIMAL_SKIP := STUB_ENTRY
