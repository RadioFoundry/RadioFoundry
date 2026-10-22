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
#ifndef CONFIG_DMR_GPS
#define CONFIG_DMR_GPS			CONFIG_DMR
#endif
#ifndef CONFIG_DMR_SMS
#define CONFIG_DMR_SMS			1
#endif
#ifndef CONFIG_DMR_TMS
#define CONFIG_DMR_TMS			CONFIG_DMR_SMS
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
#ifndef CONFIG_DMR_EMERGENCY
#define CONFIG_DMR_EMERGENCY		CONFIG_DMR
#endif
#ifndef CONFIG_DMR_ROAMING
#define CONFIG_DMR_ROAMING		CONFIG_DMR
#endif
#ifndef CONFIG_DMR_IP_GATEWAY
#define CONFIG_DMR_IP_GATEWAY		1
#endif
#ifndef CONFIG_DMR_UDP_COMPRESS
#define CONFIG_DMR_UDP_COMPRESS		CONFIG_DMR
#endif
#ifndef CONFIG_DMR_CONFIRMED
#define CONFIG_DMR_CONFIRMED		CONFIG_DMR
#endif
#ifndef CONFIG_NET_TCP
#define CONFIG_NET_TCP			CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_NET_HTTP
#define CONFIG_NET_HTTP			CONFIG_NET_TCP
#endif
#ifndef CONFIG_NET_HTTP_GZIP
#define CONFIG_NET_HTTP_GZIP		CONFIG_NET_HTTP
#endif
#ifndef CONFIG_NET_RIGCTL
#define CONFIG_NET_RIGCTL		CONFIG_NET_TCP
#endif
#ifndef CONFIG_NET_USRP
#define CONFIG_NET_USRP			CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_MDNS
#define CONFIG_MDNS			CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_TFTP
#define CONFIG_TFTP			CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_TFTP_IDDB
#define CONFIG_TFTP_IDDB		CONFIG_TFTP
#endif
#ifndef CONFIG_DMR_CONTROL_STATION
#define CONFIG_DMR_CONTROL_STATION	CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_SFR
#define CONFIG_SFR			1
#endif
#ifndef CONFIG_USB
#define CONFIG_USB			1
#endif
#ifndef CONFIG_HOSTLINK
#define CONFIG_HOSTLINK			CONFIG_USB
#endif
#ifndef CONFIG_SNMP
#define CONFIG_SNMP			CONFIG_DMR_IP_GATEWAY
#endif
#ifndef CONFIG_USB_NETWORK
#define CONFIG_USB_NETWORK		1
#endif
#ifndef CONFIG_NET_PPP
#define CONFIG_NET_PPP			0
#endif
#ifndef CONFIG_HOTSPOT
#define CONFIG_HOTSPOT			1
#endif
// The one switch that defaults off here too: it adds members to hal_dmr_modem_t (the split duplex operations), so
// every file of a program must see the same value; host tests that want it pass -DCONFIG_HOTSPOT_SPLIT=1 to all
#ifndef CONFIG_HOTSPOT_SPLIT
#define CONFIG_HOTSPOT_SPLIT		0
#endif
#ifndef CONFIG_DEBUGLINK
#define CONFIG_DEBUGLINK		CONFIG_HOSTLINK
#endif
#ifndef CONFIG_TASK_STACKS
#define CONFIG_TASK_STACKS		CONFIG_DEBUGLINK
#endif
#ifndef CONFIG_CPS_OPENFW
#define CONFIG_CPS_OPENFW		CONFIG_HOSTLINK
#endif
#ifndef CONFIG_CPS_FAST
#define CONFIG_CPS_FAST			(CONFIG_CPS_OPENFW && CONFIG_DEBUGLINK)
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
#ifndef CONFIG_FONT_AA
#define CONFIG_FONT_AA			1
#endif
#ifndef CONFIG_FONT_BIG
#define CONFIG_FONT_BIG			CONFIG_FONT_AA
#endif
#ifndef CONFIG_UI_COLOR
#define CONFIG_UI_COLOR			1
#endif
#ifndef CONFIG_UI_SETTINGS
#define CONFIG_UI_SETTINGS		1
#endif
#ifndef CONFIG_SCAN
#define CONFIG_SCAN			1
#endif
#ifndef CONFIG_KEY_ASSIGN
#define CONFIG_KEY_ASSIGN		1
#endif
#ifndef CONFIG_CHANNEL_LIST
#define CONFIG_CHANNEL_LIST		1
#endif
#ifndef CONFIG_CHANNEL_EDIT
#define CONFIG_CHANNEL_EDIT		CONFIG_CHANNEL_LIST
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
#ifndef CONFIG_VIBRATE
#define CONFIG_VIBRATE			1
#endif
#ifndef CONFIG_AUDIO_PROBE
#define CONFIG_AUDIO_PROBE		(CONFIG_DEBUGLINK && CONFIG_DMR)
#endif
#ifndef CONFIG_STUB_ENTRY
#define CONFIG_STUB_ENTRY		CONFIG_HOSTLINK
#endif
#ifndef CONFIG_CRASH_LOG
#define CONFIG_CRASH_LOG		1
#endif
#ifndef CONFIG_CRASH_SCREEN
#define CONFIG_CRASH_SCREEN		CONFIG_CRASH_LOG
#endif
#ifndef CONFIG_CRASH_REPORT
#define CONFIG_CRASH_REPORT		(CONFIG_CRASH_SCREEN && CONFIG_DMR)
#endif
#ifndef CONFIG_WATCHDOG
#define CONFIG_WATCHDOG			1
#endif
#ifndef CONFIG_FAULT_TRACE
#define CONFIG_FAULT_TRACE		1
#endif
#ifndef CONFIG_NAND_STORAGE
#define CONFIG_NAND_STORAGE		0
#endif
#ifndef CONFIG_AM_RX
#define CONFIG_AM_RX			CONFIG_FM
#endif
#ifndef CONFIG_BCAST_FM
#define CONFIG_BCAST_FM			1
#endif
#ifndef CONFIG_APRS
#define CONFIG_APRS			CONFIG_FM
#endif
#ifndef CONFIG_APRS_RX
#define CONFIG_APRS_RX			CONFIG_APRS
#endif
#ifndef CONFIG_APRS_IGATE
#define CONFIG_APRS_IGATE		(CONFIG_APRS_RX && CONFIG_HOSTLINK)
#endif
#ifndef CONFIG_NET_KISS
#define CONFIG_NET_KISS			(CONFIG_NET_TCP && CONFIG_APRS)
#endif
#ifndef CONFIG_NET_APRSIS
#define CONFIG_NET_APRSIS		(CONFIG_NET_TCP && CONFIG_APRS)
#endif

// Large state that only the CPU touches (never a DMA buffer) can go in RAM the board keeps for that, the STM32F4's
// CCM: faster, and off the bus the DMA uses. ODR_CPU_RAM is zeroed at start-up like .bss; ODR_CPU_RAM_NOINIT isn't
// (for state written before it's read). Without the board's sections both are ordinary RAM
#ifdef ODR_CPU_RAM_SECTION
#define ODR_CPU_RAM		__attribute__((section(ODR_CPU_RAM_SECTION)))
#else
#define ODR_CPU_RAM
#endif
#ifdef ODR_CPU_RAM_NOINIT_SECTION
#define ODR_CPU_RAM_NOINIT	__attribute__((section(ODR_CPU_RAM_NOINIT_SECTION)))
#else
#define ODR_CPU_RAM_NOINIT
#endif

// Derived: the shared packet data / CSBK code
// Any of the radio's own services on the USB network link (core/net/net.c starts and polls them)
#define CONFIG_NET_LOCAL		(CONFIG_NET_TCP || CONFIG_MDNS || CONFIG_TFTP || CONFIG_NET_USRP)
#ifndef CONFIG_DMR_DATA_CORE
#define CONFIG_DMR_DATA_CORE		(CONFIG_DMR_SMS || CONFIG_DMR_ARS || CONFIG_DMR_LRRP || CONFIG_DMR_SIGNALING || \
					 CONFIG_DMR_EMERGENCY || CONFIG_DMR_IP_GATEWAY || CONFIG_CRASH_REPORT)
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
#if (CONFIG_DMR_PRIVACY || CONFIG_DMR_TALKER_ALIAS || CONFIG_DMR_GPS || CONFIG_DMR_DATA_CORE || CONFIG_DMR_ROAMING || CONFIG_SFR || \
     CONFIG_HOTSPOT || CONFIG_DMR_UDP_COMPRESS || CONFIG_DMR_CONFIRMED) && !CONFIG_DMR
#error "a DMR feature is on without CONFIG_DMR"
#endif
#if CONFIG_HOTSPOT && !CONFIG_HOSTLINK
#error "CONFIG_HOTSPOT needs CONFIG_HOSTLINK"
#endif
#if CONFIG_HOTSPOT_SPLIT && !CONFIG_HOTSPOT
#error "CONFIG_HOTSPOT_SPLIT needs CONFIG_HOTSPOT"
#endif
#if CONFIG_DEBUGLINK && !CONFIG_HOSTLINK
#error "CONFIG_DEBUGLINK needs CONFIG_HOSTLINK"
#endif
#if CONFIG_TASK_STACKS && !CONFIG_DEBUGLINK
#error "CONFIG_TASK_STACKS needs CONFIG_DEBUGLINK"
#endif
#if CONFIG_CPS_OPENFW && !CONFIG_HOSTLINK
#error "CONFIG_CPS_OPENFW needs CONFIG_HOSTLINK"
#endif
#if CONFIG_CPS_FAST && !(CONFIG_CPS_OPENFW && CONFIG_DEBUGLINK)
#error "CONFIG_CPS_FAST needs CONFIG_CPS_OPENFW and CONFIG_DEBUGLINK"
#endif
#if CONFIG_DMR_EMERGENCY && !CONFIG_DMR_DATA_CORE
#error "CONFIG_DMR_EMERGENCY needs CONFIG_DMR_DATA_CORE (the alarm is a CSBK)"
#endif
#if CONFIG_DMR_IP_GATEWAY && !(CONFIG_USB_NETWORK || CONFIG_NET_PPP)
#error "CONFIG_DMR_IP_GATEWAY needs CONFIG_USB_NETWORK or CONFIG_NET_PPP (a link to the host)"
#endif
#if CONFIG_NET_PPP && !(CONFIG_HOSTLINK && CONFIG_DMR_IP_GATEWAY)
#error "CONFIG_NET_PPP needs CONFIG_HOSTLINK and CONFIG_DMR_IP_GATEWAY"
#endif
#if CONFIG_NET_TCP && !CONFIG_DMR_IP_GATEWAY
#error "CONFIG_NET_TCP needs CONFIG_DMR_IP_GATEWAY"
#endif
#if (CONFIG_NET_HTTP || CONFIG_NET_RIGCTL) && !CONFIG_NET_TCP
#error "CONFIG_NET_HTTP and CONFIG_NET_RIGCTL need CONFIG_NET_TCP"
#endif
#if (CONFIG_NET_KISS || CONFIG_NET_APRSIS) && !(CONFIG_NET_TCP && CONFIG_APRS)
#error "CONFIG_NET_KISS and CONFIG_NET_APRSIS need CONFIG_NET_TCP and CONFIG_APRS"
#endif
#if (CONFIG_MDNS || CONFIG_TFTP) && !CONFIG_DMR_IP_GATEWAY
#error "CONFIG_MDNS and CONFIG_TFTP need CONFIG_DMR_IP_GATEWAY (the IP stack they run on)"
#endif
#if CONFIG_TFTP_IDDB && !CONFIG_TFTP
#error "CONFIG_TFTP_IDDB needs CONFIG_TFTP"
#endif
#if CONFIG_DMR_TMS && !CONFIG_DMR_SMS
#error "CONFIG_DMR_TMS needs CONFIG_DMR_SMS"
#endif
#if CONFIG_DMR_CONTROL_STATION && !CONFIG_DMR_IP_GATEWAY
#error "CONFIG_DMR_CONTROL_STATION needs CONFIG_DMR_IP_GATEWAY"
#endif
#if CONFIG_SNMP && !CONFIG_DMR_IP_GATEWAY
#error "CONFIG_SNMP needs CONFIG_DMR_IP_GATEWAY"
#endif
#if CONFIG_USB_NETWORK && !CONFIG_USB
#error "CONFIG_USB_NETWORK needs CONFIG_USB"
#endif
#if (CONFIG_VFO_FM && !(CONFIG_VFO && CONFIG_FM)) || (CONFIG_VFO_DMR && !(CONFIG_VFO && CONFIG_DMR))
#error "a VFO kind is on without CONFIG_VFO or its mode"
#endif
#if (CONFIG_FM_TX_TONES || CONFIG_VOX || CONFIG_BATTERY_SAVE || CONFIG_AM_RX) && !CONFIG_FM
#error "an FM feature is on without CONFIG_FM"
#endif
#if CONFIG_AUDIO_PROBE && !(CONFIG_DEBUGLINK && CONFIG_DMR)
#error "CONFIG_AUDIO_PROBE needs CONFIG_DEBUGLINK and CONFIG_DMR"
#endif
#if CONFIG_CRASH_SCREEN && !CONFIG_CRASH_LOG
#error "CONFIG_CRASH_SCREEN needs CONFIG_CRASH_LOG"
#endif
#if CONFIG_CRASH_REPORT && !(CONFIG_CRASH_SCREEN && CONFIG_DMR)
#error "CONFIG_CRASH_REPORT needs CONFIG_CRASH_SCREEN and CONFIG_DMR"
#endif
#if CONFIG_STUB_ENTRY && !CONFIG_HOSTLINK
#error "CONFIG_STUB_ENTRY needs CONFIG_HOSTLINK"
#endif
#if CONFIG_APRS && !CONFIG_FM
#error "CONFIG_APRS needs CONFIG_FM"
#endif
#if (CONFIG_APRS_RX && !CONFIG_APRS) || (CONFIG_APRS_IGATE && !(CONFIG_APRS_RX && CONFIG_HOSTLINK))
#error "an APRS sub-feature is on without its parent (APRS_RX needs APRS; APRS_IGATE needs APRS_RX and HOSTLINK)"
#endif
#if !CONFIG_FM && !CONFIG_DMR
#error "at least one of CONFIG_FM and CONFIG_DMR"
#endif

#endif
