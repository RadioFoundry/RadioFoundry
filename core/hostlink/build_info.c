// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What this build is, for VERSION in either listener (see build_info.h)
#include "config.h"
#include "hostlink/build_info.h"

#ifndef RFOUNDRY_PRESET
#define RFOUNDRY_PRESET "unknown"
#endif
#ifndef RFOUNDRY_FEATURES_OFF
#define RFOUNDRY_FEATURES_OFF ""
#endif

const char build_preset[] = RFOUNDRY_PRESET;
const char build_off[] = RFOUNDRY_FEATURES_OFF;

const char build_features[] =
#if CONFIG_FM
	"FM,"
#endif
#if CONFIG_SUBTONE
	"SUBTONE,"
#endif
#if CONFIG_DMR
	"DMR,"
#endif
#if CONFIG_DMR_PRIVACY
	"DMR_PRIVACY,"
#endif
#if CONFIG_DMR_TALKER_ALIAS
	"DMR_TALKER_ALIAS,"
#endif
#if CONFIG_DMR_GPS
	"DMR_GPS,"
#endif
#if CONFIG_APRS
	"APRS,"
#endif
#if CONFIG_APRS_RX
	"APRS_RX,"
#endif
#if CONFIG_APRS_IGATE
	"APRS_IGATE,"
#endif
#if CONFIG_DMR_DATA_CORE
	"DMR_DATA_CORE,"
#endif
#if CONFIG_DMR_SMS
	"DMR_SMS,"
#endif
#if CONFIG_DMR_ARS
	"DMR_ARS,"
#endif
#if CONFIG_DMR_LRRP
	"DMR_LRRP,"
#endif
#if CONFIG_DMR_SIGNALING
	"DMR_SIGNALING,"
#endif
#if CONFIG_DMR_ROAMING
	"DMR_ROAMING,"
#endif
#if CONFIG_SFR
	"SFR,"
#endif
#if CONFIG_HOTSPOT
	"HOTSPOT,"
#endif
#if CONFIG_HOTSPOT_SPLIT
	"HOTSPOT_SPLIT,"
#endif
#if CONFIG_USB_NETWORK
	"USB_NETWORK,"
#endif
#if CONFIG_DMR_IP_GATEWAY
	"DMR_IP_GATEWAY,"
#endif
#if CONFIG_DMR_UDP_COMPRESS && CONFIG_DMR_DATA_CORE
	"DMR_UDP_COMPRESS,"
#endif
#if CONFIG_DMR_CONFIRMED && CONFIG_DMR_DATA_CORE
	"DMR_CONFIRMED,"
#endif
#if CONFIG_NET_TCP
	"NET_TCP,"
#endif
#if CONFIG_NET_PPP
	"NET_PPP,"
#endif
#if CONFIG_NET_KISS
	"NET_KISS,"
#endif
#if CONFIG_NET_HTTP
	"NET_HTTP,"
#endif
#if CONFIG_NET_HTTP_GZIP
	"NET_HTTP_GZIP,"
#endif
#if CONFIG_NET_RIGCTL
	"NET_RIGCTL,"
#endif
#if CONFIG_NET_APRSIS
	"NET_APRSIS,"
#endif
#if CONFIG_NET_USRP
	"NET_USRP,"
#endif
#if CONFIG_MDNS
	"MDNS,"
#endif
#if CONFIG_TFTP
	"TFTP,"
#endif
#if CONFIG_TFTP_IDDB
	"TFTP_IDDB,"
#endif
#if CONFIG_IDDB_COMPACT
	"IDDB_COMPACT,"
#endif
#if CONFIG_SNMP
	"SNMP,"
#endif
#if CONFIG_VFO
	"VFO,"
#endif
#if CONFIG_VFO_FM
	"VFO_FM,"
#endif
#if CONFIG_VFO_DMR
	"VFO_DMR,"
#endif
#if CONFIG_AM_RX
	"AM_RX,"
#endif
#if CONFIG_BCAST_FM
	"BCAST_FM,"
#endif
#if CONFIG_DIAG
	"DIAG,"
#endif
#if CONFIG_CRASH_LOG
	"CRASH_LOG,"
#endif
#if CONFIG_SCAN
	"SCAN,"
#endif
#if CONFIG_BEEP
	"BEEP,"
#endif
#if CONFIG_FM_TX_TONES
	"FM_TX_TONES,"
#endif
#if CONFIG_DTMF
	"DTMF,"
#endif
#if CONFIG_VOX
	"VOX,"
#endif
#if CONFIG_BATTERY_SAVE
	"BATTERY_SAVE,"
#endif
#if CONFIG_CPS_FAST
	"CPS_FAST,"
#endif
#if CONFIG_USB
	"USB,"
#endif
#if CONFIG_DEBUGLINK
	"DEBUGLINK,"
#endif
	"HOSTLINK";	// both listeners run on the host link (USB CDC or a UART)
