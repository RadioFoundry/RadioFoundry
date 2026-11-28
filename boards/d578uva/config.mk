# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AT-D578UV feature defaults, Artery builds (config/features.mk lists them all). The radio's code is boards/d578uv's,
# and so are these switches and reasons, kept in step with boards/d578uv/config.mk (tools/boards.py reads each board's
# own file). Bring-up state: the bring-up records. The image gets 128 KB of
# SRAM (memory.ld), no CCM
# DMR runs through the SCT3258 baseband, which carries its own vocoder and plays the voice to the codec itself
# (CAP_MODEM_VOICE). Its driver (drivers/modem/sct), the voice set-up and the RF side (receive,
# the transmit keying and the PA per burst) are written
CONFIG_DMR ?= y
# What needs the voice frames on the MCU is not written (below)
CONFIG_HOTSPOT ?= n
CONFIG_SFR ?= n
CONFIG_DMR_PRIVACY ?= n
# P25 and NXDN: the SCT3258 gives no symbols, so the MCU samples the RF chip's demodulator on PC0 and the soft
# modem makes them (boards/d578uv/rxtap_d578uv.c); to transmit, the soft modem's samples go out as PWM on PB9 into the
# RF chip's line input (boards/d578uv/tone_d578uv.c). No voice: the MCU has no PCM path to the speaker or from the
# microphone (the baseband plays and takes DMR voice itself), so the P25 voice codec is left out and a P25 key-up
# sends silence. They
# take about 2.2 KB of RAM here (the sample ring is APRS receive's) and 1 KB more of RTOS heap (freertos_board.h)
BOARD_NO_PCM := y
# RAM decides what runs on the USB network adapter beside DMR, APRS receive, P25 and NXDN. The IP gateway (about
# 8.8 KB with the adapter), mDNS and TFTP (0.6 KB) and the control station mode fit, unlike on the GD32 builds, and so
# does zone sharing (about 4.7 KB). SNMP (about 2.7 KB) and P25 trunking (about 1.5 KB) no longer do. The TCP
# services don't: TCP alone takes about 4.7 KB, and the HTTP server, the web control, rigctld, KISS and APRS-IS run
# on it
CONFIG_SNMP ?= n
CONFIG_P25_TRUNK ?= n
CONFIG_NET_TCP ?= n
# USRP needs PCM both ways (below)
CONFIG_NET_USRP ?= n
# 160x128 like the other 160x128 radios
CONFIG_FONT_XL ?= n
# The core's storage in RadioFoundry's own NAND block range
CONFIG_NAND_STORAGE ?= y
# PTT and keys from the Bluetooth module on UART4 (PC10 / PC11). Found by asking it at start,
# so a radio without the module (not every model has it) only shows it absent
CONFIG_BT_PTT ?= y
# The temperature sensors and the fan (power_d578uv.c d578uv_temp)
CONFIG_TEMP ?= y
CONFIG_SATELLITE_TLE ?= n	# RAM: about 1.2 KB, and this build is about 0.9 KB short with APRS receive on
CONFIG_SIG_TONES ?= n	# RAM: about 0.7 KB, which P25 and NXDN take here
# Air band AM and broadcast FM receive run on the KTMicro receiver on PB6/PB7, AM behind side B's chips.
# APRS receive samples the receive audio on PC0 (boards/d578uv/afsk_d578uv.c)
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed")
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (a mobile)
# Left out of tools/ci.sh's maximal configuration: what doesn't fit the RAM with DMR on (above), SNMP, P25 trunking,
# TCP and the services on it, and the zone and channel control over UDP (about 100 bytes, built by default: the maximal
# set's languages and fonts take that RAM); satellite tracking and signaling tones (off by default, above); voice capture (off by default, a
# bench tool; about 330 bytes); and PPP, which stands in for the
# USB network adapter on radios without one
MAXIMAL_SKIP := NET_PPP NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_WEBCTL NET_RIGCTL NET_APRSIS SATELLITE_TLE \
	SNMP P25_TRUNK SIG_TONES VOICE_CAPTURE
# (they follow the IP gateway, the debug link and beeps by default, so the maximal configuration turns them off by
# name)
MAXIMAL_VARS := CONFIG_SATELLITE_TLE=n CONFIG_SIG_TONES=n
# What the hardware can do but this radio's driver or glue doesn't yet
BOARD_NOT_WRITTEN += HOTSPOT	# needs the voice frames on the MCU: the baseband's frame bypass isn't written
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs the hotspot (above)
BOARD_NOT_WRITTEN += SFR	# needs the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += SFR_DUALPATH_RX	# needs SFR (above)
BOARD_NOT_WRITTEN += SFR_CWID	# needs SFR (above)
BOARD_NOT_WRITTEN += DMR_PRIVACY	# scrambles the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe taps an audio chip's PCM path; the voice stays in the baseband here
BOARD_NOT_WRITTEN += NET_USRP	# needs an audio tap (PCM both ways): the voice stays in the baseband, only receive audio reaches the MCU
BOARD_REQUIRED := FM
