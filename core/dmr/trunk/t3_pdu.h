// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III layer 3 PDUs (ETSI TS 102 361-4 7.1), written from the standard: the CSBKs and MBCs a mobile station
// receives on a trunked control or payload channel and the ones it sends, plus the C_SYS_Parms / P_SYS_Parms short
// LC (7.1.2). docs/spec/dmr-trunking.md.
//
// A CSBK is 12 octets as it leaves the BPTC: octets 0-9 the PDU, most significant bit of octet 0 first, octets 10-11
// the CRC-CCITT with the CSBK mask (TS 102 361-1 B.3.12). Octet 0 is LB, PF, CSBKO; octet 1 the FID; octets 2-9
// carry 64 bits of opcode-specific fields. An MBC is a header block (CRC masked 0xAAAA) and one continuation block
// whose CRC covers the continuation only, unmasked (TS 102 361-1 7.4).
//
// No hardware, no RTOS, no codeplug: the trunk engine and the host tests use these.
#ifndef RFOUNDRY_T3_PDU_H
#define RFOUNDRY_T3_PDU_H

#include <stdbool.h>
#include <stdint.h>

#define T3_CSBK_BYTES		12
#define T3_CRC_MASK_CSBK	0xA5A5u
#define T3_CRC_MASK_MBC_HDR	0xAAAAu
#define T3_CRC_MASK_DATA_HDR	0xCCCCu	// the UDT header (a data header)

// CSBK opcodes (Annex B.1)
#define T3_OP_C_ALOHA		25
#define T3_OP_C_UDTHD		26
#define T3_OP_C_DGNAHD		36	// the UDT header of a DGNA download (6.6.8.2.2)
#define T3_OP_C_UDTHU		27
#define T3_OP_C_AHOY		28	// P_AHOY on a payload channel
#define T3_OP_C_ACKVIT		30
#define T3_OP_C_RAND		31	// also the random access request on a payload channel
#define T3_OP_C_ACKD		32
#define T3_OP_C_ACKU		33
#define T3_OP_P_ACKD		34
#define T3_OP_P_ACKU		35
#define T3_OP_C_BCAST		40
#define T3_OP_P_MAINT		42
#define T3_OP_P_CLEAR		46
#define T3_OP_P_PROTECT		47
#define T3_OP_PV_GRANT		48
#define T3_OP_TV_GRANT		49
#define T3_OP_BTV_GRANT		50
#define T3_OP_PD_GRANT		51
#define T3_OP_TD_GRANT		52
#define T3_OP_PV_GRANT_DX	53
#define T3_OP_PD_GRANT_DX	54
#define T3_OP_PD_GRANT_MI	55
#define T3_OP_TD_GRANT_MI	56
#define T3_OP_C_MOVE		57

// Gateway and special addresses (A.4)
#define T3_ID_PSTNI		0xFFFEC0u	// PSTN, PABX, line and dispatcher gateways (aligned timing): dialed digits
#define T3_ID_PABXI		0xFFFEC1u
#define T3_ID_LINEI		0xFFFEC2u
#define T3_ID_IPI		0xFFFEC3u	// the IP gateway: packet data to an IP destination (multi-part)
#define T3_ID_SUPLI		0xFFFEC4u	// supplementary user data (6.4.13)
#define T3_ID_SDMI		0xFFFEC5u
#define T3_ID_REGI		0xFFFEC6u
#define T3_ID_MSI		0xFFFEC7u	// call diversion to an MS
#define T3_ID_DIVERTI		0xFFFEC9u	// call diversion: its address upload, canceling incoming diversions
#define T3_ID_TSI		0xFFFECAu
#define T3_ID_DISPATI		0xFFFECBu
#define T3_ID_PSTNDI		0xFFFED0u	// the same gateways with payload offset timing
#define T3_ID_DISPATDI		0xFFFED3u
#define T3_ID_STUNI		0xFFFECCu
#define T3_ID_AUTHI		0xFFFECDu
#define T3_ID_GPI		0xFFFECEu	// call diversion to a talkgroup
#define T3_ID_KILLI		0xFFFECFu
#define T3_ID_ALLMSI		0xFFFED4u
#define T3_ID_TATTSI		0xFFFED7u
#define T3_ID_ALLMSIDL		0xFFFFFDu
#define T3_ID_ALLMSIDZ		0xFFFFFEu
#define T3_ID_ALLMSID		0xFFFFFFu
#define T3_ID_GATEWAY_FIRST	0xFFFEC0u	// the gateway block (A.4) up to ALLMSID
#define T3_ID_NULL		0u		// ADRNULL
#define T3_ID_USER_MAX		0xFFFCDFu	// the last MS or talkgroup address (A.4)

// Logical physical channel numbers
#define T3_LPCN_NULL		0u
#define T3_LPCN_ABSOLUTE	0xFFFu		// an appended block (CG_AP / BC_AP / MV_AP) holds the frequencies
#define T3_LPCN_MAX		4094u

// C_BCAST announcement types (table 7.68)
#define T3_ANN_WD_TSCC		0
#define T3_ANN_CALL_TIMERS	1
#define T3_ANN_VOTE_NOW		2
#define T3_ANN_LOCAL_TIME	3
#define T3_ANN_MASS_REG		4
#define T3_ANN_CHAN_FREQ	5
#define T3_ANN_ADJACENT		6
#define T3_ANN_GEN_SITE		7

// Service_Kind (table 7.49)
#define T3_SK_IND_VOICE		0x0
#define T3_SK_TG_VOICE		0x1
#define T3_SK_IND_DATA		0x2
#define T3_SK_TG_DATA		0x3
#define T3_SK_IND_UDT		0x4
#define T3_SK_TG_UDT		0x5
#define T3_SK_UDT_POLL		0x6
#define T3_SK_STATUS		0x7
#define T3_SK_DIVERT		0x8
#define T3_SK_ANSWER		0x9
#define T3_SK_DX_VOICE		0xA
#define T3_SK_DX_DATA		0xB
#define T3_SK_SUPPLEMENTARY	0xD
#define T3_SK_REGISTRATION	0xE	// registration, authentication, de-registration, radio check
#define T3_SK_CANCEL		0xF

// Reason codes (tables 7.42 - 7.45)
#define T3_RC_MESSAGE_ACCEPTED	0x60
#define T3_RC_STORE_FORWARD	0x61
#define T3_RC_REG_ACCEPTED	0x62
#define T3_RC_STATUS_ACCEPTED	0x63
#define T3_RC_AUTH_RESPONSE	0x64
#define T3_RC_REG_SUBSCRIPTION	0x65	// Reg_Subscription/Attachment: registration accepted with a talkgroup
#define T3_RC_MS_ACCEPTED	0x44
#define T3_RC_MS_AUTH_RESPONSE	0x48	// the MS's answer to a challenge (table 7.42); the TS's is 0x64
#define T3_RC_CALLBACK		0x45
#define T3_RC_MS_ALERTING	0x46
#define T3_RC_MS_POLL_ACCEPTED	0x47	// Accepted for the Status Polling Service, from the MS: Response_Info its status
#define T3_RC_NOT_SUPPORTED	0x20
#define T3_RC_PERM_USER_REFUSED	0x21
#define T3_RC_TEMP_USER_REFUSED	0x22
#define T3_RC_TRANSIENT_REFUSED	0x23
#define T3_RC_NOREG_MS_AWAY	0x24
#define T3_RC_MS_AWAY		0x25
#define T3_RC_SYS_BUSY		0x27
#define T3_RC_SYS_NOT_READY	0x28
#define T3_RC_CANCEL_REFUSED	0x29
#define T3_RC_REG_REFUSED	0x2A
#define T3_RC_REG_DENIED	0x2B
#define T3_RC_IP_CONN_FAILED	0x2C	// IP_Connection_failed, from IPI: registered, the address not taken (6.4.11)
#define T3_RC_MS_NOT_REGISTERED	0x2D
#define T3_RC_CALLED_BUSY	0x2E
#define T3_RC_GROUP_NOT_ALLOWED	0x2F
#define T3_RC_REFUSED_UNKNOWN	0x3F
#define T3_RC_MS_NOT_SUPPORTED	0x00	// NACK sent by an MS
#define T3_RC_EQUIP_BUSY	0x13	// EquipBusy_Refused: the MS's data terminal isn't ready
#define T3_RC_MS_DUPLEX_NOT_SUPPORTED	0x16	// a duplex MS to MS availability check to a simplex MS
#define T3_RC_RECIPIENT_REFUSED	0x14
#define T3_RC_QUEUED_RESOURCE	0xA0
#define T3_RC_QUEUED_BUSY	0xA1
#define T3_RC_WAIT		0xE0

// The class of an acknowledgement reason code (tables 7.42 - 7.45)
typedef enum { T3_ACK, T3_NACK, T3_QACK, T3_WACK, T3_ACK_UNKNOWN } t3_ack_class_t;
t3_ack_class_t t3_ack_class(uint8_t reason);

// P_PROTECT kinds (table 7.82), P_MAINT kinds (table 7.83)
#define T3_PROTECT_DIS_PTT	0
#define T3_PROTECT_EN_PTT	1
#define T3_PROTECT_ILLEGALLY_PARKED	2
#define T3_PROTECT_EN_PTT_ONE_MS	3
#define T3_MAINT_DISCON		0

// Voice Service_Options (table 7.52): bit 6 emergency, 5 privacy, 4 supplementary data, 3 broadcast, 1-0 priority
#define T3_SO_EMERGENCY		0x40
// Call diversion Service_Options (table 6.61): DIVONOFF and the services it sets or clears
#define T3_SO_DIV_SET		0x10
#define T3_DIV_VOICE		0x08
#define T3_DIV_PDATA		0x04
#define T3_DIV_SDATA		0x02
#define T3_DIV_STATUS		0x01
#define T3_DIV_ALL		0x0F
#define T3_SO_PRIVACY		0x20
#define T3_SO_SUPPL_DATA	0x10
#define T3_SO_STATUS_SUPPL	0x20	// a status's SUPED_SV, above its 5 most significant bits (tables 6.57, 6.58)
#define T3_SO_BROADCAST		0x08
#define T3_SO_PRIORITY_MASK	0x03
// Registration Service_Options (table 7.55): bit 4 IP_Inform, 3-1 PowerSave_RQ, 0 Reg_Dereg (1 register)
#define T3_SO_REG_IP_INFORM	0x10
#define T3_SO_REG_REGISTER	0x01

// Trg_Adr_Cnts in the C_RAND registration request (table 7.94, the proxy flag and the 4 service-specific bits)
#define T3_TRG_SYSCODE		0
#define T3_TRG_TALKGROUP	1
#define T3_TRG_TATTSI		2

typedef struct {
	bool tsccas;
	bool site_ts_sync;
	uint8_t version;	// 3 bits (table 7.93)
	bool offset;		// TSCC offset timing
	bool active_conn;
	uint8_t mask;		// 0..24
	uint8_t service_fn;	// table 7.36
	uint8_t nrand_wait;	// 4 bits (table 7.37)
	bool reg;
	uint8_t backoff;	// 4 bits (table 7.39)
	uint16_t syscode;
	uint32_t ms;		// MS individual address the Mask applies to
} t3_aloha_t;

// PV/TV/BTV/PD/TD grants and P_GRANT (7.1.1.1.1, 7.1.1.3.1). Fields that a kind lacks read 0.
typedef struct {
	uint16_t lpcn;		// 12 bits
	uint8_t slot;		// 1 or 2
	bool late_entry;	// TV / BTV
	bool hi_rate;		// data grants
	bool emergency;
	bool offset;		// payload channel offset timing (always set for the duplex grants, tables 7.10, 7.14)
	bool dx_calling;	// duplex grants: the target (dst) is the calling MS, not the called (Call Direction)
	uint32_t dst;
	uint32_t src;
} t3_grant_t;

typedef struct {
	uint8_t type;		// T3_ANN_*
	uint16_t parms1;	// 14 bits
	bool reg;
	uint8_t backoff;
	uint16_t syscode;
	uint32_t parms2;	// 24 bits
} t3_bcast_t;

typedef struct {
	uint8_t options;	// Service_Options_Mirror, 7 bits
	bool kind_flag;		// Service_Kind_Flag
	bool als;
	bool group;		// G/I: the target is a talkgroup
	uint8_t blocks;		// Appended_Blocks, 2 bits
	uint8_t kind;		// Service_Kind
	uint32_t dst;
	uint32_t src;
} t3_ahoy_t;

// C_ACKD / C_ACKU / P_ACKD / P_ACKU: Response_Info (7 bits), the reason code, the target and the additional address
typedef struct {
	uint8_t info;
	uint8_t reason;
	uint32_t dst;
	uint32_t src;
} t3_ack_t;

typedef struct {
	uint8_t options;	// Service_Options, 7 bits
	bool proxy;
	uint8_t extra;		// the 4 service-dependent bits (Trg_Adr_Cnts and reserved for a registration)
	uint8_t kind;		// Service_Kind
	uint32_t dst;
	uint32_t src;
} t3_rand_t;

typedef struct {
	uint16_t lpcn;		// T3_LPCN_NULL: back to the last confirmed TSCC
	bool group;
	uint32_t dst;
	uint32_t src;
} t3_clear_t;

// C_MOVE (7.1.1.1.3, table 7.17): radios whose ID matches ms in its mask low bits go to another TSCC
typedef struct {
	uint8_t mask;
	bool reg;
	uint8_t backoff;
	uint16_t lpcn;		// T3_LPCN_ABSOLUTE: an MBC, the appended block gives the frequencies
	uint32_t ms;
} t3_move_t;

typedef struct {
	uint8_t kind;		// T3_PROTECT_*
	bool group;
	uint32_t dst;
	uint32_t src;
} t3_protect_t;

typedef struct {
	uint8_t kind;		// T3_MAINT_*
	uint32_t dst;
	uint32_t src;
} t3_maint_t;

typedef struct {
	bool lb;
	bool pf;
	uint8_t op;		// CSBKO
	uint8_t fid;		// 0 = standard; any other value: only raw[] is meaningful
	union {
		t3_aloha_t aloha;
		t3_grant_t grant;
		t3_bcast_t bcast;
		t3_ahoy_t ahoy;
		t3_ack_t ack;
		t3_rand_t rand;
		t3_clear_t clear;
		t3_move_t move;
		t3_protect_t protect;
		t3_maint_t maint;
	} u;
	uint8_t raw[8];		// octets 2-9 as received
} t3_csbk_t;

// Appended block of an MBC (CG_AP, BC_AP, MV_AP: 7.1.1.1.2, 7.1.1.1.5.1): CdefParms type 0 (table 7.103)
typedef struct {
	uint8_t cc;		// color code (CG_AP, MV_AP); 0 for BC_AP
	uint8_t cdeftype;
	uint16_t chan;		// the logical channel the frequencies belong to
	uint32_t tx_hz;		// the MS's transmit frequency (Annex C.1.1.4: the MS absolute frequencies)
	uint32_t rx_hz;		// the MS's receive frequency
} t3_cdef_t;

// CRC-CCITT of octets 0-9 with a mask into octets 10-11, and its check
void t3_crc_append(uint8_t pdu[T3_CSBK_BYTES], uint16_t mask);
bool t3_crc_ok(const uint8_t pdu[T3_CSBK_BYTES], uint16_t mask);

// Parse a single-block CSBK or an MBC header (octets 0-9; the CRC is not checked here). False for an opcode this
// module doesn't decode (out->op, fid and raw are still filled)
bool t3_csbk_parse(const uint8_t pdu[T3_CSBK_BYTES], t3_csbk_t *out);
// Pack octets 0-9 from out and append the CRC with mask (T3_CRC_MASK_CSBK, or T3_CRC_MASK_MBC_HDR with lb false).
// False for an opcode this module doesn't encode
bool t3_csbk_build(const t3_csbk_t *in, uint16_t mask, uint8_t pdu[T3_CSBK_BYTES]);

// MBC continuation (last) block holding CdefParms. The CRC over octets 0-9 is unmasked.
bool t3_cdef_parse(const uint8_t blk[T3_CSBK_BYTES], uint8_t op, t3_cdef_t *out);
void t3_cdef_build(const t3_cdef_t *in, uint8_t op, uint8_t blk[T3_CSBK_BYTES]);
// Absolute frequency of CdefParms (Annex C.1.1.4): MHz and 125 Hz steps
uint32_t t3_cdef_hz(uint16_t mhz, uint16_t khz125);

// C_SYS_Parms (SLCO 2) / P_SYS_Parms (SLCO 3), 7.1.2: the 28-bit short LC as dmrCach.h holds it
#define T3_SLCO_C_SYS_PARMS	0x2
#define T3_SLCO_P_SYS_PARMS	0x3
typedef struct {
	bool control;		// C_SYS_Parms (the TSCC's physical channel) or P_SYS_Parms
	uint8_t model;		// 2 bits
	uint16_t netsite;	// 12 bits: NET and SITE as the model partitions them
	bool flag;		// C: Reg; P: composite control channel
	uint16_t csc;		// Common_Slot_Counter, 9 bits
} t3_sysparms_t;
bool t3_sysparms_parse(uint32_t slc, t3_sysparms_t *out);
uint32_t t3_sysparms_build(const t3_sysparms_t *in);

// --- Unified Data Transport (6.5; TS 102 361-1 8.2.1.8, 8.2.2.5) ---
// A header (data type: data header, CRC mask 0xCCCC) and 1-4 rate 1/2 blocks of 12 octets sent in consecutive slots.
// The blocks hold the data, then pad octets, then a CRC-CCITT (unmasked) over all the octets before it

#define T3_UDT_BLOCK		12
#define T3_UDT_BLOCKS_MAX	4
#define T3_UDT_MAX		(T3_UDT_BLOCKS_MAX * T3_UDT_BLOCK - 2)	// 46 octets
#define T3_UDT_ISO7_MAX		52	// 7-bit symbols in 46 octets (table B.4)

// UDT_Format (7.2.27, table 7.88)
#define T3_UDTF_BINARY		0x0
#define T3_UDTF_ADDRESS		0x1
#define T3_UDTF_BCD		0x2
#define T3_UDTF_ISO7		0x3
#define T3_UDTF_ISO8		0x4
#define T3_UDTF_NMEA		0x5
#define T3_UDTF_IP		0x6
#define T3_UDTF_UTF16		0x7
#define T3_UDTF_DGNA_ALIAS	0xA	// DGNA_Alias mode: the mixed format (B.3.9)

typedef struct {
	uint8_t op;		// T3_OP_C_UDTHD, T3_OP_C_DGNAHD (outbound) or T3_OP_C_UDTHU (inbound)
	bool group;		// G/I: to a talkgroup
	bool a;			// a response is demanded
	bool emergency;		// outbound only (inbound: reserved)
	bool opt;		// outbound: UDT_Option_Flag; inbound: UDT_DIV
	uint8_t format;		// T3_UDTF_*
	uint32_t dst, src;
	uint8_t pad;		// Pad Nibble: 4-bit pad nibbles after the data
	uint8_t blocks;		// appended blocks, 1-4
	bool sf;		// supplementary data (else the data of a user's service)
} t3_udt_t;

// The header with its CRC; parse checks the CRC, DPF and SAP 0 and the opcode
void t3_udt_build(const t3_udt_t *h, uint8_t out[T3_CSBK_BYTES]);
bool t3_udt_parse(const uint8_t in[T3_CSBK_BYTES], t3_udt_t *h);
// The blocks for len (1-46) octets of data, padded with fill: their number (0: too long) and the Pad Nibble value
uint8_t t3_udt_pack(const uint8_t *data, uint8_t len, uint8_t fill, uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK],
		    uint8_t *pad_nibbles);
bool t3_udt_crc_ok(const uint8_t blk[][T3_UDT_BLOCK], uint8_t n);

#endif
