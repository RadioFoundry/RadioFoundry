// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Feature switches (config/features.mk sets them for firmware builds; everything defaults on here, e.g. for host
// tests). Always defined to 0 or 1: test with #if CONFIG_X, never #ifdef. See docs/build-config.md.
#ifndef ODR_CONFIG_H
#define ODR_CONFIG_H

#ifndef CONFIG_FM
#define CONFIG_FM			1
#endif
#ifndef CONFIG_SUBTONE
#define CONFIG_SUBTONE			1
#endif
#ifndef CONFIG_DMR
#define CONFIG_DMR			1
#endif
#ifndef CONFIG_DMR_PRIVACY
#define CONFIG_DMR_PRIVACY		1
#endif
#ifndef CONFIG_DMR_TALKER_ALIAS
#define CONFIG_DMR_TALKER_ALIAS		1
#endif
#ifndef CONFIG_DMR_SMS
#define CONFIG_DMR_SMS			1
#endif
#ifndef CONFIG_DMR_ARS
#define CONFIG_DMR_ARS			1
#endif
#ifndef CONFIG_DMR_LRRP
#define CONFIG_DMR_LRRP			1
#endif
#ifndef CONFIG_DMR_SIGNALING
#define CONFIG_DMR_SIGNALING		1
#endif
#ifndef CONFIG_DMR_IP_GATEWAY
#define CONFIG_DMR_IP_GATEWAY		1
#endif
#ifndef CONFIG_SFR
#define CONFIG_SFR			1
#endif
#ifndef CONFIG_USB
#define CONFIG_USB			1
#endif
#ifndef CONFIG_USB_NETWORK
#define CONFIG_USB_NETWORK		1
#endif
#ifndef CONFIG_HOTSPOT
#define CONFIG_HOTSPOT			1
#endif
#ifndef CONFIG_DEBUGLINK
#define CONFIG_DEBUGLINK		CONFIG_USB
#endif
#ifndef CONFIG_CPS_LEGACY
#define CONFIG_CPS_LEGACY		CONFIG_USB
#endif
#ifndef CONFIG_VFO
#define CONFIG_VFO			1
#endif
#ifndef CONFIG_VFO_FM
#define CONFIG_VFO_FM			(CONFIG_VFO && CONFIG_FM)
#endif
#ifndef CONFIG_VFO_DMR
#define CONFIG_VFO_DMR			(CONFIG_VFO && CONFIG_DMR)
#endif
#ifndef CONFIG_DIAG
#define CONFIG_DIAG			1
#endif
#ifndef CONFIG_FONT_XL
#define CONFIG_FONT_XL			1
#endif
#ifndef CONFIG_SCAN
#define CONFIG_SCAN			1
#endif
#ifndef CONFIG_BEEP
#define CONFIG_BEEP			1
#endif
#ifndef CONFIG_FM_TX_TONES
#define CONFIG_FM_TX_TONES		CONFIG_FM
#endif
#ifndef CONFIG_VOX
#define CONFIG_VOX			CONFIG_FM
#endif
#ifndef CONFIG_BATTERY_SAVE
#define CONFIG_BATTERY_SAVE		CONFIG_FM
#endif

// Derived: the shared packet data / CSBK code
#ifndef CONFIG_DMR_DATA_CORE
#define CONFIG_DMR_DATA_CORE		(CONFIG_DMR_SMS || CONFIG_DMR_ARS || CONFIG_DMR_LRRP || CONFIG_DMR_SIGNALING || \
					 CONFIG_DMR_IP_GATEWAY)
#endif

// Numeric build values
#ifndef CONFIG_DISPLAY_INSET_TOP
#define CONFIG_DISPLAY_INSET_TOP	0
#endif
#ifndef CONFIG_DISPLAY_INSET_BOTTOM
#define CONFIG_DISPLAY_INSET_BOTTOM	0
#endif
#ifndef CONFIG_DISPLAY_INSET_LEFT
#define CONFIG_DISPLAY_INSET_LEFT	0
#endif
#ifndef CONFIG_DISPLAY_INSET_RIGHT
#define CONFIG_DISPLAY_INSET_RIGHT	0
#endif

#if CONFIG_SUBTONE && !CONFIG_FM
#error "CONFIG_SUBTONE needs CONFIG_FM"
#endif
#if (CONFIG_DMR_PRIVACY || CONFIG_DMR_TALKER_ALIAS || CONFIG_DMR_DATA_CORE || CONFIG_SFR || CONFIG_HOTSPOT) && !CONFIG_DMR
#error "a DMR feature is on without CONFIG_DMR"
#endif
#if CONFIG_HOTSPOT && !CONFIG_USB
#error "CONFIG_HOTSPOT needs CONFIG_USB"
#endif
#if CONFIG_DEBUGLINK && !CONFIG_USB
#error "CONFIG_DEBUGLINK needs CONFIG_USB"
#endif
#if CONFIG_CPS_LEGACY && !CONFIG_USB
#error "CONFIG_CPS_LEGACY needs CONFIG_USB"
#endif
#if CONFIG_DMR_IP_GATEWAY && !CONFIG_USB_NETWORK
#error "CONFIG_DMR_IP_GATEWAY needs CONFIG_USB_NETWORK"
#endif
#if CONFIG_USB_NETWORK && !CONFIG_USB
#error "CONFIG_USB_NETWORK needs CONFIG_USB"
#endif
#if (CONFIG_VFO_FM && !(CONFIG_VFO && CONFIG_FM)) || (CONFIG_VFO_DMR && !(CONFIG_VFO && CONFIG_DMR))
#error "a VFO kind is on without CONFIG_VFO or its mode"
#endif
#if (CONFIG_FM_TX_TONES || CONFIG_VOX || CONFIG_BATTERY_SAVE) && !CONFIG_FM
#error "an FM feature is on without CONFIG_FM"
#endif
#if !CONFIG_FM && !CONFIG_DMR
#error "at least one of CONFIG_FM and CONFIG_DMR"
#endif

#endif
