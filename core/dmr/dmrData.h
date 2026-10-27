// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR packet data (ETSI TS 102 361-1 clause 8 / 9)
 */

#ifndef RFOUNDRY_DMRDATA_H
#define RFOUNDRY_DMRDATA_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// Big buffers only the CPU touches: in the board's CPU-only RAM where it has one (config.h RFOUNDRY_CPU_RAM)
#define DMR_DATA_BUFFER RFOUNDRY_CPU_RAM

// ETSI TS 102 361-1 data types (same values as the hotspot's DT_xxx)
#ifndef DT_CSBK
#define DT_VOICE_PI_HEADER		0x00U
#define DT_VOICE_LC_HEADER		0x01U
#define DT_TERMINATOR_WITH_LC	0x02U
#define DT_CSBK					0x03U
#define DT_MBC_HEADER			0x04U
#define DT_MBC_CONTINUATION		0x05U
#define DT_DATA_HEADER			0x06U
#define DT_RATE_12_DATA			0x07U
#define DT_RATE_34_DATA			0x08U
#define DT_IDLE					0x09U
#define DT_RATE_1_DATA			0x0AU
#endif

// This module has no hardware dependencies, so it can be unit tested on a host (see tests/host)

// Largest burst info field: Rate 1 data is 24 bytes, Rate 3/4 is 18, everything else is 12
#define DMR_BURST_PAYLOAD_MAX		24

// One transmission's bursts and one packet's octets, sized for the air MTU below (docs/spec/dmr-data.md 12.8)
#define DMR_DATA_MAX_BURSTS			64			// confirmed at the air MTU: preambles, header, 60 blocks, terminator
#define DMR_DATA_MAX_PACKET			612			// 51 Rate 1/2 blocks
#define DMR_DATA_RATE12_OCTETS		12			// a Rate 1/2 block's octets (unconfirmed)

// The air MTU: the longest IP packet (user data of one packet: the IPv4 header and all, or the compressed header and
// the UDP payload) that goes in one transmission of ours, confirmed or not: DMR_DATA_MAX_PACKET less the padding and
// the CRC-32 the last block may take. Not below the USB link's MTU (IPGW_LINK_MTU, 586), so every packet the host
// sends fits (docs/spec/usb-network.md 3.2). Unconfirmed it takes 50 blocks after DMR_DATA_IP_PREAMBLES preamble CSBKs
// and the header, 53 bursts; confirmed (10 octets a block) 60 blocks and a terminator, 64
#define DMR_DATA_IP_PREAMBLES		2
#define DMR_DATA_IP_MTU				(DMR_DATA_MAX_PACKET - 16)

// Burst flags
#define DMR_BURST_FLAG_MCU_CRC		0x01		// TX: payload already holds its CRC / mask, the HR-C6000 must send it verbatim
#define DMR_BURST_FLAG_RX_CRC_ERROR	0x02		// RX: the HR-C6000 reported a CRC error for this burst

typedef struct
{
	uint8_t dataType;							// DT_xxx from dmrDefines.h
	uint8_t length;
	uint8_t flags;
	uint8_t payload[DMR_BURST_PAYLOAD_MAX];
} dmrBurst_t;

// Data packet formats (DPF) and service access points (SAP)
#define DMR_DPF_UDT					0x00
#define DMR_DPF_RESPONSE			0x01
#define DMR_DPF_UNCONFIRMED			0x02
#define DMR_DPF_CONFIRMED			0x03
#define DMR_DPF_DEFINED_SHORT		0x0D
#define DMR_DPF_RAW_SHORT			0x0E
#define DMR_DPF_PROPRIETARY			0x0F

#define DMR_SAP_UDT					0x00
#define DMR_SAP_UDPIP_COMPRESSION	0x03
#define DMR_SAP_IP					0x04
#define DMR_SAP_PROPRIETARY			0x09
#define DMR_SAP_SHORT_DATA			0x0A

#define DMR_CSBKO_CALL_ALERT		0x1F
#define DMR_CSBKO_CALL_ALERT_ACK	0x20
#define DMR_CSBKO_RADIO_CHECK		0x24		// byte 3 0x80: request, else the answer
#define DMR_CSBKO_EMERGENCY_ALARM	0x27		// emergency alarm (docs/spec/dmr-emergency.md 2)
#define DMR_CSBKO_PREAMBLE			0x3D

// Feature set IDs whose call alert and radio check CSBKs are answered
#define DMR_FID_ETSI				0x00
#define DMR_FID_MOTOROLA			0x10

// IP protocols
#define DMR_IP_PROTO_ICMP			1
#define DMR_IP_PROTO_UDP			17
#define DMR_IP_PROTO_SCTP			132

#define DMR_UDP_PORT_LRRP			4001
#define DMR_UDP_PORT_ARS			4005
#define DMR_UDP_PORT_TMS			4007
#define DMR_UDP_PORT_ETSI_TEXT		5016		// ETSI IP text (TS 102 361-3 5.7.1; compressed port ID 1)

typedef struct
{
	uint8_t  dpf;
	uint8_t  sap;
	bool     group;
	bool     responseRequested;
	uint32_t dst;
	uint32_t src;
	uint8_t  blocksToFollow;
	uint8_t  padOctets;
	uint8_t  sendSeq;						// N(S), confirmed only
	uint8_t  ddFormat;						// defined short data: the DD format (6 bits); UDT: the UDT format
	uint8_t  padBits;						// defined short data: the bit padding; UDT: the pad nibbles x 4
	uint8_t  response;						// response packets: class (7-6), type (5-3), status (2-0)
	uint8_t  dataType;						// DT of the blocks
	bool     firstTry;						// confirmed: the header's F (full message) flag
	uint16_t length;						// user data length
	uint8_t  data[DMR_DATA_MAX_PACKET];
} dmrDataPacket_t;

typedef enum
{
	DMR_DATA_RX_NONE,							// burst consumed, nothing complete yet
	DMR_DATA_RX_PACKET,							// a complete packet is in dmrDataRxPacket
	DMR_DATA_RX_CSBK,							// a non preamble CSBK is in the burst passed in
	DMR_DATA_RX_ERROR,							// header or packet CRC failed, reassembly reset
	// Confirmed data (CONFIG_DMR_CONFIRMED), the header's fields in dmrDataRxPacket:
	DMR_DATA_RX_RETRY,							// blocks are missing or failed their CRC-9: a selective ACK asks for
												// them (dmrDataRxRetryFlags); the packet is held for the retry
	DMR_DATA_RX_NACK							// every block arrived good but the CRC-32 failed: a NACK
} dmrDataRxResult_t;

extern dmrDataPacket_t dmrDataRxPacket;

uint32_t dmrDataCRC32(const uint8_t *data, int length);
// The CRC-CCITT of a 12-octet CSBK / header / MBC info field (octets 10-11) with its mask (CSBK 0xA5, data header
// 0xCC, MBC 0xAA, as one octet repeated)
bool dmrDataCheckCRC16(const uint8_t *burst12, uint8_t mask);

// TX builders. Each returns the number of bursts written to out, or 0 if the data does not fit
int dmrDataBuildCSBK(const uint8_t csbk[10], dmrBurst_t *out);
// The answer from us (ID src) to a received CSBK (its 10 bytes, CRC checked): the ack of a call alert, or the
// answer to a radio check. 0 if the CSBK isn't one of those, or isn't addressed to us
int dmrDataBuildCSBKAnswer(const uint8_t *csbk, uint32_t src, dmrBurst_t *out);
// blockType: DT_RATE_12_DATA, DT_RATE_34_DATA or DT_RATE_1_DATA
int dmrDataBlockLength(uint8_t blockType);
int dmrDataBuildPacket(uint8_t dpf, uint8_t sap, bool group, uint32_t dst, uint32_t src,
						const uint8_t *data, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
// The bursts of an unconfirmed packet of length octets without preambles (header and blocks), 0 if it doesn't fit one
int dmrDataPacketBursts(int length, uint8_t blockType);
// count preamble CSBKs (Pre_CSBK, TS 102 361-2 7.1.2.5) announcing data, followed by `follow` more bursts: each one's
// CSBK Blocks to Follow is the preambles after it plus `follow` (7.2.7: the count leaves out the CSBK itself). For
// several packets in one transmission `follow` covers them all
int dmrDataBuildPreambles(bool group, uint32_t dst, uint32_t src, int count, int follow, dmrBurst_t *out);
// IPv4 addresses (srcIp, dstIp) are uint32_t in host byte order everywhere in the DMR data code: 12.0.39.21 is
// 0x0C002715. A packet's IP addresses are the Motorola CAI addresses of its DMR IDs (dmrDataIdAddress) unless a caller
// gives others: the full header (SAP 4) carries any address, so the IP destination may be an off-net host while the
// DMR destination is the radio that routes to it (TS 102 361-3 5.6)
// The CAI address of a DMR ID: 12.x.y.z for a radio, 225.x.y.z for a talkgroup (the ID's 24 bits). The compressed
// header (SAP 3) has no addresses: a receiver rebuilds these from the source and destination IDs
uint32_t dmrDataIdAddress(bool group, uint32_t id);
// On receive: true when an address is the ID's CAI address (dmrDataIdAddress), so a packet with it is for the ID
// itself, not for a host the ID routes to. Exact: an off-net address may have the same 24 bits under another first
// octet
bool dmrDataIpIsId(uint32_t ip, bool group, uint32_t id);
// ttl argument: the radio's own value, 64. A talkgroup always gets 1, whatever is asked: the air is the group's one
// hop, and no radio may route a group packet on (a receiving gateway delivers it to its link with TTL 1)
#define DMR_IP_TTL_DEFAULT			0
// UDP and IP packets go with the compressed UDP/IPv4 header (SAP 3) when the compression setting allows it for their
// ports (dmrDataUdpCompress), else with the full IPv4 header (SAP 4). Both carry the IDs' CAI addresses
int dmrDataBuildUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort,
						const uint8_t *payload, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
// An IPv4 packet of any protocol from layer 4 on (the header and its checksum included; a UDP checksum is redone)
int dmrDataBuildIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length,
						uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);

// --- Compressed UDP/IPv4 headers on transmit (TS 102 361-3 5.6, 7.2; CONFIG_DMR_UDP_COMPRESS) ---
// Which UDP packets we send compressed: none, those whose ports are both outside the DMR application ports
// 4000-4099 (MOTOTRBO TMS, ARS, LRRP and the like keep the full header their servers expect; the default), or all
#define DMR_UDPC_OFF				0
#define DMR_UDPC_AUTO				1
#define DMR_UDPC_ALL				2
#define DMR_APP_PORT_FIRST			4000
#define DMR_APP_PORT_LAST			4099
void dmrDataSetUdpCompression(uint8_t mode);
uint8_t dmrDataGetUdpCompression(void);
// True when a datagram between these ports may go compressed under the current mode (false when built without it).
// The mode decides by the ports only: a packet whose addresses aren't its IDs' CAI addresses still goes with the full
// header, whatever the mode, all included (dmrDataEncodeIP)
bool dmrDataUdpCompress(uint16_t srcPort, uint16_t dstPort);
// The compressed header (Table 7.14) into out: IPv4 identification, SAID 0 (radio network: the source is the source
// LLID), DAID 0 or 2 (group network), opcode 00, the port IDs (1 = 5016, 2 = 5017, 98 = 4007 as MOTOTRBO uses it) and
// the ports without an ID in extended headers 1 / 2. Returns its length: 5, 7 or 9
#define DMR_UDPC_HEADER_MAX			9
int dmrDataCompressUDPHeader(uint16_t ipId, bool group, uint16_t srcPort, uint16_t dstPort, uint8_t out[DMR_UDPC_HEADER_MAX]);
// The user data of an IP packet (layer 4 from l4) in the module's IP buffer, from DMR ID src to dst (group: a
// talkgroup) with the IP addresses srcIp and dstIp. With the compressed header (SAP 3) when compress is set, it is UDP
// with a consistent length, and both addresses are the IDs' CAI addresses (dmrDataIdAddress(false, src) and
// dmrDataIdAddress(group, dst): what a receiver rebuilds); else with the full IPv4 header (SAP 4) and the addresses as
// given. ttl: the IPv4 TTL, DMR_IP_TTL_DEFAULT for the radio's own. ipId: the IPv4 identification (a retry keeps it;
// dmrDataNextIpId() for a new packet). Returns the length (0: too long or malformed), *pdu points at it (valid until
// the next build) and *sap says which
int dmrDataEncodeIP(bool group, uint32_t dst, uint32_t src, uint32_t dstIp, uint32_t srcIp, uint8_t ttl, uint8_t protocol,
					const uint8_t *l4, int length, bool compress, uint16_t ipId, const uint8_t **pdu, uint8_t *sap);
uint16_t dmrDataNextIpId(void);
// What the compression saved, for the debug link (wrap freely)
typedef struct
{
	uint32_t sap3;							// IP packets built with the compressed header
	uint32_t sap4;							// ... with the full header (UDP and the rest)
	uint32_t octetsSaved;					// header octets the compressed ones didn't send
	uint32_t blocksSaved;					// data blocks they didn't send (at the rate they went at)
} dmrDataIpTxStats_t;
void dmrDataGetIpTxStats(dmrDataIpTxStats_t *stats);
void dmrDataResetIpTxStats(void);
// Count a packet sent: its user data (as dmrDataEncodeIP gave it) and the block type it goes in
void dmrDataCountIpTx(uint8_t sap, const uint8_t *pdu, int length, uint8_t blockType, bool confirmed);
int dmrDataBuildResponseAck(uint8_t sap, uint32_t dst, uint32_t src, uint8_t sendSeq, dmrBurst_t *out);
// The ACK class / type of a response packet (dmrDataPacket_t.response): class 0, type 1
#define DMR_RESPONSE_IS_ACK(r)		(((r) & 0xF8) == 0x08)
// Response class / type / status octets (TS 102 361-1 Table 8.3, -3 Table 5.7): status the N(S) answered
#define DMR_RESPONSE_ACK(ns)			(0x08 | ((ns) & 0x07))	// class 0, type 1
#define DMR_RESPONSE_NACK_CRC(ns)		(0x48 | ((ns) & 0x07))	// class 1, type 1: the packet CRC failed
#define DMR_RESPONSE_SACK(ns)			(0x80 | ((ns) & 0x07))	// class 2, type 0: the blocks flagged 0 again
#define DMR_RESPONSE_CLASS(r)			((r) >> 6)
#define DMR_RESPONSE_TYPE(r)			(((r) >> 3) & 0x07)
#define DMR_RESPONSE_NACK_SEQUENCE		5						// class 1 type 5: N(S) out of sequence, status = VI
// A response packet: the header, and for a selective ACK one rate 1/2 block with the 64 retry flags (flags[k / 8]
// bit k % 8 is block k: 1 received, 0 send it again) and their CRC-32 (Figure 8.17). flags NULL: header only
int dmrDataBuildResponse(uint8_t sap, uint32_t dst, uint32_t src, uint8_t response, const uint8_t flags[8], dmrBurst_t *out);

// Confirmed data (TS 102 361-1 8.2.1.2, 8.2.2.2, 9.2.1): data octets per block (rate 1/2 10, rate 3/4 16, rate 1 22:
// each block starts with the 7-bit serial number and the CRC-9), at most 64 blocks (one selective ACK block)
#define DMR_CONFIRMED_MAX_BLOCKS	64
int dmrDataConfirmedBlockData(uint8_t blockType);
// The CRC-9 of a confirmed block (B.3.10): over the block's data octets then the 7 serial number bits, inverted, not
// masked (the rate's mask: DMR_CRC9_MASK_xxx, B.21)
uint16_t dmrDataCRC9(const uint8_t *data, int length, uint8_t serial);
#define DMR_CRC9_MASK_RATE_12		0x0F0
#define DMR_CRC9_MASK_RATE_34		0x1FF
#define DMR_CRC9_MASK_RATE_1		0x10F
// A confirmed packet (DPF 3, individual, response requested) of the user data pdu: the header (N(S) ns, FSN single
// fragment, F set on the first try) and the blocks whose bits are set in blocks (bit k: block k; all of them on the
// first try), each with its serial number and CRC-9; the last data block ends with the CRC-32. The preambles count the
// header and the blocks. 0 if it doesn't fit
int dmrDataBuildConfirmed(uint8_t sap, uint32_t dst, uint32_t src, const uint8_t *pdu, int length, uint8_t blockType,
						uint8_t ns, bool firstTry, uint64_t blocks, int preambles, dmrBurst_t *out, int maxBursts);
// The number of blocks a confirmed packet of length octets takes (0: more than DMR_CONFIRMED_MAX_BLOCKS)
int dmrDataConfirmedBlocks(int length, uint8_t blockType);
#if CONFIG_DMR_TMS	// Motorola TMS format support (docs/spec/dmr-data.md 5.1)
int dmrDataBuildTMS(bool group, uint32_t dst, uint32_t src, const char *text, uint8_t seq, bool ackRequested,
						uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
// seq: the 7 bit sequence number of the message being acknowledged
int dmrDataBuildTMSAck(uint32_t dst, uint32_t src, uint8_t seq, int preambles, dmrBurst_t *out, int maxBursts);
#endif

// A position for the location reports, as NMEA gives it
typedef struct
{
	bool     fixValid;
	bool     north;
	bool     east;
	uint8_t  latDegrees;
	uint8_t  lonDegrees;
	uint32_t latMinutes10000;				// minutes in ten thousandths, 0 - 599999
	uint32_t lonMinutes10000;
	uint16_t speedKnots;					// DMR_LOCATION_SPEED_UNKNOWN when not known
	uint8_t  utcHours;
	uint8_t  utcMinutes;
	uint8_t  utcSeconds;
} dmrDataLocation_t;
#define DMR_LOCATION_SPEED_UNKNOWN		127

// ETSI TS 102 361-4 UDT location report: a UDT header (format 5, NMEA) and one Rate 1/2 block with the short NMEA
// format (80 bits: C, N/S, E/W, Q, speed in knots, latitude and longitude in degrees and minutes, UTC to ten
// seconds), its CRC-CCITT unmasked, after the preamble CSBKs (none straight after a call of ours: receivers are in sync)
#define DMR_UDT_FORMAT_NMEA				0x05
#define DMR_UDT_OPCODE					0x1A	// as rfoundry_sdrbench's builder (C_UDTHU)
int dmrDataBuildUDTLocation(bool group, uint32_t dst, uint32_t src, const dmrDataLocation_t *location, int preambles,
							dmrBurst_t *out, int maxBursts);

// ETSI short data text (defined short data, TS 102 361-1 9.2.12, DPF 13, SAP 10): the header carries the defined
// data format, rate 1/2 blocks with the CRC-32 follow. The DD formats used here (Table 9.50, docs/spec/dmr-data.md 5.2)
#define DMR_DD_BINARY					0x00
#define DMR_DD_BCD						0x01
#define DMR_DD_7BIT						0x02	// ISO/IEC 646, packed 7 bits per character
#define DMR_DD_ISO8859_1				0x03	// 0x03-0x11: ISO/IEC 8859-1 ... -16 (no -12)
#define DMR_DD_ISO8859_LAST				0x11
#define DMR_DD_UTF8						0x12
#define DMR_DD_UTF16					0x13	// with a byte order mark, big endian without one
#define DMR_DD_UTF16BE					0x14
#define DMR_DD_UTF16LE					0x15
// UDT formats that carry text (TS 102 361-4 UDT header), for reception
#define DMR_UDT_FORMAT_ISO7				0x03
#define DMR_UDT_FORMAT_ISO8				0x04
#define DMR_UDT_FORMAT_UTF16BE			0x07
#define DMR_ETSI_TEXT_MAX				140		// characters we send
// Text (Latin-1, NUL terminated) as defined short data in ISO 8859-1, with an ETSI response requested when
// responseRequested (individual only). 0 if it doesn't fit
int dmrDataBuildEtsiText(bool group, uint32_t dst, uint32_t src, const char *text, bool responseRequested,
						int preambles, dmrBurst_t *out, int maxBursts);
// The text of a received defined short data (any of the text DD formats above) or UDT text packet, as Latin-1 ('?'
// for characters outside it). False when the packet isn't one, or its format isn't text
bool dmrDataDecodeEtsiText(const dmrDataPacket_t *packet, char *text, int textSize);
// ETSI IP text (TS 102 361-3 5.7.1): Latin-1 text (NUL terminated, DMR_ETSI_TEXT_MAX characters at most) as UTF-16BE
// in UDP from and to DMR_UDP_PORT_ETSI_TEXT, full IPv4 header (SAP 4). 0 if it doesn't fit or the text is empty
// The same text as the UDP datagram (header, checksum left 0 for the IPv4 layer, then the payload) into l4: its
// length, 0 if the text is empty. The data service sends an individual one as confirmed data with it
#define DMR_ETSI_TEXT_UDP_MAX			(8 + (2 * DMR_ETSI_TEXT_MAX))
int dmrDataIPTextUDP(const char *text, uint8_t l4[DMR_ETSI_TEXT_UDP_MAX]);
int dmrDataBuildIPText(bool group, uint32_t dst, uint32_t src, const char *text, uint8_t blockType, int preambles,
						dmrBurst_t *out, int maxBursts);
// The text of an ETSI IP text datagram's payload (UTF-16BE; a byte order mark is honored) as Latin-1 ('?' outside
// it). False when nothing printable is in it
bool dmrDataDecodeIPText(const uint8_t *payload, int length, char *text, int textSize);

// RX
void dmrDataRxReset(void);
dmrDataRxResult_t dmrDataRxBurst(const dmrBurst_t *burst);
// After DMR_DATA_RX_RETRY: the selective ACK's 64 flags (1 received, 0 send again; unused ones 1)
void dmrDataRxRetryFlags(uint8_t flags[8]);
// A confirmed packet whose blocks stopped coming (some bursts were lost): ends its reassembly and returns
// DMR_DATA_RX_RETRY (or what the blocks so far give), DMR_DATA_RX_NONE when no confirmed packet is being received
dmrDataRxResult_t dmrDataRxConfirmedEnd(void);
bool dmrDataRxConfirmedActive(void);
// Confirmed blocks whose CRC-9 failed in packets the CRC-32 then passed (our CRC-9 disagreeing with a sender's)
uint32_t dmrDataRxCrc9Mismatches(void);
typedef struct
{
	uint16_t srcPort;
	uint16_t dstPort;
	uint16_t appPort;					// the well known one of the two (LRRP, ARS, TMS), else the destination port
	uint32_t srcIp;						// the IPv4 addresses (host byte order): SAP 4 from its header, SAP 3
	uint32_t dstIp;						// the IDs' CAI addresses (dmrDataIdAddress), as a receiver rebuilds them
	uint8_t ttl;						// SAP 4 its header's, SAP 3 the sender's default (64, a talkgroup 1)
	const uint8_t *payload;
	int length;
} dmrDataUDP_t;

bool dmrDataGetUDP(const dmrDataPacket_t *packet, dmrDataUDP_t *udp);

// Layer 4 of an IP based packet (SAP 4), whatever its protocol; false for anything else and for fragments
typedef struct
{
	uint8_t protocol;
	uint32_t srcIp;						// the header's IPv4 addresses (host byte order)
	uint32_t dstIp;
	uint8_t ttl;						// the header's TTL
	const uint8_t *payload;
	int length;
} dmrDataIP_t;

bool dmrDataGetIP(const dmrDataPacket_t *packet, dmrDataIP_t *ip);

// Decoded TMS text message, text is converted from UTF-16LE to Latin-1 ('?' for anything else)
typedef struct
{
	bool    isAck;
	bool    ackRequested;
	uint8_t seq;							// 7 bit sequence number
	char    text[160];
} dmrDataTMS_t;

#if CONFIG_DMR_TMS
bool dmrDataDecodeTMS(const uint8_t *payload, int length, dmrDataTMS_t *tms);
#endif

// Motorola ARS (Automatic Registration Service, UDP 4005): the radio registers with a presence server, the "ARS radio
// ID". PDU types (the low 4 bits of the header octet)
#define DMR_ARS_DEVICE_REGISTRATION		0x00
#define DMR_ARS_DEVICE_DEREGISTRATION	0x01
#define DMR_ARS_QUERY					0x04		// from the server: register again
#define DMR_ARS_RESPONSE				0x0F		// from the server: the registration's acknowledgment
#define DMR_ARS_MAX_PDU					16

// The UDP payloads, as MOTOTRBO sends them. Each returns its length (at most DMR_ARS_MAX_PDU)
// Registration: 00 LL F0 20 <n> <device ID: the radio ID in decimal, n digits> 00 00 (no user ID, no password)
int dmrDataBuildARSRegistration(uint32_t radioId, uint8_t *out);
// Deregistration: 00 01 31
int dmrDataBuildARSDeregistration(uint8_t *out);
// The PDU type of an ARS payload (DMR_ARS_xxx), or -1 when it isn't one
int dmrDataDecodeARS(const uint8_t *payload, int length);
// The server's answer to a registration (DMR_ARS_RESPONSE): accepted unless its ACK/NAK bit (0x40) is set, and the
// refresh time it asks for in minutes. The octet after the header (as in hbnet's 00 02 BF 01) counts 30 minute units,
// 0 meaning no refresh expected (ok-dmrlib's ResponseSecondHeader); DMR_ARS_REFRESH_NOT_GIVEN when there's no such octet
bool dmrDataDecodeARSResponse(const uint8_t *payload, int length, bool *accepted, uint16_t *refreshMinutes);
#define DMR_ARS_REFRESH_UNIT_MINUTES	30
#define DMR_ARS_REFRESH_NOT_GIVEN		0xFFFF

#endif
