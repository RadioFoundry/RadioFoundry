// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Glue between the IP gateway and the radio's own UDP services (net_services.h): mDNS (core/net/mdns.c) and TFTP
// (core/net/tftp.c with codeplug.bin from tftp_codeplug.c and info.txt here).
#include <string.h>
#include "config.h"
#include "net/net_services.h"
#include "dmr/ipGateway.h"
#include "board.h"
#include "hal_time.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"
#if CONFIG_MDNS
#include "net/mdns.h"
#endif
#if CONFIG_NET_TCP
#include "net/net_svc.h"	// the TCP services' ports, advertised over mDNS
#endif
#if CONFIG_TFTP
#include "net/tftp.h"
#include "net/tftp_codeplug.h"
#endif

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

static uint8_t *payload(int *max)
{
	*max = IPGW_UDP_PAYLOAD_MAX;
	return ipGatewayUdpPayload();
}

#define IDENTITY_MS	1000

static char call[17];			// the radio's call sign, read once a second (it reads storage)
static uint32_t call_ms;
static bool call_read;

// The radio's call sign: the settings' radio name, else the codeplug's; its first word, printable ASCII
static void read_callsign(char *out, size_t size)
{
	char name[CODEPLUG_NAME_LEN + 1];
	settings_t st;
	size_t n = 0;

	settings_read(&st);
	memcpy(name, st.radio_name, SETTINGS_NAME_LEN < CODEPLUG_NAME_LEN ? SETTINGS_NAME_LEN : CODEPLUG_NAME_LEN);
	name[SETTINGS_NAME_LEN < CODEPLUG_NAME_LEN ? SETTINGS_NAME_LEN : CODEPLUG_NAME_LEN] = '\0';
	if (name[0] == '\0') {
		codeplug_radio_name(name);
	}
	const char *p = name;
	while (*p == ' ') {
		p++;
	}
	for (; *p > ' ' && *p < 0x7F && n + 1 < size; p++) {
		out[n++] = *p;
	}
	out[n] = '\0';
}

#if CONFIG_MDNS
// One line per service the radio offers on its address; other services add theirs here
static const mdns_service_t services[] = {
#if CONFIG_DEBUGLINK
	{ "_odr-debug._udp", IPGW_SERIAL_PORT },
#endif
#if CONFIG_HOTSPOT
	{ "_mmdvm._udp", IPGW_SERIAL_PORT },
#endif
#if CONFIG_TFTP
	{ "_tftp._udp", TFTP_PORT },
#endif
#if CONFIG_SNMP
	{ "_snmp._udp", 161 },
#endif
#if CONFIG_NET_HTTP
	{ "_http._tcp", NET_HTTP_PORT },
#endif
#if CONFIG_NET_KISS
	{ "_kiss-tnc._tcp", NET_KISS_PORT },
#endif
#if CONFIG_NET_RIGCTL
	{ "_rigctld._tcp", NET_RIGCTL_PORT },
#endif
	{ NULL, 0 }		// the end (and the table is never empty)
};
#define SERVICES	((int)(sizeof(services) / sizeof(services[0])) - 1)

static void mdns_identity(mdns_identity_t *id)
{
	id->ip = ipGatewayRadioIP();
	id->dmr_id = ipGatewayRadioId();
	memcpy(id->callsign, call, sizeof(id->callsign));
	id->board = board.name;
	id->firmware = ODR_GIT;
}

static bool mdns_send(uint32_t ip, uint16_t port, const uint8_t *msg, int length)
{
	return ipGatewayUdpSend(ip, MDNS_PORT, port, msg, length);
}

static uint32_t mdns_random(void)
{
	uint8_t host[6], radio[6];
	uint32_t h = hal_millis() * 2654435761u;

	if (board.netif != NULL) {
		board.netif->macs(host, radio);
		h ^= ((uint32_t)radio[2] << 24) | ((uint32_t)radio[3] << 16) | ((uint32_t)radio[4] << 8) | radio[5];
	}
	return h ^ (h >> 15);
}

static const mdns_io_t mdns_io = { .identity = mdns_identity, .buffer = payload, .send = mdns_send, .random = mdns_random };

static void mdns_udp(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	mdnsReceive(src, sport, data, length, hal_millis());
}
#endif

#if CONFIG_TFTP
// ---- info.txt: who the radio is, as text ----

static void add(char *b, size_t size, const char *s)
{
	size_t n = strlen(b);
	while (*s != '\0' && n + 1 < size) {
		b[n++] = *s++;
	}
	b[n] = '\0';
}

static void add_u32(char *b, size_t size, uint32_t v)
{
	char t[11];
	int n = 0;
	char s[11];
	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0);
	for (int i = 0; i < n; i++) {
		s[i] = t[n - 1 - i];
	}
	s[n] = '\0';
	add(b, size, s);
}

static void add_ip(char *b, size_t size, uint32_t ip)
{
	for (int i = 3; i >= 0; i--) {
		add_u32(b, size, (ip >> (8 * i)) & 0xFFu);
		if (i > 0) {
			add(b, size, ".");
		}
	}
}

#define INFO_MAX	320

static void info_text(char *b)
{
	b[0] = '\0';
	add(b, INFO_MAX, "board: ");
	add(b, INFO_MAX, board.name != NULL ? board.name : "?");
	add(b, INFO_MAX, "\nfirmware: " ODR_GIT "\ndmr_id: ");
	add_u32(b, INFO_MAX, ipGatewayRadioId());
	add(b, INFO_MAX, "\ncallsign: ");
	add(b, INFO_MAX, call);
	add(b, INFO_MAX, "\nradio_ip: ");
	add_ip(b, INFO_MAX, ipGatewayRadioIP());
	add(b, INFO_MAX, "\nhost_ip: ");
	add_ip(b, INFO_MAX, ipGatewayHostIP());
#if CONFIG_MDNS
	add(b, INFO_MAX, "\nmdns: ");
	add(b, INFO_MAX, mdnsHostName());
	add(b, INFO_MAX, mdnsHostName()[0] != '\0' ? ".local" : "off");
#endif
	add(b, INFO_MAX, "\ncodeplug: ");
	codeplug_source_t src = codeplug_source();
	add(b, INFO_MAX, src == CODEPLUG_OPENFW ? "open-firmware" : src == CODEPLUG_BUILTIN ? "builtin" : "none");
	add(b, INFO_MAX, "\ncodeplug_write: ");
	add(b, INFO_MAX, tftp_codeplug_write_allowed() ? "allowed" : "off");
	add(b, INFO_MAX, "\n");
}

static bool info_open(uint32_t *size)
{
	char b[INFO_MAX];
	info_text(b);
	*size = (uint32_t)strlen(b);
	return true;
}

static bool info_read(uint32_t off, uint8_t *buf, uint16_t len)
{
	char b[INFO_MAX];
	info_text(b);
	size_t n = strlen(b);
	memset(buf, ' ', len);	// the text changed under the transfer (an ID change): padded, never past the size
	if (off < n) {
		memcpy(buf, b + off, (n - off) < len ? (n - off) : len);
	}
	return true;
}

static const tftp_file_t info_file = { .name = "info.txt", .open_read = info_open, .read = info_read };
static const tftp_file_t *const files[] = { &info_file, &tftp_codeplug_file };

static void tftp_udp(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	tftpReceive(src, sport, dport, data, length, hal_millis());
}

static bool tftp_send(uint32_t ip, uint16_t sport, uint16_t dport, const uint8_t *pkt, int length)
{
	return ipGatewayUdpSend(ip, sport, dport, pkt, length);
}

static void tftp_bind(uint16_t port, bool on)
{
	ipGatewayUdpBind(port, on ? tftp_udp : NULL);
}

static const tftp_io_t tftp_io = { .buffer = payload, .send = tftp_send, .bind = tftp_bind };
#endif

void net_services_init(void)
{
#if CONFIG_MDNS
	mdnsInit(&mdns_io, services, SERVICES);
	ipGatewayUdpBind(MDNS_PORT, mdns_udp);
#endif
#if CONFIG_TFTP
	tftpInit(&tftp_io, files, (int)(sizeof(files) / sizeof(files[0])));
	ipGatewayUdpBind(TFTP_PORT, tftp_udp);
#endif
}

void net_services_poll(uint32_t now_ms, bool up)
{
	if (!call_read || now_ms - call_ms >= IDENTITY_MS) {
		call_read = true;
		call_ms = now_ms;
		read_callsign(call, sizeof(call));
	}
#if CONFIG_MDNS
	mdnsTick(now_ms, up);
#endif
#if CONFIG_TFTP
	tftpTick(now_ms);
#endif
}

void net_services_view(dl_text_t *t)
{
#if CONFIG_MDNS
	mdns_stats_t md;

	mdnsStats(&md);
	dl_text(t, "mdns.state=%u\nmdns.name=%s\nmdns.queries=%u\nmdns.answered=%u\n", md.state, mdnsHostName(),
		(unsigned)md.queries, (unsigned)md.answered);
	dl_text(t, "mdns.sent=%u\nmdns.busy=%u\nmdns.conflicts=%u\nmdns.bad=%u\n", (unsigned)md.sent, (unsigned)md.busy,
		(unsigned)md.conflicts, (unsigned)md.bad);
#endif
#if CONFIG_TFTP
	tftp_stats_t tf;

	tftpStats(&tf);
	dl_text(t, "tftp.state=%u\ntftp.reads=%u\ntftp.writes=%u\ntftp.errors=%u\n", tf.state, (unsigned)tf.reads,
		(unsigned)tf.writes, (unsigned)tf.errors);
	dl_text(t, "tftp.timeouts=%u\ntftp.retransmits=%u\ntftp.last_size=%u\ntftp.blksize=%u\ntftp.write_allowed=%u\n",
		(unsigned)tf.timeouts, (unsigned)tf.retransmits, (unsigned)tf.last_size, tf.blksize,
		tftp_codeplug_write_allowed());
#endif
}
