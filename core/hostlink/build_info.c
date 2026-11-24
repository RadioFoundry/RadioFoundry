// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What this build is, for VERSION in either listener (see build_info.h)
#include "config.h"
#include "hostlink/build_info.h"
#include "ui/i18n.h"
#if CONFIG_P25
#include "radio/radio_p25.h"
#endif
#if CONFIG_NXDN
#include "radio/radio_nxdn.h"
#endif

#ifndef RFOUNDRY_PRESET
#define RFOUNDRY_PRESET "unknown"
#endif
#ifndef RFOUNDRY_FEATURES_OFF
#define RFOUNDRY_FEATURES_OFF ""
#endif

const char build_preset[] = RFOUNDRY_PRESET;
const char build_off[] = RFOUNDRY_FEATURES_OFF;

// The language an empty setting means, as the UI picks it (i18n.c lang_find)
const char *build_start_now(void) { return lang_code(lang_find(0)); }

// The air-framed modes, each only where the board runs it (build_modes_now)
#if CONFIG_P25_TRUNK
#define P25_FEATURES	"P25,P25_TRUNK,"
#elif CONFIG_P25
#define P25_FEATURES	"P25,"
#endif
#if CONFIG_P25 && CONFIG_NXDN
static const char modes_both[] = P25_FEATURES "NXDN,";
#endif

static const char features[] =
#if CONFIG_NXDN_SCRAMBLER
	"NXDN_SCRAMBLER,"
#endif
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
#if CONFIG_GPS
	"GPS,"
#endif
#if CONFIG_SATELLITE_TLE
	"SATELLITE_TLE,"
#endif
#if CONFIG_APRS
	"APRS,"
#endif
#if CONFIG_APRS_AFSK
	"APRS_AFSK,"
#endif
#if CONFIG_APRS_DMR
	"APRS_DMR,"
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
#if CONFIG_DMR_AES
	"DMR_AES,"
#endif
#if CONFIG_DMR_TRUNK
	"DMR_TRUNK,"
#endif
#if CONFIG_DMR_TRUNK_TX
	"DMR_TRUNK_TX,"
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
#if CONFIG_NET_RADIO_UDP
	"NET_RADIO_UDP,"
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
#if CONFIG_AIR_RAW
	"AIR_RAW,"
#endif
#if CONFIG_BCAST_FM
	"BCAST_FM,"
#endif
#if CONFIG_DIAG
	"DIAG,"
#endif
#if CONFIG_UI_SETTINGS
	"UI_SETTINGS,"
#endif
#if CONFIG_CHANNEL_LIST
	"CHANNEL_LIST,"
#endif
#if CONFIG_CRASH_LOG
	"CRASH_LOG,"
#endif
#if CONFIG_SUPPORT_INFO
	"SUPPORT_INFO,"
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

// P25 and NXDN run where the board's air interface carries them: the Codeplug Editor offers their channels only there
const char *build_modes_now(void)
{
#if CONFIG_P25 && CONFIG_NXDN
	bool p25 = radio_p25_fits(), nxdn = radio_nxdn_fits();

	return p25 && nxdn ? modes_both : p25 ? P25_FEATURES : nxdn ? "NXDN," : "";
#elif CONFIG_P25
	return radio_p25_fits() ? P25_FEATURES : "";
#elif CONFIG_NXDN
	return radio_nxdn_fits() ? "NXDN," : "";
#else
	return "";
#endif
}

const char *build_features_now(void)
{
	return features;
}
