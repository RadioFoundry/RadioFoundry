# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## Feature switches (y / n). Precedence: the make command line, then boards/<board>/config.mk, then the defaults here.
##
##   make BOARD=<radio> CONFIG_DMR=n         an FM-only build
##   make BOARD=<radio> CONFIG_HOTSPOT=n     no MMDVM hotspot
##
## Each switch reaches C as CONFIG_<NAME> = 0 or 1 (always defined, so use #if, not #ifdef) and can leave whole
## source directories out of the build. Sub-features follow their parent unless set (CONFIG_DMR=n turns off every
## DMR feature; asking for a sub-feature without its parent is an error). See docs/build-config.md. Add a feature here, in core/config.h and in the doc.
##
## Policy: every feature is on by default wherever the radio has what it needs (hardware, a parent feature). A board
## turns features off only for hardware it lacks, or when the image doesn't fit; then the most obscure large features
## go first. Bench diagnostics (AUDIO_PROBE) stay off.

# analog FM receive and transmit
CONFIG_FM ?= y
# CTCSS / DCS encode and decode (needs FM)
CONFIG_SUBTONE ?= $(CONFIG_FM)
# DMR Tier II voice: modem driver, call stack, AMBE FEC, vocoder hosting
CONFIG_DMR ?= y
# basic privacy (Motorola / Anytone BP) on the voice path (needs DMR)
CONFIG_DMR_PRIVACY ?= $(CONFIG_DMR)
# talker alias decode and TX of our radio name (needs DMR)
CONFIG_DMR_TALKER_ALIAS ?= $(CONFIG_DMR)
# ETSI GPS Info in the embedded LC (FLCO 8): the caller's position decoded and shown, ours sent from the APRS
# position source when the radio-wide setting is on (needs DMR)
CONFIG_DMR_GPS ?= $(CONFIG_DMR)
# DMR text messages (SMS; needs DMR): ETSI IP text and ETSI defined short data, sent and received (docs/spec/dmr-data.md 5)
CONFIG_DMR_SMS ?= $(CONFIG_DMR)
# Motorola TMS format support for text messages: send, receive, the TMS ACK (needs DMR_SMS; docs/spec/dmr-data.md 5.1)
CONFIG_DMR_TMS ?= $(CONFIG_DMR_SMS)
# ARS registration with a presence server (needs DMR; docs/spec/dmr-data.md)
CONFIG_DMR_ARS ?= $(CONFIG_DMR)
# LRRP location request / response (needs DMR; uses GPS where the board has it, else answers "no fix")
CONFIG_DMR_LRRP ?= $(CONFIG_DMR)
# CSBK signaling: call alert, radio check (needs DMR)
CONFIG_DMR_SIGNALING ?= $(CONFIG_DMR)
# DMR emergency: the emergency alarm CSBK sent and heard, emergency calls (the LC's emergency service option, hot mic),
# the receive alert (needs DMR; core/dmr/dmr_emergency.c, docs/spec/dmr-emergency.md)
CONFIG_DMR_EMERGENCY ?= $(CONFIG_DMR)
# DMR repeater roaming: move between the repeater sites of a roaming zone by signal strength (needs DMR;
# core/radio/roam.c, docs/spec/dmr-roaming.md)
CONFIG_DMR_ROAMING ?= $(CONFIG_DMR)
# IP over DMR packet data through a network link to the host: the USB network adapter or PPP on the serial host port
# (needs DMR and USB_NETWORK or NET_PPP; core/hostlink/hostlink_net.c)
CONFIG_DMR_IP_GATEWAY ?= $(if $(and $(filter y,$(CONFIG_DMR)),$(filter y,$(CONFIG_USB_NETWORK) $(CONFIG_NET_PPP))),y,n)
# zone and channel sharing over DMR data: "Share zone to..." and offers of zones from other radios, imported only when
# the user accepts (needs DMR; core/dmr/zoneShare*.c, docs/spec/zone-share.md)
CONFIG_ZONE_SHARE ?= $(CONFIG_DMR)
# compressed UDP/IPv4 headers on transmit (SAP 3, ETSI TS 102 361-3 5.6 / 7.2): UDP packets whose header fits the
# compression rules go with a 5 to 9 octet header instead of 28 (needs DMR; docs/spec/dmr-data.md 11)
CONFIG_DMR_UDP_COMPRESS ?= $(CONFIG_DMR)
# confirmed packet data (ETSI TS 102 361-1 8.2 / -3 5.4): our IP packets confirmed per destination or UDP port (N(S),
# responses, selective retry, retry limit), and the selective retry asked for when confirmed data to us has bad blocks
# (needs DMR; docs/spec/dmr-data.md 12)
CONFIG_DMR_CONFIRMED ?= $(CONFIG_DMR)
# a minimal TCP for the radio's own services on the USB network link, never over RF (needs DMR_IP_GATEWAY, whose IPv4
# layer it uses; core/net, docs/spec/tcp-services.md)
CONFIG_NET_TCP ?= $(CONFIG_DMR_IP_GATEWAY)
# KISS over TCP on port 8001, bridged to the APRS KISS TNC (needs NET_TCP and APRS; core/net/kiss_tcp.c)
CONFIG_NET_KISS ?= $(if $(and $(filter y,$(CONFIG_NET_TCP)),$(filter y,$(CONFIG_APRS))),y,n)
# the HTTP status server on port 80: a page, JSON status and view sections, the screen (needs NET_TCP; core/net/http.c)
CONFIG_NET_HTTP ?= $(CONFIG_NET_TCP)
# the status server's static files (core/net/http_assets) stored gzip compressed at build time and served with
# Content-Encoding: gzip; off: stored and served as they are (needs NET_HTTP; tools/gen_http_assets.py)
CONFIG_NET_HTTP_GZIP ?= $(CONFIG_NET_HTTP)
# web control on the status server: /ws (WebSocket) for the radio's front panel page, keys, status and receive audio
# (needs NET_HTTP; core/net/ws.c, docs/spec/tcp-services.md 5)
CONFIG_NET_WEBCTL ?= $(CONFIG_NET_HTTP)
# Hamlib rigctld on port 4532: frequency, mode, PTT, S meter for loggers and SDR programs (needs NET_TCP;
# core/net/rigctld.c)
CONFIG_NET_RIGCTL ?= $(CONFIG_NET_TCP)
# the radio's own APRS-IS client through the host: our beacons and the iGate's lines, off until a server is set
# (needs NET_TCP and APRS; core/net/aprsis.c)
CONFIG_NET_APRSIS ?= $(if $(and $(filter y,$(CONFIG_NET_TCP)),$(filter y,$(CONFIG_APRS))),y,n)
# audio linking for AllStarLink, DVSwitch and bridges (USRP voice over UDP, port 34001): the radio sends what it
# receives, with the squelch as keyup, and transmits what the computer sends. DMR works both ways on every radio; FM
# needs the radio's audio paths to the processor, which radios have both ways, DMR only or FM one way
# (docs/feature-matrix.md) (needs DMR_IP_GATEWAY; core/usbaudio, docs/spec/usb-audio.md)
CONFIG_NET_USRP ?= $(CONFIG_DMR_IP_GATEWAY)
# mDNS / DNS-SD responder on the USB network link: <host>.local and the radio's services (needs DMR_IP_GATEWAY, the
# IP stack it runs on; core/net/mdns.c, docs/spec/mdns.md)
CONFIG_MDNS ?= $(CONFIG_DMR_IP_GATEWAY)
# TFTP server on the USB network link: codeplug.bin read and (staged, checked) written, info.txt (needs
# DMR_IP_GATEWAY; core/net/tftp.c, docs/spec/tftp.md)
CONFIG_TFTP ?= $(CONFIG_DMR_IP_GATEWAY)
# DMR ID database over TFTP: iddb.bin read and written in place, and the database part of a combined codeplug.bin
# (needs TFTP; core/net/tftp_iddb.c, docs/spec/tftp.md 5)
CONFIG_TFTP_IDDB ?= $(CONFIG_TFTP)
# the compact DMR ID database (format C): dictionary-coded names, IDs as gaps, pages with room to grow, read beside
# format N (needs DMR; core/codeplug/dmr_iddb.c, docs/spec/iddb-compact.md)
CONFIG_IDDB_COMPACT ?= $(CONFIG_DMR)
# control station mode for PC dispatch software on the USB network link: the PC as the endpoint of the radio's own
# data ports (passthrough) and a fixed private link address pair (needs DMR_IP_GATEWAY; docs/spec/usb-network.md 5)
CONFIG_DMR_CONTROL_STATION ?= $(CONFIG_DMR_IP_GATEWAY)
# SNMP agent (SNMPv2c) on UDP 161 of the radio's address, over the USB network and over the air, with notifications
# (needs DMR_IP_GATEWAY; core/snmp, docs/spec/snmp.md)
CONFIG_SNMP ?= $(CONFIG_DMR_IP_GATEWAY)
# single frequency repeater (needs DMR): the MMDVM hotspot's SFR mode (core/hotspot/hotspotSfr.c, core/dmr/sfrRunner.c)
# and the standalone repeater and user roles of a channel (core/radio/radio_sfr.c, docs/spec/sfr.md 4)
CONFIG_SFR ?= $(CONFIG_DMR)
# SFR user receive mode "dual-path RX": a voice frame whose FEC failed is taken from the other copy (needs SFR)
CONFIG_SFR_DUALPATH_RX ?= $(CONFIG_SFR)
# the standalone SFR repeater's periodic CW ID: its callsign in Morse, in FM on the channel (needs SFR)
CONFIG_SFR_CWID ?= $(CONFIG_SFR)
# USB device (CDC serial)
CONFIG_USB ?= y
# the host link (core/hostlink): MMDVM hotspot, CPS and debug link over the board's host port (USB CDC, or the
# programming cable's UART on radios without USB)
CONFIG_HOSTLINK ?= $(CONFIG_USB)
# USB network adapter personality (CDC-NCM, switched with the serial one at run time; needs USB)
CONFIG_USB_NETWORK ?= $(if $(and $(filter y,$(CONFIG_USB)),$(filter y,$(CONFIG_HOSTLINK))),y,n)
# IPv4 over PPP on the serial host port, into the IP gateway and its services: for radios whose only host link is a
# USB-serial cable, and a fallback on the USB serial port (needs HOSTLINK and DMR_IP_GATEWAY, which
# it turns on by default; core/net/ppp.c, core/hostlink/hostlink_ppp.c, docs/spec/ppp.md). On in the config.mk of
# a radio whose only IP link it is; off on the USB radios (docs/feature-matrix.md)
CONFIG_NET_PPP ?= n
# MMDVM modem / hotspot mode over the host port (needs DMR and HOSTLINK)
CONFIG_HOTSPOT ?= $(if $(and $(filter y,$(CONFIG_DMR)),$(filter y,$(CONFIG_HOSTLINK))),y,n)
# split duplex repeater: two radios as one duplex MMDVM modem through the hotspot's vendor command 0xD6 (needs
# HOTSPOT; core/hotspot/hotspotSplit.c, core/dmr/bsOutbound.c, core/dmr/slotEpoch.c, docs/spec/split-duplex.md)
CONFIG_HOTSPOT_SPLIT ?= $(CONFIG_HOTSPOT)
# VFO: tune a frequency by hand (off: programmed channels only)
CONFIG_VFO ?= y
# analog FM VFO (needs VFO and FM)
CONFIG_VFO_FM ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_FM))),y,n)
# digital DMR VFO: color code, timeslot and talk target typed in, the Mode tile switches it FM / DMR (needs VFO and DMR)
CONFIG_VFO_DMR ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_DMR))),y,n)
# the debug link: framed commands on the host port for bench automation (docs/spec/debuglink.md; needs HOSTLINK)
CONFIG_DEBUGLINK ?= $(CONFIG_HOSTLINK)
# every task's least free stack in the debug link's tasks view: the kernel's task create / delete trace hooks keep a
# table of the tasks (core/app/task_stack.c, docs/spec/debuglink.md 3.7; needs DEBUGLINK). About 0.6 KB
CONFIG_TASK_STACKS ?= $(CONFIG_DEBUGLINK)
# the open-firmware CPS protocol shim on the host port, so qdmr and the open-firmware CPS can read and write the
# codeplug (docs/spec/open-firmware-cps.md; needs HOSTLINK)
CONFIG_CPS_OPENFW ?= $(CONFIG_HOSTLINK)
# the fast codeplug protocol on the debug link's framing: blank blocks skipped, CRC-32 compare so only changed blocks
# move, LZ4 both ways, staged writes with a power-cut safe commit (docs/spec/cps-fast.md; needs CPS_OPENFW, and
# DEBUGLINK or STUB_ENTRY, whose own listener carries it without the debug link)
CONFIG_CPS_FAST ?= $(if $(filter y,$(CONFIG_CPS_OPENFW)),$(if $(filter y,$(CONFIG_DEBUGLINK) $(CONFIG_STUB_ENTRY)),y,n),n)
# the diagnostics page
CONFIG_DIAG ?= y
# 16x32 font, only useful on 240 px and wider displays
CONFIG_FONT_XL ?= n
# anti-aliased fonts from 6x12 up (JetBrains Mono Bold, 2 bits a pixel) instead of the bitmap ones: smooth text on color
# panels. About 5 KB more flash, 12 KB more with FONT_XL; a mono panel draws them as plain ink and gains nothing.
# On a 4 bpp color panel the edge grays share the 16 palette entries with the screen's colors: some font sizes or colors
# can be slightly harder to read (docs/spec/ui.md 5)
CONFIG_FONT_AA ?= n
# larger text on 240 px and wider color panels: lists, labels and the status strip in a 9x18 font instead of 8x16
# (12% larger, 24 characters a line on a 240 px panel instead of 27). About 3 KB more flash (needs FONT_AA)
CONFIG_FONT_BIG ?= n
# color screen settings: theme, highlight color, filled status bar (off: the dark theme only; for radios whose only
# screen is monochrome)
CONFIG_UI_COLOR ?= y
# the Settings menu on the radio (off: settings keep their stored values and are set with qdmr; docs/spec/ui.md)
CONFIG_UI_SETTINGS ?= y
# channel / zone scan, VFO band scan, dual watch, nuisance delete (docs/spec/radio-features.md)
CONFIG_SCAN ?= y
# the dual watch screens: two channels on Home, which one PTT uses and which one a call came in on; the radio core's
# dual watch across zones and with the VFO, PTT on the selected channel (needs SCAN; docs/ux/dual-watch.md)
CONFIG_DUAL_WATCH ?= $(CONFIG_SCAN)
# key assignment: Settings > Keys and the codeplug's quick-key slots say what the board's programmable keys do
# (core/input/keyassign.c, docs/spec/ui.md 3.1)
CONFIG_KEY_ASSIGN ?= y
# the hub's Channels list: every codeplug channel by number, and a channel's settings (read only; core/ui/ui_menus.c,
# docs/spec/ui.md 2)
CONFIG_CHANNEL_LIST ?= y
# editing a channel on the radio, New channel from current settings, keypad text entry (docs/spec/ui.md 2.1): with
# the Channels list
CONFIG_CHANNEL_EDIT ?= $(CONFIG_CHANNEL_LIST)
# the zone editor in a zone's list: add, move, copy here, remove channels; rename, copy, delete zones (docs/spec/ui.md
# 2.2): with the channel editor
CONFIG_ZONE_EDIT ?= $(CONFIG_CHANNEL_EDIT)
# the contact editor: a new contact from Find, a call's actions or a typed number, its name, ID, type and timeslot
# (docs/spec/ui.md 2.4): with the channel editor (its text entry) and DMR
CONFIG_CONTACT_EDIT ?= $(if $(and $(filter y,$(CONFIG_CHANNEL_EDIT)),$(filter y,$(CONFIG_DMR))),y,n)
# beeps and alert tones as PCM through the speaker (needs the board's PCM audio, which some radios have only with DMR)
CONFIG_BEEP ?= y
# FM transmit tones: roger beep and the 1750 Hz repeater access burst (needs FM)
CONFIG_FM_TX_TONES ?= $(CONFIG_FM)
# DTMF transmit: keypad digits send touch tones while PTT is held on FM (needs FM; docs/spec/radio-features.md 3.7)
CONFIG_DTMF ?= $(CONFIG_FM)
# FM VOX: voice-operated transmit from the microphone level (needs FM; unverified on the bench)
CONFIG_VOX ?= $(CONFIG_FM)
# reference oscillator trim: Settings > Advanced > Freq trim moves receive and transmit together (needs the board's
# hal_radio_t.ref_trim; docs/spec/radio-features.md 6.1)
CONFIG_REF_TRIM ?= y
# receiver battery saving: FM receiver duty cycling while idle (needs FM)
CONFIG_BATTERY_SAVE ?= $(CONFIG_FM)
# vibration alerts on the board's motor (hal_power_t.vibrate) for messages, call alerts, private calls and emergencies,
# Settings > Sound > Vibrate (core/radio/vibe.c, docs/spec/radio-features.md 4.1). On for radios with a motor (their
# config.mk); a board without one shows the setting dimmed
CONFIG_VIBRATE ?= n
# the GPS receiver: the board's module driver and its NMEA parsing (drivers/gps, platform/*/gps*, CAP_GPS). Off, the
# radio has no GPS: DMR_GPS, LRRP and APRS keep a fixed position and answer "no fix". Off where the radio has none
CONFIG_GPS ?= y
# air band AM receive: FM channels and the VFO in 108-137 MHz are received in AM, receive only, on radios whose
# receiver does AM (CAP_AM_RX; needs FM; docs/spec/airband-and-broadcast.md). Off where the receiver can't
CONFIG_AM_RX ?= $(CONFIG_FM)
# broadcast FM receive: FM channels and the VFO in the broadcast receiver's range play on that second receiver
# chip, receive only (board.bcast, CAP_BCAST_FM; docs/spec/airband-and-broadcast.md). Off where there is none
CONFIG_BCAST_FM ?= y
# audio path probe: debug link command 0x10 to read / write the audio chip's registers and watch the PCM stream's
# levels, for the USB audio feasibility tests on the bench (docs/spec/hardware-dm1701.md 21.3; needs DEBUGLINK and DMR, whose
# PCM stream it borrows). A diagnostic, never for a release
CONFIG_AUDIO_PROBE ?= n
# runtime RAM-monitor entry: command 0x1A on the host port loads the host's flash-access stub into free RAM and jumps
# to it without a power cycle, so updates need no key combo at power-on (the web flasher, stub/stubtool.py, rfoundry;
# core/stubentry, docs/spec/stub-entry.md). Its own feature: with the debug link off it answers on its own small
# listener (PING, VERSION, STUB). Needs HOSTLINK (the task that owns the port) and a platform with
# hal_system_t.enter_stub: the HR_C7000, and the STM32F4 with an 8 KB staging buffer (docs/spec/stub-stm32f4.md). On
# wherever the host link is; a board leaves it off in its config.mk only for a stated reason
CONFIG_STUB_ENTRY ?= $(CONFIG_HOSTLINK)
# the crash log: a CPU fault, a task stack overflow, running out of heap or a failed assertion keeps a record (registers,
# fault status, task, stack words) where a reset doesn't clear it and restarts the radio; the next start shows it
# (core/crash, docs/spec/crash-log.md). Off: a crash stops the radio until a power cycle, as before. Needs nothing else
CONFIG_CRASH_LOG ?= y
# the crash log's screen: Settings > Advanced > Crash log, the record's summary and Clear (needs CRASH_LOG). Off, the
# debug link still shows the record
CONFIG_CRASH_SCREEN ?= $(CONFIG_CRASH_LOG)
# the crash report: the crash log's record, packed (76-124 bytes), as one UDP datagram over DMR packet data to a
# codeplug contact, from the Crash log screen or after the restart (off / ask / automatic; docs/spec/crash-log.md 5)
# (needs CRASH_SCREEN and DMR)
CONFIG_CRASH_REPORT ?= $(if $(and $(filter y,$(CONFIG_CRASH_SCREEN)),$(filter y,$(CONFIG_DMR))),y,n)
# the hardware watchdog: started in main(), fed by the idle task, so a hang or a starved system of a few seconds resets
# the chip (the IWDG on the STM32F4, the SoC's watchdog on the HR_C7000; docs/spec/watchdog.md). With the debug link it
# also has a bench test (command 0x1D). Off: no watchdog runs, as before. Needs nothing else
CONFIG_WATCHDOG ?= y
# the fault trace: a CPU fault prints its registers on the host UART, three framed lines a second apart, before the
# restart (core/crash/crash_trace.c, tools/rfoundry_fault_trace.py; docs/spec/crash-log.md 2.3). Only where the host port is
# a UART the fault handler can drive polled: the HR_C7000 radios (the STM32 radios' config.mk turn it off: USB only).
# Off: straight to the record and the restart. Needs nothing else
CONFIG_FAULT_TRACE ?= y
# the core's storage (settings, codeplug) on SPI NAND: the NOR-like 4 KB sector layer over the board's own block range
# (drivers/flash/nand_sectors, docs/spec/nand-sectors.md). On in the config.mk of radios whose data flash is SPI NAND
# (docs/feature-matrix.md); off, such a radio runs on the default settings and stores nothing. Radios with SPI NOR don't use it
CONFIG_NAND_STORAGE ?= n

# APRS (core/aprs, docs/spec/aprs.md): position, status and message packets as AX.25 / 1200 baud AFSK on FM, positions
# over DMR (LRRP / UDT), a KISS TNC on the host port. Transmit stays off until the user turns it on (needs FM)
CONFIG_APRS ?= $(CONFIG_FM)
# APRS receive: the AFSK demodulator, station list, messages to us, digipeater (needs APRS; useful only on boards
# whose board.afsk can sample received audio)
CONFIG_APRS_RX ?= $(CONFIG_APRS)
# APRS iGate rules, RF <-> APRS-IS through the host helper tools/aprs_gate.py (needs APRS_RX and HOSTLINK)
CONFIG_APRS_IGATE ?= $(if $(and $(filter y,$(CONFIG_APRS_RX)),$(filter y,$(CONFIG_HOSTLINK))),y,n)
# the GPS status screen: satellites on a sky plot and signal bars, fix quality, grid locator (core/aprs/nmea_sky.c,
# core/ui/ui_gps.c, docs/spec/aprs.md 7.1). Needs GPS and APRS; off, Tools > Position is a page of text
CONFIG_GPS_SKY ?= $(if $(and $(filter y,$(CONFIG_GPS)),$(filter y,$(CONFIG_APRS))),y,n)


FEATURES := FM SUBTONE DMR DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_TMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY ZONE_SHARE DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_WEBCTL NET_RIGCTL NET_APRSIS NET_USRP MDNS TFTP TFTP_IDDB IDDB_COMPACT DMR_CONTROL_STATION SNMP SFR SFR_DUALPATH_RX SFR_CWID TASK_STACKS \
	USB HOSTLINK USB_NETWORK NET_PPP HOTSPOT HOTSPOT_SPLIT DEBUGLINK CPS_OPENFW CPS_FAST VFO VFO_FM VFO_DMR DIAG FONT_XL FONT_AA FONT_BIG UI_COLOR UI_SETTINGS SCAN DUAL_WATCH BEEP FM_TX_TONES DTMF \
	VOX REF_TRIM BATTERY_SAVE VIBRATE GPS AUDIO_PROBE STUB_ENTRY NAND_STORAGE CRASH_LOG CRASH_SCREEN CRASH_REPORT WATCHDOG FAULT_TRACE AM_RX BCAST_FM APRS APRS_RX APRS_IGATE GPS_SKY KEY_ASSIGN CHANNEL_LIST CHANNEL_EDIT ZONE_EDIT CONTACT_EDIT

# Dependencies
feature_on = $(filter y,$(CONFIG_$(1)))
ifneq ($(call feature_on,SUBTONE),)
  ifeq ($(call feature_on,FM),)
    $(error CONFIG_SUBTONE=y needs CONFIG_FM=y)
  endif
endif
$(foreach f,IDDB_COMPACT DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY ZONE_SHARE DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_RIGCTL NET_APRSIS NET_USRP MDNS TFTP SFR HOTSPOT,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,DMR),,x)),$(error CONFIG_$(f)=y needs CONFIG_DMR=y)))
$(foreach f,SFR_DUALPATH_RX SFR_CWID,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,SFR),,x)),$(error CONFIG_$(f)=y needs CONFIG_SFR=y)))
ifneq ($(call feature_on,CHANNEL_EDIT),)
  ifeq ($(call feature_on,CHANNEL_LIST),)
    $(error CONFIG_CHANNEL_EDIT=y needs CONFIG_CHANNEL_LIST=y)
  endif
endif
ifneq ($(call feature_on,ZONE_EDIT),)
  ifeq ($(call feature_on,CHANNEL_EDIT),)
    $(error CONFIG_ZONE_EDIT=y needs CONFIG_CHANNEL_EDIT=y)
  endif
endif
ifneq ($(call feature_on,CONTACT_EDIT),)
  ifeq ($(and $(call feature_on,CHANNEL_EDIT),$(call feature_on,DMR)),)
    $(error CONFIG_CONTACT_EDIT=y needs CONFIG_CHANNEL_EDIT=y and CONFIG_DMR=y)
  endif
endif
ifneq ($(call feature_on,DUAL_WATCH),)
  ifeq ($(call feature_on,SCAN),)
    $(error CONFIG_DUAL_WATCH=y needs CONFIG_SCAN=y)
  endif
endif
ifneq ($(call feature_on,FONT_BIG),)
  ifeq ($(call feature_on,FONT_AA),)
    $(error CONFIG_FONT_BIG=y needs CONFIG_FONT_AA=y)
  endif
endif
ifneq ($(call feature_on,HOTSPOT),)
  ifeq ($(call feature_on,HOSTLINK),)
    $(error CONFIG_HOTSPOT=y needs CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,HOTSPOT_SPLIT),)
  ifeq ($(call feature_on,HOTSPOT),)
    $(error CONFIG_HOTSPOT_SPLIT=y needs CONFIG_HOTSPOT=y)
  endif
endif
ifneq ($(call feature_on,DEBUGLINK),)
  ifeq ($(call feature_on,HOSTLINK),)
    $(error CONFIG_DEBUGLINK=y needs CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,TASK_STACKS),)
  ifeq ($(call feature_on,DEBUGLINK),)
    $(error CONFIG_TASK_STACKS=y needs CONFIG_DEBUGLINK=y)
  endif
endif
ifneq ($(call feature_on,CPS_OPENFW),)
  ifeq ($(call feature_on,HOSTLINK),)
    $(error CONFIG_CPS_OPENFW=y needs CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,CPS_FAST),)
  ifeq ($(call feature_on,CPS_OPENFW),)
    $(error CONFIG_CPS_FAST=y needs CONFIG_CPS_OPENFW=y)
  endif
  ifeq ($(call feature_on,DEBUGLINK)$(call feature_on,STUB_ENTRY),)
    $(error CONFIG_CPS_FAST=y needs CONFIG_DEBUGLINK=y or CONFIG_STUB_ENTRY=y)
  endif
endif
ifneq ($(call feature_on,USB_NETWORK),)
  ifeq ($(call feature_on,USB)$(call feature_on,HOSTLINK),yy)
  else
    $(error CONFIG_USB_NETWORK=y needs CONFIG_USB=y and CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,DMR_IP_GATEWAY),)
  ifeq ($(call feature_on,USB_NETWORK)$(call feature_on,NET_PPP),)
    $(error CONFIG_DMR_IP_GATEWAY=y needs CONFIG_USB_NETWORK=y or CONFIG_NET_PPP=y)
  endif
endif
ifneq ($(call feature_on,NET_PPP),)
  ifeq ($(call feature_on,HOSTLINK)$(call feature_on,DMR_IP_GATEWAY),yy)
  else
    $(error CONFIG_NET_PPP=y needs CONFIG_HOSTLINK=y and CONFIG_DMR_IP_GATEWAY=y)
  endif
endif
ifneq ($(call feature_on,NET_TCP),)
  ifeq ($(call feature_on,DMR_IP_GATEWAY),)
    $(error CONFIG_NET_TCP=y needs CONFIG_DMR_IP_GATEWAY=y)
  endif
endif
$(foreach f,NET_HTTP_GZIP NET_WEBCTL,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,NET_HTTP),,x)),$(error CONFIG_$(f)=y needs CONFIG_NET_HTTP=y)))
$(foreach f,NET_HTTP NET_RIGCTL,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,NET_TCP),,x)),$(error CONFIG_$(f)=y needs CONFIG_NET_TCP=y)))
$(foreach f,NET_KISS NET_APRSIS,$(if $(call feature_on,$(f)),$(if $(and $(call feature_on,NET_TCP),$(call feature_on,APRS)),,$(error CONFIG_$(f)=y needs CONFIG_NET_TCP=y and CONFIG_APRS=y))))
$(foreach f,MDNS TFTP NET_USRP,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,DMR_IP_GATEWAY),,x)),$(error CONFIG_$(f)=y needs CONFIG_DMR_IP_GATEWAY=y)))
$(foreach f,TFTP_IDDB,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,TFTP),,x)),$(error CONFIG_$(f)=y needs CONFIG_TFTP=y)))
ifneq ($(call feature_on,DMR_TMS),)
  ifeq ($(call feature_on,DMR_SMS),)
    $(error CONFIG_DMR_TMS=y needs CONFIG_DMR_SMS=y)
  endif
endif
ifneq ($(call feature_on,DMR_CONTROL_STATION),)
  ifeq ($(call feature_on,DMR_IP_GATEWAY),)
    $(error CONFIG_DMR_CONTROL_STATION=y needs CONFIG_DMR_IP_GATEWAY=y)
  endif
endif
ifneq ($(call feature_on,SNMP),)
  ifeq ($(call feature_on,DMR_IP_GATEWAY),)
    $(error CONFIG_SNMP=y needs CONFIG_DMR_IP_GATEWAY=y)
  endif
endif
ifneq ($(call feature_on,VFO_FM),)
  ifeq ($(call feature_on,VFO),)
    $(error CONFIG_VFO_FM=y needs CONFIG_VFO=y)
  endif
  ifeq ($(call feature_on,FM),)
    $(error CONFIG_VFO_FM=y needs CONFIG_FM=y)
  endif
endif
ifneq ($(call feature_on,VFO_DMR),)
  ifeq ($(call feature_on,VFO),)
    $(error CONFIG_VFO_DMR=y needs CONFIG_VFO=y)
  endif
  ifeq ($(call feature_on,DMR),)
    $(error CONFIG_VFO_DMR=y needs CONFIG_DMR=y)
  endif
endif
$(foreach f,FM_TX_TONES DTMF VOX BATTERY_SAVE AM_RX APRS,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,FM),,x)),$(error CONFIG_$(f)=y needs CONFIG_FM=y)))
ifneq ($(call feature_on,AUDIO_PROBE),)
  ifeq ($(call feature_on,DEBUGLINK)$(call feature_on,DMR),yy)
  else
    $(error CONFIG_AUDIO_PROBE=y needs CONFIG_DEBUGLINK=y and CONFIG_DMR=y)
  endif
endif
ifneq ($(call feature_on,STUB_ENTRY),)
  ifeq ($(call feature_on,HOSTLINK),)
    $(error CONFIG_STUB_ENTRY=y needs CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,CRASH_SCREEN),)
  ifeq ($(call feature_on,CRASH_LOG),)
    $(error CONFIG_CRASH_SCREEN=y needs CONFIG_CRASH_LOG=y)
  endif
endif
ifneq ($(call feature_on,CRASH_REPORT),)
  ifeq ($(call feature_on,CRASH_SCREEN)$(call feature_on,DMR),yy)
  else
    $(error CONFIG_CRASH_REPORT=y needs CONFIG_CRASH_SCREEN=y and CONFIG_DMR=y)
  endif
endif
ifneq ($(call feature_on,APRS_RX),)
  ifeq ($(call feature_on,APRS),)
    $(error CONFIG_APRS_RX=y needs CONFIG_APRS=y)
  endif
endif
ifneq ($(call feature_on,APRS_IGATE),)
  ifeq ($(and $(call feature_on,APRS_RX),$(call feature_on,HOSTLINK)),)
    $(error CONFIG_APRS_IGATE=y needs CONFIG_APRS_RX=y and CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,GPS_SKY),)
  ifeq ($(and $(call feature_on,GPS),$(call feature_on,APRS)),)
    $(error CONFIG_GPS_SKY=y needs CONFIG_GPS=y and CONFIG_APRS=y)
  endif
endif
ifeq ($(call feature_on,FM)$(call feature_on,DMR),)
  $(error at least one of CONFIG_FM and CONFIG_DMR must be y)
endif

# Derived: the shared packet data / CSBK code is built when any data feature is on (the emergency alarm is a CSBK)
CONFIG_DMR_DATA_CORE := $(if $(strip $(foreach f,DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_IP_GATEWAY ZONE_SHARE CRASH_REPORT,$(call feature_on,$(f)))),y,n)

FEATURE_DEFS := $(foreach f,$(FEATURES) DMR_DATA_CORE,-DCONFIG_$(f)=$(if $(call feature_on,$(f)),1,0))

## Numeric build values (board config.mk sets the defaults; the command line overrides)
# Display safe area in pixels: edges hidden by the bezel or lens. CONFIG_DISPLAY_INSET sets all four
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_BOTTOM ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_LEFT ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_RIGHT ?= $(CONFIG_DISPLAY_INSET)
# The air queue's buffer in octets (IP gateway and zone sharing, docs/spec/dmr-data.md 12): queued packets take
# their own length plus a 36-octet header. 2048 holds two longest packets (612) with room to spare, a zone's four
# fragments, or about thirteen 120-octet datagrams
CONFIG_DMR_AIR_QUEUE_BYTES ?= 2048
# The gap the data service keeps after our own data transmission before it keys up for another, in ms (0: none;
# docs/spec/dmr-data.md 12.6). A workaround: receivers lost a packet that came in a new transmission right after
# ours ended, cause not yet known; packets to the same destination go in one transmission instead, with no gap
CONFIG_DMR_DATA_TX_GUARD_MS ?= 250
VALUES := DISPLAY_INSET_TOP DISPLAY_INSET_BOTTOM DISPLAY_INSET_LEFT DISPLAY_INSET_RIGHT DMR_AIR_QUEUE_BYTES \
	  DMR_DATA_TX_GUARD_MS
FEATURE_DEFS += $(foreach v,$(VALUES),-DCONFIG_$(v)=$(strip $(CONFIG_$(v))))
