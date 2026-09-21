# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## Feature switches (y / n). Precedence: the make command line, then boards/<board>/config.mk, then the defaults here.
##
##   make BOARD=dm1701 CONFIG_DMR=n          an FM-only build
##   make BOARD=dm1701 CONFIG_HOTSPOT=n      no MMDVM hotspot
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
# DMR text messages (SMS; needs DMR; not ported yet)
CONFIG_DMR_SMS ?= $(CONFIG_DMR)
# ARS registration with a dispatcher (needs DMR; not ported yet)
CONFIG_DMR_ARS ?= $(CONFIG_DMR)
# LRRP location request / response (needs DMR; uses GPS where the board has it; not ported yet)
CONFIG_DMR_LRRP ?= $(CONFIG_DMR)
# CSBK signaling: call alert, radio check (needs DMR; not ported yet)
CONFIG_DMR_SIGNALING ?= $(CONFIG_DMR)
# DMR emergency: the emergency alarm CSBK sent and heard, emergency calls (the LC's emergency service option, hot mic),
# the receive alert (needs DMR; core/dmr/dmr_emergency.c, docs/spec/dmr-emergency.md)
CONFIG_DMR_EMERGENCY ?= $(CONFIG_DMR)
# DMR repeater roaming: move between the repeater sites of a roaming zone by signal strength (needs DMR;
# core/radio/roam.c, docs/spec/dmr-roaming.md)
CONFIG_DMR_ROAMING ?= $(CONFIG_DMR)
# IP over DMR packet data through the USB network adapter (needs DMR and USB_NETWORK; core/hostlink/hostlink_net.c)
CONFIG_DMR_IP_GATEWAY ?= $(if $(and $(filter y,$(CONFIG_DMR)),$(filter y,$(CONFIG_USB_NETWORK))),y,n)
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
# Hamlib rigctld on port 4532: frequency, mode, PTT, S meter for loggers and SDR programs (needs NET_TCP;
# core/net/rigctld.c)
CONFIG_NET_RIGCTL ?= $(CONFIG_NET_TCP)
# the radio's own APRS-IS client through the host: our beacons and the iGate's lines, off until a server is set
# (needs NET_TCP and APRS; core/net/aprsis.c)
CONFIG_NET_APRSIS ?= $(if $(and $(filter y,$(CONFIG_NET_TCP)),$(filter y,$(CONFIG_APRS))),y,n)
# single frequency repeater (needs DMR): the MMDVM hotspot's SFR mode (core/hotspot/hotspotSfr.c, core/dmr/sfrRunner.c);
# the stand-alone repeater and user roles on a channel are not wired yet (docs/spec/sfr.md)
CONFIG_SFR ?= $(CONFIG_DMR)
# USB device (CDC serial)
CONFIG_USB ?= y
# the host link (core/hostlink): MMDVM hotspot, CPS and debug link over the board's host port (USB CDC, or the
# programming cable's UART on radios without USB, e.g. the DM-32)
CONFIG_HOSTLINK ?= $(CONFIG_USB)
# USB network adapter personality (CDC-NCM, switched with the serial one at run time; needs USB)
CONFIG_USB_NETWORK ?= $(if $(and $(filter y,$(CONFIG_USB)),$(filter y,$(CONFIG_HOSTLINK))),y,n)
# MMDVM modem / hotspot mode over the host port (needs DMR and HOSTLINK)
CONFIG_HOTSPOT ?= $(if $(and $(filter y,$(CONFIG_DMR)),$(filter y,$(CONFIG_HOSTLINK))),y,n)
# split duplex repeater: two radios as one duplex MMDVM modem through the hotspot's vendor command 0xD6 (needs
# HOTSPOT; core/hotspot/hotspotSplit.c, core/dmr/bsOutbound.c, core/dmr/slotEpoch.c, docs/spec/split-duplex.md)
CONFIG_HOTSPOT_SPLIT ?= $(CONFIG_HOTSPOT)
# VFO: tune a frequency by hand (off: programmed channels only)
CONFIG_VFO ?= y
# analog FM VFO (needs VFO and FM)
CONFIG_VFO_FM ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_FM))),y,n)
# digital DMR VFO: own ID, color code, timeslot, talk target typed in (needs VFO and DMR; not built yet)
CONFIG_VFO_DMR ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_DMR))),y,n)
# the debug link: framed commands on the host port for bench automation (docs/spec/debuglink.md; needs HOSTLINK)
CONFIG_DEBUGLINK ?= $(CONFIG_HOSTLINK)
# the legacy CPS protocol shim on the host port, so qdmr and the legacy CPS can read and write the codeplug
# (docs/spec/legacy-cps-interop.md; needs HOSTLINK)
CONFIG_CPS_LEGACY ?= $(CONFIG_HOSTLINK)
# the diagnostics page
CONFIG_DIAG ?= y
# 16x32 font, only useful on 240 px and wider displays
CONFIG_FONT_XL ?= n
# the Settings menu on the radio (off: settings keep their stored values and are set with qdmr; docs/spec/ui.md)
CONFIG_UI_SETTINGS ?= y
# channel / zone scan, VFO band scan, dual watch, nuisance delete (docs/spec/radio-features.md)
CONFIG_SCAN ?= y
# beeps and alert tones as PCM through the speaker (needs the board's PCM audio, which the DM-1701 has with DMR)
CONFIG_BEEP ?= y
# FM transmit tones: roger beep and the 1750 Hz repeater access burst (needs FM)
CONFIG_FM_TX_TONES ?= $(CONFIG_FM)
# FM VOX: voice-operated transmit from the microphone level (needs FM; unverified on the bench)
CONFIG_VOX ?= $(CONFIG_FM)
# receiver battery saving: FM receiver duty cycling while idle (needs FM)
CONFIG_BATTERY_SAVE ?= $(CONFIG_FM)
# air band AM receive: FM channels and the VFO in 108-137 MHz are received in AM, receive only, on boards with
# CAP_AM_RX (needs FM; docs/spec/airband-and-broadcast.md). Off where the board can't: on for the DM-32UV
CONFIG_AM_RX ?= n
# broadcast FM receiver: Tools > FM radio on a second receiver (board.bcast), paused for channel traffic
# (core/bcast, docs/spec/airband-and-broadcast.md). Off where the board has none: on for the DM-32UV
CONFIG_BCAST_FM ?= n
# audio path probe: debug link command 0x10 to read / write the audio chip's registers and watch the PCM stream's
# levels, for the USB audio feasibility tests on the bench (docs/spec/usb-audio.md 2.3; needs DEBUGLINK and DMR, whose
# PCM stream it borrows). A diagnostic, never for a release
CONFIG_AUDIO_PROBE ?= n

# APRS (core/aprs, docs/spec/aprs.md): position, status and message packets as AX.25 / 1200 baud AFSK on FM, positions
# over DMR (LRRP / UDT), a KISS TNC on the host port. Transmit stays off until the user turns it on (needs FM)
CONFIG_APRS ?= $(CONFIG_FM)
# APRS receive: the AFSK demodulator, station list, messages to us, digipeater (needs APRS; useful only on boards
# whose board.afsk can sample received audio)
CONFIG_APRS_RX ?= $(CONFIG_APRS)
# APRS iGate rules, RF <-> APRS-IS through the host helper tools/aprs_gate.py (needs APRS_RX and HOSTLINK)
CONFIG_APRS_IGATE ?= $(if $(and $(filter y,$(CONFIG_APRS_RX)),$(filter y,$(CONFIG_HOSTLINK))),y,n)


FEATURES := FM SUBTONE DMR DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_RIGCTL NET_APRSIS SFR \
	USB HOSTLINK USB_NETWORK HOTSPOT HOTSPOT_SPLIT DEBUGLINK CPS_LEGACY VFO VFO_FM VFO_DMR DIAG FONT_XL UI_SETTINGS SCAN BEEP FM_TX_TONES \
	VOX BATTERY_SAVE AUDIO_PROBE AM_RX BCAST_FM APRS APRS_RX APRS_IGATE

# Dependencies
feature_on = $(filter y,$(CONFIG_$(1)))
ifneq ($(call feature_on,SUBTONE),)
  ifeq ($(call feature_on,FM),)
    $(error CONFIG_SUBTONE=y needs CONFIG_FM=y)
  endif
endif
$(foreach f,DMR_PRIVACY DMR_TALKER_ALIAS DMR_GPS DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_ROAMING DMR_IP_GATEWAY DMR_UDP_COMPRESS DMR_CONFIRMED NET_TCP NET_KISS NET_HTTP NET_RIGCTL NET_APRSIS SFR HOTSPOT,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,DMR),,x)),$(error CONFIG_$(f)=y needs CONFIG_DMR=y)))
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
ifneq ($(call feature_on,CPS_LEGACY),)
  ifeq ($(call feature_on,HOSTLINK),)
    $(error CONFIG_CPS_LEGACY=y needs CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,USB_NETWORK),)
  ifeq ($(call feature_on,USB)$(call feature_on,HOSTLINK),yy)
  else
    $(error CONFIG_USB_NETWORK=y needs CONFIG_USB=y and CONFIG_HOSTLINK=y)
  endif
endif
ifneq ($(call feature_on,DMR_IP_GATEWAY),)
  ifeq ($(call feature_on,USB_NETWORK),)
    $(error CONFIG_DMR_IP_GATEWAY=y needs CONFIG_USB_NETWORK=y)
  endif
endif
ifneq ($(call feature_on,NET_TCP),)
  ifeq ($(call feature_on,DMR_IP_GATEWAY),)
    $(error CONFIG_NET_TCP=y needs CONFIG_DMR_IP_GATEWAY=y)
  endif
endif
$(foreach f,NET_HTTP NET_RIGCTL,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,NET_TCP),,x)),$(error CONFIG_$(f)=y needs CONFIG_NET_TCP=y)))
$(foreach f,NET_KISS NET_APRSIS,$(if $(call feature_on,$(f)),$(if $(and $(call feature_on,NET_TCP),$(call feature_on,APRS)),,$(error CONFIG_$(f)=y needs CONFIG_NET_TCP=y and CONFIG_APRS=y))))
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
$(foreach f,FM_TX_TONES VOX BATTERY_SAVE AM_RX APRS,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,FM),,x)),$(error CONFIG_$(f)=y needs CONFIG_FM=y)))
ifneq ($(call feature_on,AUDIO_PROBE),)
  ifeq ($(call feature_on,DEBUGLINK)$(call feature_on,DMR),yy)
  else
    $(error CONFIG_AUDIO_PROBE=y needs CONFIG_DEBUGLINK=y and CONFIG_DMR=y)
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
ifeq ($(call feature_on,FM)$(call feature_on,DMR),)
  $(error at least one of CONFIG_FM and CONFIG_DMR must be y)
endif

# Derived: the shared packet data / CSBK code is built when any data feature is on (the emergency alarm is a CSBK)
CONFIG_DMR_DATA_CORE := $(if $(strip $(foreach f,DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_EMERGENCY DMR_IP_GATEWAY,$(call feature_on,$(f)))),y,n)

FEATURE_DEFS := $(foreach f,$(FEATURES) DMR_DATA_CORE,-DCONFIG_$(f)=$(if $(call feature_on,$(f)),1,0))

## Numeric build values (board config.mk sets the defaults; the command line overrides)
# Display safe area in pixels: edges hidden by the bezel or lens. CONFIG_DISPLAY_INSET sets all four
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_BOTTOM ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_LEFT ?= $(CONFIG_DISPLAY_INSET)
CONFIG_DISPLAY_INSET_RIGHT ?= $(CONFIG_DISPLAY_INSET)
VALUES := DISPLAY_INSET_TOP DISPLAY_INSET_BOTTOM DISPLAY_INSET_LEFT DISPLAY_INSET_RIGHT
FEATURE_DEFS += $(foreach v,$(VALUES),-DCONFIG_$(v)=$(strip $(CONFIG_$(v))))
