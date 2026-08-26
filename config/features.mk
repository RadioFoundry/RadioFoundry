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
# DMR text messages (SMS; needs DMR; not ported yet)
CONFIG_DMR_SMS ?= $(CONFIG_DMR)
# ARS registration with a dispatcher (needs DMR; not ported yet)
CONFIG_DMR_ARS ?= $(CONFIG_DMR)
# LRRP location request / response (needs DMR; uses GPS where the board has it; not ported yet)
CONFIG_DMR_LRRP ?= $(CONFIG_DMR)
# CSBK signaling: call alert, radio check (needs DMR; not ported yet)
CONFIG_DMR_SIGNALING ?= $(CONFIG_DMR)
# IP over DMR packet data through the USB network adapter (needs DMR and USB_NETWORK; core/hostlink/hostlink_net.c)
CONFIG_DMR_IP_GATEWAY ?= n
# single frequency repeater (needs DMR): the MMDVM hotspot's SFR mode (core/hotspot/hotspotSfr.c, core/dmr/sfrRunner.c);
# the stand-alone repeater and user roles on a channel are not wired yet (docs/spec/sfr.md)
CONFIG_SFR ?= n
# USB device (CDC serial)
CONFIG_USB ?= y
# USB network adapter personality (CDC-NCM, switched with the serial one at run time; needs USB)
CONFIG_USB_NETWORK ?= $(CONFIG_DMR_IP_GATEWAY)
# MMDVM modem / hotspot mode over USB (needs DMR and USB)
CONFIG_HOTSPOT ?= $(if $(and $(filter y,$(CONFIG_DMR)),$(filter y,$(CONFIG_USB))),y,n)
# VFO: tune a frequency by hand (off: programmed channels only)
CONFIG_VFO ?= y
# analog FM VFO (needs VFO and FM)
CONFIG_VFO_FM ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_FM))),y,n)
# digital DMR VFO: own ID, color code, timeslot, talk target typed in (needs VFO and DMR; not built yet)
CONFIG_VFO_DMR ?= $(if $(and $(filter y,$(CONFIG_VFO)),$(filter y,$(CONFIG_DMR))),y,n)
# the USB debug link: framed commands on the host port for bench automation (docs/spec/debuglink.md; needs USB)
CONFIG_DEBUGLINK ?= $(CONFIG_USB)
# the legacy CPS protocol shim on the host port, so qdmr and the legacy CPS can read and write the codeplug
# (docs/spec/legacy-cps-interop.md; needs USB)
CONFIG_CPS_LEGACY ?= $(CONFIG_USB)
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
CONFIG_VOX ?= n
# receiver battery saving: FM receiver duty cycling while idle (needs FM)
CONFIG_BATTERY_SAVE ?= $(CONFIG_FM)

FEATURES := FM SUBTONE DMR DMR_PRIVACY DMR_TALKER_ALIAS DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_IP_GATEWAY SFR \
	USB USB_NETWORK HOTSPOT DEBUGLINK CPS_LEGACY VFO VFO_FM VFO_DMR DIAG FONT_XL UI_SETTINGS SCAN BEEP FM_TX_TONES VOX BATTERY_SAVE

# Dependencies
feature_on = $(filter y,$(CONFIG_$(1)))
ifneq ($(call feature_on,SUBTONE),)
  ifeq ($(call feature_on,FM),)
    $(error CONFIG_SUBTONE=y needs CONFIG_FM=y)
  endif
endif
$(foreach f,DMR_PRIVACY DMR_TALKER_ALIAS DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_IP_GATEWAY SFR HOTSPOT,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,DMR),,x)),$(error CONFIG_$(f)=y needs CONFIG_DMR=y)))
ifneq ($(call feature_on,HOTSPOT),)
  ifeq ($(call feature_on,USB),)
    $(error CONFIG_HOTSPOT=y needs CONFIG_USB=y)
  endif
endif
ifneq ($(call feature_on,DEBUGLINK),)
  ifeq ($(call feature_on,USB),)
    $(error CONFIG_DEBUGLINK=y needs CONFIG_USB=y)
  endif
endif
ifneq ($(call feature_on,CPS_LEGACY),)
  ifeq ($(call feature_on,USB),)
    $(error CONFIG_CPS_LEGACY=y needs CONFIG_USB=y)
  endif
endif
ifneq ($(call feature_on,USB_NETWORK),)
  ifeq ($(call feature_on,USB),)
    $(error CONFIG_USB_NETWORK=y needs CONFIG_USB=y)
  endif
endif
ifneq ($(call feature_on,DMR_IP_GATEWAY),)
  ifeq ($(call feature_on,USB_NETWORK),)
    $(error CONFIG_DMR_IP_GATEWAY=y needs CONFIG_USB_NETWORK=y)
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
$(foreach f,FM_TX_TONES VOX BATTERY_SAVE,$(if $(and $(call feature_on,$(f)),$(if $(call feature_on,FM),,x)),$(error CONFIG_$(f)=y needs CONFIG_FM=y)))
ifeq ($(call feature_on,FM)$(call feature_on,DMR),)
  $(error at least one of CONFIG_FM and CONFIG_DMR must be y)
endif

# Derived: the shared packet data / CSBK code is built when any data feature is on
CONFIG_DMR_DATA_CORE := $(if $(strip $(foreach f,DMR_SMS DMR_ARS DMR_LRRP DMR_SIGNALING DMR_IP_GATEWAY,$(call feature_on,$(f)))),y,n)

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
